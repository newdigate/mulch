#pragma once
#include "core/Graph.h"

namespace oss {

// The "Properties" window: field-style inputs (integer/colour/text/dropdown/checkbox), the node's
// toggle/preset buttons, and a read-only connections list, for the selected node.
class PropertiesPanel {
public:
    void draw(Graph& graph, int selectedNodeId, bool* open);
};

} // namespace oss
