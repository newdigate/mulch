#pragma once
#include <cmath>
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

// Expand an indexed triangle mesh into a flat, interleaved vertex array of
// position + normal (6 floats per vertex, 18 per triangle), for shaded
// GL_TRIANGLES rendering. If `normals` is supplied (per-vertex, parallel to
// `positions`), each vertex uses its own normal -> smooth shading; where it is
// absent or degenerate (or `normals` is empty) a flat per-face normal,
// normalize((b-a) x (c-a)), is used instead. Bad/partial triangles are skipped.
// GL-free.
inline std::vector<float> trianglesToShadedList(const std::vector<float>& positions,
                                                const std::vector<unsigned int>& indices,
                                                const std::vector<float>& normals = {}) {
    std::vector<float> out;
    out.reserve(indices.size() * 6);
    const std::size_t vertCount = positions.size() / 3;
    const bool haveNormals = normals.size() == positions.size();
    auto P = [&](unsigned int v, int k) { return positions[(std::size_t)v * 3 + k]; };
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        unsigned int a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (a >= vertCount || b >= vertCount || c >= vertCount) continue;
        float ux = P(b,0)-P(a,0), uy = P(b,1)-P(a,1), uz = P(b,2)-P(a,2);
        float vx = P(c,0)-P(a,0), vy = P(c,1)-P(a,1), vz = P(c,2)-P(a,2);
        float fnx = uy*vz - uz*vy, fny = uz*vx - ux*vz, fnz = ux*vy - uy*vx;
        float fl = std::sqrt(fnx*fnx + fny*fny + fnz*fnz);
        if (fl > 1e-12f) { fnx /= fl; fny /= fl; fnz /= fl; } else { fnx = 0; fny = 0; fnz = 1; }
        for (unsigned int v : {a, b, c}) {
            float nx = fnx, ny = fny, nz = fnz;
            if (haveNormals) {
                float gx = normals[(std::size_t)v*3+0], gy = normals[(std::size_t)v*3+1], gz = normals[(std::size_t)v*3+2];
                if (gx*gx + gy*gy + gz*gz > 1e-12f) { nx = gx; ny = gy; nz = gz; }  // else keep face normal
            }
            out.push_back(P(v,0)); out.push_back(P(v,1)); out.push_back(P(v,2));
            out.push_back(nx);     out.push_back(ny);     out.push_back(nz);
        }
    }
    return out;
}

} // namespace oss
