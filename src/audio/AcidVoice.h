#pragma once
#include <cmath>

namespace oss {

// Compact 4-pole (24 dB/oct) resonant low-pass -- the classic "simplified Moog"
// ladder (Stilson/Smith). Stateful; GL-free. res in [0,1] self-oscillates near 1.
struct LadderFilter {
    float s1 = 0, s2 = 0, s3 = 0, s4 = 0;   // stage outputs
    float d1 = 0, d2 = 0, d3 = 0, d4 = 0;   // one-sample delays
    void reset() { s1 = s2 = s3 = s4 = d1 = d2 = d3 = d4 = 0.0f; }

    float process(float in, float cutoffHz, float res, int sr) {
        float fc = cutoffHz / (0.5f * (float)sr);
        if (fc < 0.0f) fc = 0.0f;
        if (fc > 0.99f) fc = 0.99f;
        float f  = fc * 1.16f;
        float fb = res * 4.0f * (1.0f - 0.15f * f * f);
        float x  = in - s4 * fb;
        x *= 0.35013f * (f * f) * (f * f);
        s1 = x  + 0.3f * d1 + (1.0f - f) * s1;  d1 = x;
        s2 = s1 + 0.3f * d2 + (1.0f - f) * s2;  d2 = s1;
        s3 = s2 + 0.3f * d3 + (1.0f - f) * s3;  d3 = s2;
        s4 = s3 + 0.3f * d4 + (1.0f - f) * s4;  d4 = s3;
        return s4;
    }
};

} // namespace oss
