#pragma once

namespace oss {

class Graph;

// Draws the "Automation" window as a grid: a song-length toolbar, then one row per
// channel (with a reserved top row) over a shared, horizontally-scrollable time
// axis. Each row's left header carries the channel's category, output range, and a
// clear button; the mouse edits that channel's breakpoint curve in its lane. Edits
// the first AutomationNode in the graph. Call inside an active ImGui frame. Holds
// the in-progress drag (which channel + which point), so it's an Application member.
class AutomationPanel {
public:
    void draw(Graph& graph);

private:
    int dragChannel_ = -1;   // channel being edited during a drag
    int dragPoint_   = -1;   // index of the point being dragged in that channel
};

} // namespace oss
