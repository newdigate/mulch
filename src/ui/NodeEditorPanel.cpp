#include "ui/NodeEditorPanel.h"
#include "ui/PortWidgets.h"
#include "app/Application.h"   // for nodeCategories()
#include <imgui.h>
#include <imgui_node_editor.h>
#include <set>
#include <string>
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

struct NodeEditorPanel::Impl {
    ed::EditorContext* ctx = nullptr;
    std::set<int> placed;
};

NodeEditorPanel::NodeEditorPanel() : impl_(std::make_unique<Impl>()) {
    ed::Config cfg;
    cfg.SettingsFile = nullptr;   // don't write a NodeEditor.json
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
        for (std::size_t i = 0; i < n.inputs().size(); ++i) {
            ed::BeginPin(ed::PinId(pinId(n.id(), (int)i, false)), ed::PinKind::Input);
            ImGui::Text("-> %s", n.inputs()[i].name.c_str());
            ed::EndPin();
            if (!graph.isInputConnected(n.id(), (int)i)) {
                ImGui::SameLine();
                drawInlineInputWidget(n, i);
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

    // Background context menu: add a node of a chosen type.
    ed::Suspend();
    if (ed::ShowBackgroundContextMenu()) ImGui::OpenPopup("AddNode");
    if (ImGui::BeginPopup("AddNode")) {
        ImVec2 mouse = ImGui::GetMousePosOnOpeningCurrentPopup();
        ImVec2 canvas = ed::ScreenToCanvas(mouse);
        // One submenu per category (Texture / Audio / MIDI / 3D) so the list
        // stays readable as nodes are added.
        for (auto& cat : nodeCategories()) {
            if (ImGui::BeginMenu(cat.name.c_str())) {
                for (auto& type : cat.types) {
                    if (ImGui::MenuItem(type.c_str()))
                        addNodeOfType(type, glm::vec2(canvas.x, canvas.y));
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }
    ed::Resume();

    ed::End();
    ed::SetCurrentEditor(nullptr);
    ImGui::End();
}

} // namespace oss
