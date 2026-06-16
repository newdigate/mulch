#pragma once

namespace oss {

class Graph;

// Draws the "Automation" window: a toolbar (channel select, song length in bars,
// the active channel's output range, clear) and a horizontally-scrollable timeline
// where the mouse edits breakpoints for the first AutomationNode in the graph.
// Call inside an active ImGui frame. Holds the small editing state (active channel
// + in-progress drag), so it's a member of the Application.
class AutomationPanel {
public:
    void draw(Graph& graph);

private:
    int activeChannel_ = 0;
    int dragPoint_     = -1;   // index into the active channel during a drag
};

} // namespace oss
