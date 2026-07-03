#pragma once
#include "core/Port.h"

namespace oss {

// Which panel edits a given input's widget.
enum class PanelSlot { Controls, Properties, None };

// Continuous Float sliders -> Controls; integer/choice Floats, Bool, Colour, String -> Properties;
// non-control ports (Texture/Audio/Midi/Vertex/Transform/Shader) have no widget -> None (they show
// only in the Properties connections list). GL-free.
inline PanelSlot inputSlot(const Port& p) {
    switch (p.type) {
        case PortType::Float:
            if (!p.choices.empty() || p.integer) return PanelSlot::Properties;  // dropdown / int field
            return PanelSlot::Controls;                                          // continuous slider
        case PortType::Bool:
        case PortType::Colour:
        case PortType::String:
            return PanelSlot::Properties;
        default:
            return PanelSlot::None;
    }
}

} // namespace oss
