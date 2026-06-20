#pragma once
#include <cmath>
#include <glm/vec3.hpp>

namespace oss {

// One cube-map sample: which face a direction hits, and the [0,1] UV on that face.
struct CubeSample { int face; float u; float v; };

// OpenGL cube-map major-axis face selection. face 0..5 = +X,-X,+Y,-Y,+Z,-Z. The shader
// (shaders/skybox.frag) mirrors this exactly; keep them in sync.
inline CubeSample cubeFaceUV(const glm::vec3& d) {
    float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    int face; float sc, tc, ma;
    if (ax >= ay && ax >= az) {
        ma = ax;
        if (d.x > 0.0f) { face = 0; sc = -d.z; tc = -d.y; }
        else            { face = 1; sc =  d.z; tc = -d.y; }
    } else if (ay >= az) {
        ma = ay;
        if (d.y > 0.0f) { face = 2; sc =  d.x; tc =  d.z; }
        else            { face = 3; sc =  d.x; tc = -d.z; }
    } else {
        ma = az;
        if (d.z > 0.0f) { face = 4; sc =  d.x; tc = -d.y; }
        else            { face = 5; sc = -d.x; tc = -d.y; }
    }
    float inv = ma > 1e-8f ? 0.5f / ma : 0.0f;
    return { face, sc * inv + 0.5f, tc * inv + 0.5f };
}

} // namespace oss
