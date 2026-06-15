#pragma once
#include <cstddef>
#include "core/Node.h"

namespace oss {
// Render an inline editor for an unconnected input port `i` of `node`,
// editing that port's default value in place. No-op for Texture/Audio ports.
void drawInlineInputWidget(Node& node, std::size_t i);
}
