#include "ui/ControlsPanel.h"
#include "core/PanelSlot.h"
#include <imgui.h>
#include <cfloat>
#include <variant>
#include <string>

namespace oss {

void ControlsPanel::draw(Graph& graph, int selectedNodeId, bool* open) {
    if (!open || !*open) return;
    if (!ImGui::Begin("Controls", open)) { ImGui::End(); return; }

    Node* n = graph.findNode(selectedNodeId);
    if (!n) { ImGui::TextDisabled("Select a node"); ImGui::End(); return; }

    ImGui::TextUnformatted(n->name().c_str());
    ImGui::Separator();

    bool any = false;
    for (std::size_t i = 0; i < n->inputs().size(); ++i) {
        const Port& p = n->inputs()[i];
        if (inputSlot(p) != PanelSlot::Controls) continue;
        any = true;
        Value& v = n->inputDefault(i);
        ImGui::PushID((int)i);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat(p.name.c_str(), &std::get<float>(v), p.minVal, p.maxVal);
        ImGui::PopID();
    }

    int grows = n->gridRows(), gcols = n->gridCols();
    if (grows > 0 && gcols > 0) {
        any = true;
        for (int r = 0; r < grows; ++r) {
            std::string rl = n->gridRowLabel(r);
            if (!rl.empty()) { ImGui::TextUnformatted(rl.c_str()); ImGui::SameLine(); }
            for (int c = 0; c < gcols; ++c) {
                if (c) ImGui::SameLine();
                int st = n->gridCell(r, c);
                ImU32 col = st == 2 ? IM_COL32(250, 210,  70, 255)
                          : st == 1 ? IM_COL32(120, 150, 200, 255)
                                    : IM_COL32( 45,  48,  56, 255);
                ImGui::PushStyleColor(ImGuiCol_Button, col);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
                std::string id = "##g" + std::to_string(r) + "_" + std::to_string(c);
                if (ImGui::Button(id.c_str(), ImVec2(16, 16))) n->onGridCellPressed(r, c);
                ImGui::PopStyleColor(3);
            }
        }
    }

    if (!any) ImGui::TextDisabled("(no controls)");
    ImGui::End();
}

} // namespace oss
