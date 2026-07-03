#pragma once
#include "core/Graph.h"

namespace oss {

// The "Controls" window: continuous sliders + tri-state grids for the selected node.
class ControlsPanel {
public:
    void draw(Graph& graph, int selectedNodeId, bool* open);
};

} // namespace oss
