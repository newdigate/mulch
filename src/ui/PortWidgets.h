#pragma once
#include <cstddef>
#include "core/Node.h"

namespace oss {
// Render an inline editor for an unconnected input port `i` of `node`,
// editing that port's default value in place. No-op for Texture/Audio ports.
// Returns true if this frame the port is a choice (dropdown) whose button was
// clicked -- the caller opens the dropdown popup OUTSIDE the node (in the editor's
// Suspend/Resume block), since ImGui popups opened inside a node mis-position.
bool drawInlineInputWidget(Node& node, std::size_t i);
}
