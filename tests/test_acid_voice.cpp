#include <doctest/doctest.h>
#include "audio/AcidVoice.h"
#include <algorithm>
#include <cmath>
#include <vector>

using namespace oss;

static constexpr double kPi = 3.14159265358979323846;

static float rms(const float* x, int n) {
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += (double)x[i] * x[i];
    return (float)std::sqrt(s / (n > 0 ? n : 1));
}

TEST_CASE("ladder filter attenuates content above its cutoff") {
    const int sr = 48000, n = 4800;
    std::vector<float> in(n), low(n), high(n);
    for (int i = 0; i < n; ++i) in[i] = std::sin(2.0 * kPi * 8000.0 * i / sr);  // 8 kHz
    LadderFilter f; f.reset();
    for (int i = 0; i < n; ++i) low[i]  = f.process(in[i], 200.0f, 0.2f, sr);
    f.reset();
    for (int i = 0; i < n; ++i) high[i] = f.process(in[i], 18000.0f, 0.2f, sr);
    // 8 kHz survives a 18 kHz cutoff but is crushed by a 200 Hz cutoff.
    CHECK(rms(low.data() + 480, n - 480) < 0.3f * rms(high.data() + 480, n - 480));
}

TEST_CASE("ladder filter resonance boosts energy near the cutoff") {
    const int sr = 48000, n = 9600;
    std::vector<float> in(n), lo(n), hi(n);
    for (int i = 0; i < n; ++i) in[i] = 0.5f * std::sin(2.0 * kPi * 1000.0 * i / sr);
    LadderFilter f1; for (int i = 0; i < n; ++i) lo[i] = f1.process(in[i], 1000.0f, 0.1f, sr);
    LadderFilter f2; for (int i = 0; i < n; ++i) hi[i] = f2.process(in[i], 1000.0f, 0.9f, sr);
    CHECK(rms(hi.data() + 960, n - 960) > rms(lo.data() + 960, n - 960));
}

TEST_CASE("ladder filter stays finite and bounded at max resonance") {
    const int sr = 48000, n = 48000;
    LadderFilter f;
    f.process(1.0f, 1000.0f, 1.0f, sr);                 // a kick to perturb it
    float m = 0.0f; bool finite = true;
    for (int i = 0; i < n; ++i) {
        float y = f.process(0.0f, 1000.0f, 1.0f, sr);
        if (!std::isfinite(y)) finite = false;
        m = std::max(m, std::fabs(y));
    }
    CHECK(finite);
    CHECK(m < 10.0f);
}
