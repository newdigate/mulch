#pragma once
#include <cstddef>
#include <vector>

namespace oss {

// Expand an indexed triangle mesh into a flat GL_LINES vertex array: each
// triangle contributes its three edges (6 vertices). `positions` is tightly-
// packed vec3 (x,y,z per vertex); `indices` is triangle vertex indices in
// groups of 3. Returns tightly-packed vec3 line-segment endpoints. Triangles
// that reference an out-of-range index are skipped; trailing partial triangles
// are ignored. GL-free -- pure geometry, so it is unit-tested directly.
inline std::vector<float> trianglesToLineList(const std::vector<float>& positions,
                                              const std::vector<unsigned int>& indices) {
    std::vector<float> out;
    out.reserve(indices.size() * 2 * 3);
    const std::size_t vertCount = positions.size() / 3;
    auto emit = [&](unsigned int v) {
        out.push_back(positions[(std::size_t)v * 3 + 0]);
        out.push_back(positions[(std::size_t)v * 3 + 1]);
        out.push_back(positions[(std::size_t)v * 3 + 2]);
    };
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        unsigned int a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (a >= vertCount || b >= vertCount || c >= vertCount) continue;
        emit(a); emit(b);   // edge a-b
        emit(b); emit(c);   // edge b-c
        emit(c); emit(a);   // edge c-a
    }
    return out;
}

} // namespace oss
