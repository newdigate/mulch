#pragma once
#include <map>

namespace oss {

class Graph;

// Draws the "Automation" window as collapsible groups over one shared,
// horizontally-scrollable time axis (global length from the graph's
// AutomationStore): one stream group per AutomationNode (its 4 channel lanes) and
// one ui group per module that has ui-automation channels (one lane per channel).
// The mouse edits a lane's breakpoint curve; ui lanes can also be deleted. Call
// inside an active ImGui frame. Holds per-group collapse state and the in-progress
// drag, so it's an Application member.
class AutomationPanel {
public:
    void draw(Graph& graph);

private:
    bool isOpen(long key) const {
        auto it = open_.find(key);
        return it == open_.end() ? true : it->second;   // groups default to open
    }

    std::map<long, bool> open_;       // group collapse state, keyed by header key
    long dragLane_  = -1;             // stable key of the lane being edited (-1 = none)
    int  dragPoint_ = -1;             // index of the dragged point in that lane
    float zoomX_ = 1.0f;              // horizontal zoom (scales px-per-bar)
    float zoomY_ = 1.0f;              // vertical zoom (scales lane height)
};

} // namespace oss
