#include "ui/PortWidgets.h"
#include <imgui.h>
#include <glm/vec4.hpp>
#include <variant>
#include <string>
#include <cstdio>

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
            ImGui::SliderFloat("##f", &std::get<float>(v), 0.0f, 1.0f);
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
