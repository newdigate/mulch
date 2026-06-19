#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace oss {

// Ordered blend-mode labels for the Compositor's `mode` dropdown. The index is the mode
// id used by both blendPixel() below and the shader's switch -- keep all three in sync.
inline const std::vector<std::string>& blendModeLabels() {
    static const std::vector<std::string> labels = {
        "Normal", "Add", "Subtract", "Difference", "Exclusion",      // 0..4
        "Multiply", "Screen", "Overlay", "Darken", "Lighten",        // 5..9
        "Color Dodge", "Color Burn", "Hard Light", "Soft Light", "Divide",  // 10..14
        "Average", "Hue", "Saturation", "Color", "Luminosity",       // 15..19
        "AND", "OR", "XOR"                                           // 20..22
    };
    return labels;
}

namespace blend_detail {

inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// Separable (per-channel) blend for ids 0..15. Result is clamped by the caller.
inline float channel(int id, float a, float b) {
    switch (id) {
        case 1:  return a + b;                                        // Add
        case 2:  return a - b;                                        // Subtract
        case 3:  return std::fabs(a - b);                             // Difference
        case 4:  return a + b - 2.0f * a * b;                         // Exclusion
        case 5:  return a * b;                                        // Multiply
        case 6:  return 1.0f - (1.0f - a) * (1.0f - b);               // Screen
        case 7:  return a < 0.5f ? 2.0f*a*b : 1.0f - 2.0f*(1.0f-a)*(1.0f-b);   // Overlay
        case 8:  return std::min(a, b);                               // Darken
        case 9:  return std::max(a, b);                               // Lighten
        case 10: return b >= 1.0f ? 1.0f : std::min(1.0f, a / (1.0f - b));         // Color Dodge
        case 11: return b <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - a) / b);  // Color Burn
        case 12: return b < 0.5f ? 2.0f*a*b : 1.0f - 2.0f*(1.0f-a)*(1.0f-b);   // Hard Light
        case 13: return (1.0f - 2.0f*b)*a*a + 2.0f*b*a;               // Soft Light (Pegtop)
        case 14: return b <= 0.0f ? 1.0f : std::min(1.0f, a / b);     // Divide
        case 15: return 0.5f * (a + b);                              // Average
        default: return b;                                           // Normal (0)
    }
}

// Rec.601 luma weights (0.3/0.59/0.11) -- the values the PDF/SVG non-separable blend
// spec uses for Hue/Saturation/Color/Luminosity (not CSS Color 4's Rec.709). They sum
// to exactly 1.0f, so a grey's luma is exactly the grey value. The GLSL mirror uses the
// same weights.
inline float lum(const glm::vec3& c) { return 0.3f*c.x + 0.59f*c.y + 0.11f*c.z; }

inline glm::vec3 clipColor(glm::vec3 c) {
    float L = lum(c);
    float n = std::min(c.x, std::min(c.y, c.z));
    float x = std::max(c.x, std::max(c.y, c.z));
    if (n < 0.0f) c = glm::vec3(L) + (c - glm::vec3(L)) * (L / (L - n));
    if (x > 1.0f) c = glm::vec3(L) + (c - glm::vec3(L)) * ((1.0f - L) / (x - L));
    return c;
}
inline glm::vec3 setLum(const glm::vec3& c, float l) { return clipColor(c + glm::vec3(l - lum(c))); }
inline float sat(const glm::vec3& c) {
    return std::max(c.x, std::max(c.y, c.z)) - std::min(c.x, std::min(c.y, c.z));
}
// Index-based SetSat -- written to match the GLSL line-for-line (no pointer sort).
inline glm::vec3 setSat(const glm::vec3& c, float s) {
    int mni = 0, mxi = 0;
    if (c[1] < c[mni]) mni = 1;  if (c[2] < c[mni]) mni = 2;
    if (c[1] > c[mxi]) mxi = 1;  if (c[2] > c[mxi]) mxi = 2;
    int mdi = 3 - mni - mxi;
    glm::vec3 o(0.0f);
    if (c[mxi] > c[mni]) {
        o[mdi] = (c[mdi] - c[mni]) * s / (c[mxi] - c[mni]);
        o[mxi] = s;
    }
    return o;   // o[mni] stays 0
}

inline int q8(float x) { return (int)std::lround(clamp01(x) * 255.0f); }

} // namespace blend_detail

// Reference blend of two RGB colours in [0,1] under mode id (clamped to a valid id).
// This is the source of truth the shader mirrors; result is clamped to [0,1].
inline glm::vec3 blendPixel(int id, const glm::vec3& base, const glm::vec3& blend) {
    using namespace blend_detail;
    int n = (int)blendModeLabels().size();
    if (id < 0) id = 0; if (id >= n) id = n - 1;

    glm::vec3 o;
    if (id <= 15) {
        o = glm::vec3(channel(id, base.x, blend.x),
                      channel(id, base.y, blend.y),
                      channel(id, base.z, blend.z));
    } else if (id == 16) { o = setLum(setSat(blend, sat(base)), lum(base)); }   // Hue
    else if (id == 17)   { o = setLum(setSat(base, sat(blend)), lum(base)); }   // Saturation
    else if (id == 18)   { o = setLum(blend, lum(base)); }                      // Color
    else if (id == 19)   { o = setLum(base, lum(blend)); }                      // Luminosity
    else {                                                                      // 20 AND/21 OR/22 XOR
        auto bw = [id](float a, float b) {
            int ia = q8(a), ib = q8(b);
            int r = (id == 20) ? (ia & ib) : (id == 21) ? (ia | ib) : (ia ^ ib);
            return (float)r / 255.0f;
        };
        o = glm::vec3(bw(base.x, blend.x), bw(base.y, blend.y), bw(base.z, blend.z));
    }
    return glm::vec3(clamp01(o.x), clamp01(o.y), clamp01(o.z));
}

} // namespace oss
