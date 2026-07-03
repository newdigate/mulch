#include "ui/PropertiesPanel.h"
#include "core/PanelSlot.h"
#include "core/AssetTree.h"     // uniqueAssetFolders
#include <imgui.h>
#include <glm/vec4.hpp>
#include <variant>
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace oss {

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

void PropertiesPanel::draw(Graph& graph, int selectedNodeId, bool* open) {
    if (!open || !*open) return;
    if (!ImGui::Begin("Properties", open)) { ImGui::End(); return; }

    Node* n = graph.findNode(selectedNodeId);
    if (!n) { ImGui::TextDisabled("Select a node"); ImGui::End(); return; }

    ImGui::TextUnformatted(n->name().c_str());
    ImGui::Separator();

    for (std::size_t i = 0; i < n->inputs().size(); ++i) {
        const Port& p = n->inputs()[i];
        if (inputSlot(p) != PanelSlot::Properties) continue;
        Value& v = n->inputDefault(i);
        ImGui::PushID((int)i);
        ImGui::SetNextItemWidth(180.0f);

        if (p.type == PortType::Colour) {
            auto& c = std::get<glm::vec4>(v);
            ImGui::ColorEdit4(p.name.c_str(), &c.x, ImGuiColorEditFlags_AlphaBar);
        } else if (p.type == PortType::Bool) {
            ImGui::Checkbox(p.name.c_str(), &std::get<bool>(v));
        } else if (p.type == PortType::Float && !p.choices.empty()) {
            int idx = std::clamp((int)std::lround(std::get<float>(v)), 0, (int)p.choices.size() - 1);
            if (ImGui::BeginCombo(p.name.c_str(), p.choices[idx].c_str())) {
                for (int k = 0; k < (int)p.choices.size(); ++k)
                    if (ImGui::Selectable(p.choices[k].c_str(), k == idx)) std::get<float>(v) = (float)k;
                ImGui::EndCombo();
            }
        } else if (p.type == PortType::Float && p.integer) {
            int iv = (int)std::lround(std::get<float>(v));
            if (ImGui::DragInt(p.name.c_str(), &iv, 1.0f, (int)p.minVal, (int)p.maxVal))
                std::get<float>(v) = (float)iv;
        } else if (p.type == PortType::String) {
            auto& s = std::get<std::string>(v);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", s.c_str());
            if (p.assetBacked) {
                ImGui::SetNextItemWidth(180.0f - ImGui::GetFrameHeight() - 4.0f);
                if (ImGui::InputText("##s", buf, sizeof(buf))) s = buf;
                ImGui::SameLine(0.0f, 2.0f);
                if (ImGui::ArrowButton("##pick", ImGuiDir_Down)) ImGui::OpenPopup("pick");
                ImGui::SameLine(); ImGui::TextUnformatted(p.name.c_str());
                if (ImGui::BeginPopup("pick")) {
                    if (p.folderPicker) {
                        std::vector<std::string> folders = uniqueAssetFolders(graph.assets().byType(p.assetType));
                        if (folders.empty()) ImGui::TextDisabled("No %s folders", assetTypeName(p.assetType));
                        for (const std::string& f : folders)
                            if (ImGui::Selectable(f.c_str(), f == s)) { s = f; ImGui::CloseCurrentPopup(); }
                    } else {
                        std::vector<const Asset*> assets = graph.assets().byType(p.assetType);
                        if (assets.empty()) ImGui::TextDisabled("No %s assets", assetTypeName(p.assetType));
                        for (const Asset* a : assets) {
                            std::string label = a->label.empty() ? a->path : a->label;
                            if (label.empty()) label = "(unnamed)";
                            ImGui::PushID(a->id);
                            if (ImGui::Selectable(label.c_str(), a->path == s)) { s = a->path; ImGui::CloseCurrentPopup(); }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndPopup();
                }
            } else {
                if (ImGui::InputText(p.name.c_str(), buf, sizeof(buf))) s = buf;
            }
        }
        ImGui::PopID();
    }

    // Toggle / preset buttons (the node's button-bank hook).
    int nbtn = n->buttonCount();
    if (nbtn > 0) {
        ImGui::Separator();
        int bAct = n->buttonActive(), bPend = n->buttonPending();
        for (int b = 0; b < nbtn; ++b) {
            if (b) ImGui::SameLine();
            int pushed = 0;
            if (b == bAct)       { ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 130, 200, 255)); pushed = 1; }
            else if (b == bPend) { ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70,  90, 110, 255)); pushed = 1; }
            if (ImGui::SmallButton((n->buttonLabel(b) + "##btn" + std::to_string(b)).c_str()))
                n->onButtonPressed(b);
            if (pushed) ImGui::PopStyleColor(pushed);
        }
    }

    // Connections (read-only).
    ImGui::Separator();
    ImGui::TextUnformatted("Connections");
    NodeConnections cc = nodeConnectionSummary(graph, selectedNodeId);
    auto label = [&](int nodeId) -> const char* {
        Node* o = graph.findNode(nodeId);
        return o ? o->name().c_str() : "?";
    };
    if (cc.inputs.empty() && cc.outputs.empty()) {
        ImGui::TextDisabled("(none)");
    } else {
        for (const auto& l : cc.inputs)
            ImGui::BulletText("%s <- %s : out %d", n->inputs()[(std::size_t)l.port].name.c_str(),
                              label(l.otherNode), l.otherPort);
        for (const auto& l : cc.outputs)
            ImGui::BulletText("%s -> %s : in %d", n->outputs()[(std::size_t)l.port].name.c_str(),
                              label(l.otherNode), l.otherPort);
    }

    ImGui::End();
}

} // namespace oss
