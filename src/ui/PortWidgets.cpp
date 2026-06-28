#include "ui/PortWidgets.h"
#include <imgui.h>
#include <glm/vec4.hpp>
#include <variant>
#include <string>
#include <cstdio>
#include <algorithm>
#include <cmath>

namespace oss {

bool drawInlineInputWidget(Node& node, std::size_t i) {
    const Port& port = node.inputs()[i];
    Value& v = node.inputDefault(i);
    bool popupClicked = false;
    ImGui::PushID((int)i);
    ImGui::PushItemWidth(120.0f);
    switch (port.type) {
        case PortType::Colour: {
            // The colour swatch is a button; clicking it asks the caller to open the
            // colour picker in the editor's Suspend/Resume block. ColorEdit4's built-in
            // picker popup, opened here inside the node, would render in canvas
            // coordinates -- off-screen and unusable, like the choice dropdown.
            auto& c = std::get<glm::vec4>(v);
            if (ImGui::ColorButton("##c", ImVec4(c.x, c.y, c.z, c.w),
                    ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(120.0f, 0.0f)))
                popupClicked = true;
            break;
        }
        case PortType::Float: {
            if (!port.choices.empty()) {
                // A choice port: show the current label as a button. Clicking it asks
                // the caller to open the dropdown in the editor's Suspend/Resume block --
                // a BeginCombo popup opened here (inside the node) would render in canvas
                // coordinates, landing off-screen and unclickable. "###" keeps the
                // button's ImGui id stable as the displayed label changes.
                int idx = (int)std::lround(std::get<float>(v));
                idx = std::clamp(idx, 0, (int)port.choices.size() - 1);
                std::string label = port.choices[idx] + "###choice";
                if (ImGui::Button(label.c_str(), ImVec2(120.0f, 0.0f))) popupClicked = true;
            } else if (port.integer) {
                // A whole-number slider (easier to drag than a fine float slider); the value
                // is still stored as a float.
                int iv = (int)std::lround(std::get<float>(v));
                if (ImGui::SliderInt("##i", &iv, (int)port.minVal, (int)port.maxVal))
                    std::get<float>(v) = (float)iv;
            } else {
                ImGui::SliderFloat("##f", &std::get<float>(v), port.minVal, port.maxVal);
            }
            break;
        }
        case PortType::Bool: {
            ImGui::Checkbox("##b", &std::get<bool>(v));
            break;
        }
        case PortType::String: {
            auto& s = std::get<std::string>(v);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", s.c_str());
            if (port.assetBacked) {
                // Editable path + a down-arrow that opens the library picker. The popup
                // must be opened in the editor's Suspend block (screen space), so just
                // signal the click here -- like the choice/colour buttons do.
                ImGui::SetNextItemWidth(120.0f - ImGui::GetFrameHeight() - 2.0f);   // 120 (node width) - arrow - spacing
                if (ImGui::InputText("##s", buf, sizeof(buf))) s = buf;
                ImGui::SameLine(0.0f, 2.0f);
                if (ImGui::ArrowButton("##assetpick", ImGuiDir_Down)) popupClicked = true;
            } else {
                if (ImGui::InputText("##s", buf, sizeof(buf))) s = buf;
            }
            break;
        }
        default: break; // Texture / Audio: no inline widget
    }
    ImGui::PopItemWidth();
    ImGui::PopID();
    return popupClicked;
}

} // namespace oss
