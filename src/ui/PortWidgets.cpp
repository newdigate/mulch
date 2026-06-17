#include "ui/PortWidgets.h"
#include <imgui.h>
#include <glm/vec4.hpp>
#include <variant>
#include <string>
#include <cstdio>
#include <algorithm>
#include <cmath>

namespace oss {

void drawInlineInputWidget(Node& node, std::size_t i) {
    const Port& port = node.inputs()[i];
    Value& v = node.inputDefault(i);
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
                // A choice port: a dropdown whose value is the selected index.
                int idx = (int)std::lround(std::get<float>(v));
                idx = std::clamp(idx, 0, (int)port.choices.size() - 1);
                if (ImGui::BeginCombo("##choice", port.choices[idx].c_str())) {
                    for (int k = 0; k < (int)port.choices.size(); ++k) {
                        bool sel = (k == idx);
                        if (ImGui::Selectable(port.choices[k].c_str(), sel)) v = Value((float)k);
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
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
}

} // namespace oss
