#pragma once
#include <cmath>

namespace oss {

// One evaluation of the filter yields all three band taps from the same state.
struct SvfOut { float low, band, high; };

// 2-pole topology-preserving-transform state-variable filter (Zavalishin/Cytomic).
// Coefficients are set per control-block from cutoff (Hz) + resonance (0..1); process()
// runs per audio sample. Integrator state persists across calls (and thus across frames).
// GL-free. Unconditionally stable for all cutoff/resonance in range.
struct StateVariableFilter {
    float ic1 = 0.0f, ic2 = 0.0f;                               // integrator memory
    float g = 0.0f, k = 2.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;  // derived coefficients

    void reset() { ic1 = ic2 = 0.0f; }

    // fc in Hz (clamped to [20, 0.45*sr]); res in [0,1] (clamped). res maps to the
    // damping k = 1/Q: res 0 -> k=2 (Q=0.5, gentle), res 1 -> k~=0.02 (Q~=50, sharp peak).
    void setCoefficients(float fc, float res, int sr) {
        float ny = 0.45f * (float)sr;
        fc  = fc  < 20.0f ? 20.0f : (fc  > ny   ? ny   : fc);
        res = res < 0.0f  ? 0.0f  : (res > 1.0f ? 1.0f : res);
        g  = std::tan(3.14159265f * fc / (float)sr);
        k  = 2.0f - 1.98f * res;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    SvfOut process(float in) {
        float v3 = in - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return { v2, v1, in - k * v1 - v2 };   // low, band, high
    }
};

} // namespace oss
