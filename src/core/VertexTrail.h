#pragma once
#include <deque>
#include <vector>
#include "core/Value.h"   // VertexFormat, Primitive

namespace oss {

// A FIFO queue of geometry snapshots for the Vertex Trail node. GL-free. Each snapshot is
// normalised to flat Pos3Color3 floats (x,y,z,r,g,b per vertex). build() concatenates the
// queue, offsetting each snapshot in Z and rotating its hue by its age (0 = newest).
class VertexTrail {
public:
    // Capture a snapshot of `count` vertices in `fmt` layout (Pos3 = 3 floats/vertex,
    // Pos3Normal3 / Pos3Color3 = 6). A Pos3Color3 input keeps its colour; a colourless input
    // gets a red base (hue 0). Newest goes to the front; prunes beyond maxFrames.
    void push(const float* verts, int count, VertexFormat fmt, Primitive prim);
    void setMaxFrames(int n);          // clamp to >= 1 and prune the oldest beyond n
    // Concatenate all snapshots as flat Pos3Color3 floats. For age k (0 = newest):
    //   pos.z += k * zSpacing;  colour hue rotated by k * hueRate.
    // Returns the total vertex count (out.size() == returned * 6).
    int build(float zSpacing, float hueRate, std::vector<float>& out) const;
    int frameCount() const { return (int)snaps_.size(); }
    // True iff every snapshot has the same vertex count (so the trail can be drawn as equal strips).
    bool uniformCount() const {
        if (snaps_.empty()) return false;
        for (const auto& s : snaps_) if (s.count != snaps_.front().count) return false;
        return true;
    }
    // The output primitive: a uniform LineStrip trail stays LineStrip (drawn as stripCount() separate
    // strips via multi-draw); a variable-count LineStrip falls back to independent segments (Lines);
    // Lines/Triangles inputs pass through unchanged.
    Primitive primitive() const {
        if (snaps_.empty()) return Primitive::Lines;
        Primitive in = snaps_.front().prim;
        if (in != Primitive::LineStrip) return in;
        return uniformCount() ? Primitive::LineStrip : Primitive::Lines;
    }
    // Number of equal-length strips the output buffer should be drawn as: the frame count for a
    // uniform LineStrip trail, else 1 (a single primitive).
    int stripCount() const {
        return (!snaps_.empty() && snaps_.front().prim == Primitive::LineStrip && uniformCount())
                   ? (int)snaps_.size() : 1;
    }

private:
    struct Snapshot { std::vector<float> pc; int count; Primitive prim; };   // pc = 6 floats/vertex
    std::deque<Snapshot> snaps_;       // front = newest (age 0)
    int maxFrames_ = 16;
};

} // namespace oss
