#include "core/Oscilloscope.h"
#include <algorithm>
#include <cmath>

namespace oss {
namespace {

// Linear interpolation of a channel at fractional position p in [0, n-1].
float sampleAt(const float* s, std::size_t n, double p) {
    if (p <= 0.0) return s[0];
    if (p >= (double)(n - 1)) return s[n - 1];
    std::size_t i = (std::size_t)p;
    float frac = (float)(p - (double)i);
    return s[i] * (1.0f - frac) + s[i + 1] * frac;
}

} // namespace

void buildScopeVertices(const float* histL, const float* histR, std::size_t n,
                        int windowSamples, int pointCount, ScopeMode mode,
                        bool trigger, float gain, std::vector<float>& out) {
    if (pointCount < 1) pointCount = 1;
    out.assign((std::size_t)pointCount * 3, 0.0f);
    if (n < 2 || histL == nullptr || histR == nullptr) return;   // flat trace

    int win = windowSamples;
    if (win < 2) win = 2;
    if ((std::size_t)win > n) win = (int)n;

    if (mode == ScopeMode::XY) {
        std::size_t start = n - (std::size_t)win;
        for (int j = 0; j < pointCount; ++j) {
            double t = (pointCount > 1) ? (double)j / (pointCount - 1) : 0.0;
            double p = (double)start + t * (double)(win - 1);
            out[(std::size_t)j * 3 + 0] = sampleAt(histL, n, p) * gain;
            out[(std::size_t)j * 3 + 1] = sampleAt(histR, n, p) * gain;
            out[(std::size_t)j * 3 + 2] = 0.0f;
        }
        return;
    }

    // Waveform: downmix to mono so the trigger search and the resample read one signal.
    std::vector<float> s(n);
    for (std::size_t i = 0; i < n; ++i) s[i] = 0.5f * (histL[i] + histR[i]);

    std::size_t start = n - (std::size_t)win;                    // free-running default
    if (trigger && start >= 1) {                                 // need s[i-1]; room to look back
        std::size_t lo = (start > (std::size_t)win) ? (start - (std::size_t)win) : 1;
        for (std::size_t i = start; ; --i) {
            if (s[i - 1] < 0.0f && s[i] >= 0.0f) { start = i; break; }   // latest rising ZC
            if (i == lo) break;
        }
    }

    for (int j = 0; j < pointCount; ++j) {
        double t = (pointCount > 1) ? (double)j / (pointCount - 1) : 0.0;
        double p = (double)start + t * (double)(win - 1);
        out[(std::size_t)j * 3 + 0] = -1.0f + 2.0f * (float)t;
        out[(std::size_t)j * 3 + 1] = sampleAt(s.data(), n, p) * gain;
        out[(std::size_t)j * 3 + 2] = 0.0f;
    }
}

} // namespace oss
