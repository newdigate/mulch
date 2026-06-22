#include "core/VertexTrail.h"
#include "core/ColorHsv.h"
#include <glm/vec3.hpp>
#include <cstddef>
#include <utility>

namespace oss {

void VertexTrail::push(const float* verts, int count, VertexFormat fmt, Primitive prim) {
    Snapshot s;
    s.count = count;
    s.prim  = prim;
    s.pc.resize((std::size_t)count * 6);
    int  fl       = (fmt == VertexFormat::Pos3) ? 3 : 6;
    bool hasColor = (fmt == VertexFormat::Pos3Color3);
    for (int i = 0; i < count; ++i) {
        const float* v = verts + (std::size_t)i * fl;
        float*       o = s.pc.data() + (std::size_t)i * 6;
        o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
        if (hasColor) { o[3] = v[3]; o[4] = v[4]; o[5] = v[5]; }
        else          { o[3] = 1.0f; o[4] = 0.0f; o[5] = 0.0f; }   // red base (hue 0)
    }
    snaps_.push_front(std::move(s));
    while ((int)snaps_.size() > maxFrames_) snaps_.pop_back();
}

void VertexTrail::setMaxFrames(int n) {
    maxFrames_ = n < 1 ? 1 : n;
    while ((int)snaps_.size() > maxFrames_) snaps_.pop_back();
}

int VertexTrail::build(float zSpacing, float hueRate, std::vector<float>& out) const {
    out.clear();
    // Expand strips into independent segments ONLY when their vertex counts differ (so the trail
    // can't be drawn as equal strips). The uniform case stays compact (1 vertex per point) and is
    // drawn as stripCount() separate strips by the renderer -- restoring the pre-multi-draw cost.
    const bool expand = !snaps_.empty()
                     && snaps_.front().prim == Primitive::LineStrip && !uniformCount();
    int total = 0, k = 0;
    for (const Snapshot& s : snaps_) {        // front (k=0) = newest
        float zoff = (float)k * zSpacing;
        float hoff = (float)k * hueRate;
        auto emit = [&](int i) {
            const float* v = s.pc.data() + (std::size_t)i * 6;
            glm::vec3 hsv = rgbToHsv(v[3], v[4], v[5]);
            glm::vec3 rgb = hsvToRgb(hsv.x + hoff, hsv.y, hsv.z);
            out.push_back(v[0]);
            out.push_back(v[1]);
            out.push_back(v[2] + zoff);
            out.push_back(rgb.x);
            out.push_back(rgb.y);
            out.push_back(rgb.z);
            ++total;
        };
        if (expand) {
            for (int i = 0; i + 1 < s.count; ++i) { emit(i); emit(i + 1); }   // independent segments
        } else {
            for (int i = 0; i < s.count; ++i) emit(i);                        // compact (1 vert/point)
        }
        ++k;
    }
    return total;
}

} // namespace oss
