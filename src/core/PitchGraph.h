#pragma once
#include <vector>
#include <glm/vec3.hpp>
#include "core/Value.h"

namespace oss {

// HSV (each component in [0,1]; hue wraps) -> RGB in [0,1]. GL-free.
glm::vec3 hsvToRgb(float h, float s, float v);

// Rolling MIDI note history -> a scrolling pitch-vs-time line graph (GL-free). Notes are
// horizontal segments at their pitch; pitch class picks a rainbow hue, velocity the
// brightness. X is time (newest at the right edge, scrolling left).
class PitchGraph {
public:
    // Advance the local clock by dt, ingest this frame's MIDI (note-on opens a record,
    // note-off closes the matching open one), and prune records scrolled off the left.
    void ingest(const MidiRef& midi, float dt, float window);

    // Emit the line-segment vertices as flat Pos3Color3 floats (x,y,z,r,g,b per vertex,
    // 2 vertices per segment). Returns the vertex count (out.size() == count*6).
    int build(float window, std::vector<float>& out) const;

    int activeCount() const;   // records currently held (for the status line)

private:
    struct Note { int note; int vel; double startT; double endT; };  // endT < 0 while held
    std::vector<Note> notes_;
    double clock_ = 0.0;
};

} // namespace oss
