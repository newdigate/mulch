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
    // The output primitive: a LineStrip input is emitted as independent segments (Lines), so stacked
    // trail copies don't visually join; Lines/Triangles inputs are passed through unchanged.
    Primitive primitive() const {
        if (snaps_.empty()) return Primitive::Lines;
        Primitive in = snaps_.front().prim;
        return in == Primitive::LineStrip ? Primitive::Lines : in;
    }

private:
    struct Snapshot { std::vector<float> pc; int count; Primitive prim; };   // pc = 6 floats/vertex
    std::deque<Snapshot> snaps_;       // front = newest (age 0)
    int maxFrames_ = 16;
};

} // namespace oss
