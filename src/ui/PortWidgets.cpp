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
    bool choiceClicked = false;
    ImGui::PushID((int)i);
    ImGui::PushItemWidth(120.0f);
    switch (port.type) {
        case PortType::Colour: {
            auto& c = std::get<glm::vec4>(v);
            ImGui::ColorEdit4("##c", &c.x,
                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
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
                if (ImGui::Button(label.c_str(), ImVec2(120.0f, 0.0f))) choiceClicked = true;
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
            if (ImGui::InputText("##s", buf, sizeof(buf))) s = buf;
            break;
        }
        default: break; // Texture / Audio: no inline widget
    }
    ImGui::PopItemWidth();
    ImGui::PopID();
    return choiceClicked;
}

} // namespace oss
