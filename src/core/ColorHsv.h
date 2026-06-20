#pragma once
#include <algorithm>
#include <cmath>
#include <glm/vec3.hpp>

namespace oss {

// HSV (each component in [0,1]; hue wraps) -> RGB in [0,1]. GL-free.
inline glm::vec3 hsvToRgb(float h, float s, float v) {
    h = h - std::floor(h);                 // wrap to [0,1)
    float i = std::floor(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    switch (((int)i) % 6) {
        case 0:  return glm::vec3(v, t, p);
        case 1:  return glm::vec3(q, v, p);
        case 2:  return glm::vec3(p, v, t);
        case 3:  return glm::vec3(p, q, v);
        case 4:  return glm::vec3(t, p, v);
        default: return glm::vec3(v, p, q);
    }
}

// RGB in [0,1] -> HSV (each in [0,1]; hue wraps). Inverse of hsvToRgb. GL-free.
inline glm::vec3 rgbToHsv(float r, float g, float b) {
    float mx = std::max(r, std::max(g, b));
    float mn = std::min(r, std::min(g, b));
    float d  = mx - mn;
    float h  = 0.0f;
    if (d > 1e-6f) {
        if      (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (mx == g) h = (b - r) / d + 2.0f;
        else              h = (r - g) / d + 4.0f;
        h /= 6.0f;
    }
    float s = mx > 1e-6f ? d / mx : 0.0f;
    return glm::vec3(h, s, mx);
}

} // namespace oss
