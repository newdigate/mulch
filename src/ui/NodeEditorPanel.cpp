#include "ui/NodeEditorPanel.h"
#include "ui/PortWidgets.h"
#include "app/Application.h"   // for nodeCategories()
#include "core/AssetLibrary.h"
#include <imgui.h>
#include <imgui_node_editor.h>
#include <glm/vec4.hpp>
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <variant>
#include <vector>
#include <utility>

namespace ed = ax::NodeEditor;

namespace oss {

static int pinId(int nodeId, int port, bool isOutput) {
    return nodeId * 1000 + port * 2 + (isOutput ? 1 : 0);
}
static void decodePin(int pin, int& nodeId, int& port, bool& isOutput) {
    nodeId = pin / 1000;
    int rest = pin % 1000;
    port = rest / 2;
    isOutput = (rest % 2) == 1;
}

static const char* assetTypeName(AssetType t) {
    switch (t) {
        case AssetType::Audio: return "Audio";
        case AssetType::Video: return "Video";
        case AssetType::Midi:  return "MIDI";
        case AssetType::Mesh:  return "3D";
        case AssetType::Image: return "Image";
    }
    return "media";
}

struct NodeEditorPanel::Impl {
    ed::EditorContext* ctx = nullptr;
    std::set<int> placed;
    int ctxNodeId = 0;   // node whose context menu is open

    // Deferred in-node popup (a choice dropdown or a colour picker). A port's inline
    // button (drawn inside a node) records a request here; the popup is opened/drawn in
    // screen space inside the editor's Suspend/Resume block, dispatched by port type.
    // `pending*` is set the frame a button is clicked; `open*` holds the port whose
    // popup is currently open.
    int    pendingPopupNode = -1, pendingPopupPort = -1;
    ImVec2 pendingPopupPos{0.0f, 0.0f};
    int    openPopupNode = -1, openPopupPort = -1;
    ImVec2 openPopupPos{0.0f, 0.0f};
};

NodeEditorPanel::NodeEditorPanel() : impl_(std::make_unique<Impl>()) {
    ed::Config cfg;
    cfg.SettingsFile = nullptr;       // don't write a NodeEditor.json
    cfg.NavigateButtonIndex = 2;      // pan the canvas with a middle-mouse drag (default was right)
    cfg.EnableSmoothZoom = true;      // continuous wheel/trackpad zoom; the default integer-step
                                      // mode rounds fractional trackpad deltas to 0 (near-unusable
                                      // zoom on macOS). SmoothZoomPower defaults to a Mac-tuned 1.1.
    impl_->ctx = ed::CreateEditor(&cfg);
}

NodeEditorPanel::~NodeEditorPanel() {
    if (impl_->ctx) ed::DestroyEditor(impl_->ctx);
}

void NodeEditorPanel::draw(Graph& graph,
        const std::function<int(const std::string&, glm::vec2)>& addNodeOfType) {
    ImGui::Begin("Node Graph");
    // Capture focus while "Node Graph" is the current window. Used below to gate
    // the Backspace-to-delete shortcut so it only fires when the graph window
    // (or its canvas child) is focused -- matching the editor's built-in Delete
    // key, which requires the canvas to be focused + hovered.
    const bool windowFocused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ed::SetCurrentEditor(impl_->ctx);
    ed::Begin("graph");

    for (auto& up : graph.nodes()) {
        Node& n = *up;
        ed::BeginNode(ed::NodeId(n.id()));
        // imgui-node-editor doesn't scope ImGui IDs per node, so push the node's
        // id to keep inline-widget IDs unique across nodes -- otherwise a widget
        // at the same port index on two nodes (e.g. a Float at index 1) collides.
        ImGui::PushID(n.id());
        ImGui::TextUnformatted(n.name().c_str());
        std::string status = n.statusLine();
        if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
        int nbtn = n.buttonCount();
        if (nbtn > 0) {
            int bAct = n.buttonActive(), bPend = n.buttonPending();
            for (int b = 0; b < nbtn; ++b) {
                if (b) ImGui::SameLine();
                int pushed = 0;
                if (b == bAct)        { ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 130, 200, 255)); pushed = 1; }
                else if (b == bPend)  { ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70,  90, 110, 255)); pushed = 1; }
                if (ImGui::SmallButton((n.buttonLabel(b) + "##btn" + std::to_string(b)).c_str()))
                    n.onButtonPressed(b);
                if (pushed) ImGui::PopStyleColor(pushed);
            }
        }
        int grows = n.gridRows(), gcols = n.gridCols();
        if (grows > 0 && gcols > 0) {
            for (int r = 0; r < grows; ++r) {
                std::string rl = n.gridRowLabel(r);
                if (!rl.empty()) { ImGui::TextUnformatted(rl.c_str()); ImGui::SameLine(); }
                for (int c = 0; c < gcols; ++c) {
                    if (c) ImGui::SameLine();
                    int st = n.gridCell(r, c);
                    ImU32 col = st == 2 ? IM_COL32(250, 210,  70, 255)    // accent: brightest
                              : st == 1 ? IM_COL32(120, 150, 200, 255)    // on
                                        : IM_COL32( 45,  48,  56, 255);   // off: dark
                    ImGui::PushStyleColor(ImGuiCol_Button, col);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
                    std::string id = "##g" + std::to_string(r) + "_" + std::to_string(c);
                    if (ImGui::Button(id.c_str(), ImVec2(14, 14))) n.onGridCellPressed(r, c);
                    ImGui::PopStyleColor(3);
                }
            }
        }
        for (std::size_t i = 0; i < n.inputs().size(); ++i) {
            ed::BeginPin(ed::PinId(pinId(n.id(), (int)i, false)), ed::PinKind::Input);
            ImGui::Text("-> %s", n.inputs()[i].name.c_str());
            ed::EndPin();
            if (!graph.isInputConnected(n.id(), (int)i)) {
                ImGui::SameLine();
                if (drawInlineInputWidget(n, i)) {
                    // A popup-backed control (choice / colour) was clicked; request its
                    // popup (opened below in the Suspend block, anchored at the click in
                    // screen space).
                    impl_->pendingPopupNode = n.id();
                    impl_->pendingPopupPort = (int)i;
                    impl_->pendingPopupPos  = ImGui::GetMousePos();
                }
            }
        }
        for (std::size_t i = 0; i < n.outputs().size(); ++i) {
            ed::BeginPin(ed::PinId(pinId(n.id(), (int)i, true)), ed::PinKind::Output);
            ImGui::Text("%s ->", n.outputs()[i].name.c_str());
            ed::EndPin();
        }
        ImGui::PopID();
        ed::EndNode();

        // Place each node at its model position the first time it is seen; on
        // later frames read the position back so user drags persist into n.pos.
        // `placed` only grows with total nodes ever created (ids are monotonic
        // and never reused), so stale entries for deleted nodes are harmless.
        if (impl_->placed.find(n.id()) == impl_->placed.end()) {
            ed::SetNodePosition(ed::NodeId(n.id()), ImVec2(n.pos.x, n.pos.y));
            impl_->placed.insert(n.id());
        } else {
            ImVec2 p = ed::GetNodePosition(ed::NodeId(n.id()));
            n.pos = glm::vec2(p.x, p.y);
        }
    }

    const auto& conns = graph.connections();
    for (std::size_t li = 0; li < conns.size(); ++li) {
        const Connection& c = conns[li];
        ed::Link(ed::LinkId((int)li + 1),
                 ed::PinId(pinId(c.srcNode, c.srcPort, true)),
                 ed::PinId(pinId(c.dstNode, c.dstPort, false)));
    }

    // Create links.
    if (ed::BeginCreate()) {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b) {
            int an, ap; bool aout; decodePin((int)a.Get(), an, ap, aout);
            int bn, bp; bool bout; decodePin((int)b.Get(), bn, bp, bout);
            if (aout == bout) {
                ed::RejectNewItem();   // both inputs or both outputs
            } else {
                int sn, sp, dn, dp;
                if (aout) { sn = an; sp = ap; dn = bn; dp = bp; }
                else      { sn = bn; sp = bp; dn = an; dp = ap; }
                // connect() self-rejects bad type / cycle / already-connected.
                if (ed::AcceptNewItem()) graph.connect(sn, sp, dn, dp);
            }
        }
    }
    ed::EndCreate();

    // Backspace also deletes the current selection. The editor only binds the
    // Delete key internally (DeleteItemsAction::Accept), so translate a
    // Backspace press into ed::DeleteNode/DeleteLink. Those queue the items so
    // they surface through QueryDeleted*() on the next frame and reuse the
    // graph-mutation code below -- no duplicate delete logic. Gated to match
    // Delete: the graph window must be focused and no text field may be
    // capturing input (WantTextInput), else Backspace in a filename field
    // would delete the selected node instead of editing the text.
    if (windowFocused && !ImGui::GetIO().WantTextInput &&
        ed::AreShortcutsEnabled() &&
        ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        int count = ed::GetSelectedObjectCount();
        if (count > 0) {
            std::vector<ed::NodeId> selNodes(count);
            std::vector<ed::LinkId> selLinks(count);
            int nNodes = ed::GetSelectedNodes(selNodes.data(), count);
            int nLinks = ed::GetSelectedLinks(selLinks.data(), count);
            for (int i = 0; i < nNodes; ++i) ed::DeleteNode(selNodes[i]);
            for (int i = 0; i < nLinks; ++i) ed::DeleteLink(selLinks[i]);
        }
    }

    // Delete links / nodes. Resolve each deleted LinkId to a stable
    // (dstNode, dstPort) key BEFORE mutating: disconnect() erases from
    // connections_ and shifts later indices, so deleting 2+ links in one
    // batch by live index would drop or mis-target edges. (Inputs are
    // single-connection, so the key uniquely identifies the edge.)
    if (ed::BeginDelete()) {
        std::vector<std::pair<int, int>> toDisconnect;   // (dstNode, dstPort)
        ed::LinkId lid;
        while (ed::QueryDeletedLink(&lid)) {
            if (ed::AcceptDeletedItem()) {
                int idx = (int)lid.Get() - 1;
                if (idx >= 0 && idx < (int)graph.connections().size()) {
                    const Connection& c = graph.connections()[idx];
                    toDisconnect.emplace_back(c.dstNode, c.dstPort);
                }
            }
        }
        for (auto& [dn, dp] : toDisconnect) graph.disconnect(dn, dp);

        ed::NodeId nid;
        while (ed::QueryDeletedNode(&nid)) {
            if (ed::AcceptDeletedItem()) graph.removeNode((int)nid.Get());
        }
    }
    ed::EndDelete();

    // Background context menu: Add (a node of a chosen type) / View.
    ed::Suspend();
    if (ed::ShowBackgroundContextMenu()) ImGui::OpenPopup("BackgroundMenu");
    if (ImGui::BeginPopup("BackgroundMenu")) {
        ImVec2 mouse = ImGui::GetMousePosOnOpeningCurrentPopup();
        ImVec2 canvas = ed::ScreenToCanvas(mouse);
        if (ImGui::BeginMenu("Add")) {
            // One submenu per category (Texture / Audio / MIDI / 3D) so the
            // list stays readable as nodes are added.
            for (auto& cat : nodeCategories()) {
                if (ImGui::BeginMenu(cat.name.c_str())) {
                    for (auto& type : cat.types) {
                        if (ImGui::MenuItem(type.c_str()))
                            addNodeOfType(type, glm::vec2(canvas.x, canvas.y));
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            // Reset zoom/pan by fitting all nodes in view -- the way back when a
            // stray scroll has zoomed the canvas too far in or out to recover.
            if (ImGui::MenuItem("Reset View (fit all)")) ed::NavigateToContent();
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    // Node context menu: right-click a node to automate one of its Float inputs
    // as a UI-automation channel (created/removed in the graph's AutomationStore;
    // shown grouped under the node in the Automation window).
    ed::NodeId ctxNode;
    if (ed::ShowNodeContextMenu(&ctxNode)) {
        impl_->ctxNodeId = (int)ctxNode.Get();
        ImGui::OpenPopup("NodeMenu");
    }
    if (ImGui::BeginPopup("NodeMenu")) {
        Node* n = graph.findNode(impl_->ctxNodeId);
        if (n) {
            ImGui::TextDisabled("Automate (UI)");
            ImGui::Separator();
            bool any = false;
            for (std::size_t i = 0; i < n->inputs().size(); ++i) {
                if (n->inputs()[i].type != PortType::Float) continue;
                any = true;
                bool on = graph.automation().find(n->id(), (int)i) != nullptr;
                if (ImGui::MenuItem(n->inputs()[i].name.c_str(), nullptr, on)) {
                    if (on) graph.automation().remove(n->id(), (int)i);
                    else    graph.automation().add(graph, n->id(), (int)i);
                }
            }
            if (!any) ImGui::TextDisabled("No automatable parameters");
        }
        ImGui::EndPopup();
    }

    // In-node popup (deferred from a node's inline choice button or colour swatch).
    // Opened and drawn here, inside Suspend, so the popup uses screen coordinates and is
    // clickable -- a popup opened inside the node lands in canvas space, off-screen and
    // unusable. Dispatched by the port's type.
    if (impl_->pendingPopupNode >= 0) {
        impl_->openPopupNode = impl_->pendingPopupNode;
        impl_->openPopupPort = impl_->pendingPopupPort;
        impl_->openPopupPos  = impl_->pendingPopupPos;
        impl_->pendingPopupNode = -1;
        ImGui::OpenPopup("NodePopup");
    }
    ImGui::SetNextWindowPos(impl_->openPopupPos, ImGuiCond_Appearing);
    if (ImGui::BeginPopup("NodePopup")) {
        Node* n = graph.findNode(impl_->openPopupNode);
        if (n && impl_->openPopupPort >= 0 && impl_->openPopupPort < (int)n->inputs().size()) {
            const Port& port = n->inputs()[(std::size_t)impl_->openPopupPort];
            Value& v = n->inputDefault((std::size_t)impl_->openPopupPort);
            if (port.type == PortType::Colour) {
                auto& c = std::get<glm::vec4>(v);
                ImGui::ColorPicker4("##picker", &c.x, ImGuiColorEditFlags_AlphaBar);
            } else if (port.type == PortType::String && port.assetBacked) {
                // Pick a library asset of this type; copy its path into the field.
                std::vector<const Asset*> assets = graph.assets().byType(port.assetType);
                if (assets.empty()) {
                    ImGui::TextDisabled("No %s assets -- add them in the Assets window",
                                        assetTypeName(port.assetType));
                } else {
                    const std::string cur = std::get<std::string>(v);
                    for (const Asset* a : assets) {
                        ImGui::PushID(a->id);
                        std::string label = a->label.empty() ? a->path : a->label;
                        if (label.empty()) label = "(unnamed)";
                        // Checkmark by path (copy-path model: the field stores a path, not an
                        // id), so a path match -- not asset identity -- marks the current row.
                        if (ImGui::Selectable(label.c_str(), a->path == cur)) {
                            v = Value(a->path);
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::PopID();
                    }
                }
            } else {   // a Float choice port: a list of its labels
                int idx = std::clamp((int)std::lround(std::get<float>(v)),
                                     0, (int)port.choices.size() - 1);
                for (int k = 0; k < (int)port.choices.size(); ++k) {
                    if (ImGui::Selectable(port.choices[k].c_str(), k == idx)) {
                        v = Value((float)k);
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
        } else {
            ImGui::CloseCurrentPopup();   // node went away
        }
        ImGui::EndPopup();
    }
    ed::Resume();

    ed::End();
    ed::SetCurrentEditor(nullptr);
    ImGui::End();
}

} // namespace oss
