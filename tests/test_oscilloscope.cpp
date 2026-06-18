#include <doctest/doctest.h>
#include "core/Oscilloscope.h"
#include <cmath>
#include <vector>

using namespace oss;

static constexpr double kPi = 3.14159265358979323846;

// A sine history of length n at `freq` Hz / `sr`, with an optional phase offset (rad).
static std::vector<float> sineHist(std::size_t n, double freq, double sr, double phase = 0.0) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = (float)std::sin(2.0 * kPi * freq * (double)i / sr + phase);
    return v;
}

TEST_CASE("free-running waveform maps the window samples to a line strip") {
    const int N = 2000, P = 512;
    std::vector<float> ramp(N);
    for (int i = 0; i < N; ++i) ramp[i] = -1.0f + 2.0f * (float)i / (N - 1);   // -1..+1
    std::vector<float> out;
    buildScopeVertices(ramp.data(), ramp.data(), N, /*window*/ N, P,
                       ScopeMode::Waveform, /*trigger*/ false, /*gain*/ 1.0f, out);
    REQUIRE(out.size() == (std::size_t)P * 3);
    CHECK(out[0] == doctest::Approx(-1.0f));               // first x
    CHECK(out[(P - 1) * 3] == doctest::Approx(1.0f));      // last x
    CHECK(out[1] == doctest::Approx(-1.0f));               // first y == ramp[0]
    CHECK(out[(P - 1) * 3 + 1] == doctest::Approx(1.0f));  // last y == ramp[N-1]
    CHECK(out[2] == doctest::Approx(0.0f));                // z flat
}

TEST_CASE("gain scales the waveform amplitude") {
    const int N = 2000, P = 512;
    std::vector<float> ramp(N);
    for (int i = 0; i < N; ++i) ramp[i] = -1.0f + 2.0f * (float)i / (N - 1);
    std::vector<float> g1, g2;
    buildScopeVertices(ramp.data(), ramp.data(), N, N, P, ScopeMode::Waveform, false, 1.0f, g1);
    buildScopeVertices(ramp.data(), ramp.data(), N, N, P, ScopeMode::Waveform, false, 2.0f, g2);
    for (int j = 0; j < P; ++j)
        CHECK(g2[j * 3 + 1] == doctest::Approx(2.0f * g1[j * 3 + 1]));
}

TEST_CASE("trigger starts the waveform at a rising zero-crossing") {
    const int N = 1500, P = 300;
    auto sine = sineHist(N, 480.0, 48000.0);   // period 100 samples
    std::vector<float> out;
    buildScopeVertices(sine.data(), sine.data(), N, /*window*/ 300, P,
                       ScopeMode::Waveform, /*trigger*/ true, 1.0f, out);
    CHECK(std::abs(out[1]) < 0.1f);            // first y at/near zero
    CHECK(out[5 * 3 + 1] > out[1]);            // and rising
}

TEST_CASE("trigger keeps the trace phase-stable across input phase shifts") {
    const int N = 1500, P = 300;
    auto a = sineHist(N, 480.0, 48000.0, 0.0);
    auto b = sineHist(N, 480.0, 48000.0, 2.0 * kPi * 0.37);   // shifted ~0.37 of a period
    std::vector<float> oa, ob;
    buildScopeVertices(a.data(), a.data(), N, 300, P, ScopeMode::Waveform, true, 1.0f, oa);
    buildScopeVertices(b.data(), b.data(), N, 300, P, ScopeMode::Waveform, true, 1.0f, ob);
    CHECK(std::abs(oa[1]) < 0.1f);             // both lock to a zero-crossing...
    CHECK(std::abs(ob[1]) < 0.1f);
    int agree = 0;
    for (int j = 0; j < P; ++j)
        if (std::abs(oa[j * 3 + 1] - ob[j * 3 + 1]) < 0.12f) ++agree;
    CHECK(agree >= (int)(P * 0.9));            // ...so the displayed traces match
}

TEST_CASE("trigger falls back to free-running when there is no zero-crossing") {
    const int N = 1000, P = 256;
    std::vector<float> dc(N, 0.5f);            // all positive: no rising zero-crossing
    std::vector<float> trig, freerun;
    buildScopeVertices(dc.data(), dc.data(), N, 400, P, ScopeMode::Waveform, true, 1.0f, trig);
    buildScopeVertices(dc.data(), dc.data(), N, 400, P, ScopeMode::Waveform, false, 1.0f, freerun);
    for (int j = 0; j < P; ++j)
        CHECK(trig[j * 3 + 1] == doctest::Approx(freerun[j * 3 + 1]));
}

TEST_CASE("X-Y mode maps L to x and R to y and ignores the trigger") {
    const int N = 1000, P = 256;
    std::vector<float> l(N, 0.5f), r(N, -0.25f);   // distinct constants prove no swap
    std::vector<float> xy, xyTrig;
    buildScopeVertices(l.data(), r.data(), N, 400, P, ScopeMode::XY, false, 2.0f, xy);
    buildScopeVertices(l.data(), r.data(), N, 400, P, ScopeMode::XY, true,  2.0f, xyTrig);
    for (int j = 0; j < P; ++j) {
        CHECK(xy[j * 3 + 0] == doctest::Approx(1.0f));    // 0.5 * gain 2
        CHECK(xy[j * 3 + 1] == doctest::Approx(-0.5f));   // -0.25 * gain 2
        CHECK(xy[j * 3 + 2] == doctest::Approx(0.0f));
        CHECK(xyTrig[j * 3 + 0] == doctest::Approx(xy[j * 3 + 0]));   // trigger ignored
        CHECK(xyTrig[j * 3 + 1] == doctest::Approx(xy[j * 3 + 1]));
    }
}

TEST_CASE("X-Y traces the unit circle for sine/cosine inputs") {
    const int N = 1500, P = 300;
    auto s = sineHist(N, 480.0, 48000.0, 0.0);
    auto c = sineHist(N, 480.0, 48000.0, kPi / 2.0);   // cosine
    std::vector<float> out;
    buildScopeVertices(s.data(), c.data(), N, 300, P, ScopeMode::XY, false, 1.0f, out);
    for (int j = 0; j < P; ++j) {
        float x = out[j * 3 + 0], y = out[j * 3 + 1];
        CHECK(x * x + y * y == doctest::Approx(1.0f).epsilon(0.02));
    }
}

TEST_CASE("waveform downmixes L and R to mono") {
    const int N = 1000, P = 256;
    std::vector<float> l(N, 1.0f), r(N, -1.0f);          // 0.5*(1 + -1) = 0 -> cancels
    std::vector<float> out;
    buildScopeVertices(l.data(), r.data(), N, 400, P, ScopeMode::Waveform, false, 1.0f, out);
    for (int j = 0; j < P; ++j) CHECK(out[j * 3 + 1] == doctest::Approx(0.0f));
    std::vector<float> l2(N, 0.4f), r2(N, 0.6f);         // 0.5*(0.4+0.6)=0.5, *gain 2 = 1
    buildScopeVertices(l2.data(), r2.data(), N, 400, P, ScopeMode::Waveform, false, 2.0f, out);
    for (int j = 0; j < P; ++j) CHECK(out[j * 3 + 1] == doctest::Approx(1.0f));
}

TEST_CASE("degenerate inputs produce a safe flat trace") {
    std::vector<float> one(1, 0.5f);
    std::vector<float> out;
    buildScopeVertices(one.data(), one.data(), 1, 100, 256, ScopeMode::Waveform, true, 1.0f, out);
    REQUIRE(out.size() == (std::size_t)256 * 3);
    for (float v : out) CHECK(v == doctest::Approx(0.0f));        // n < 2 -> all zeros
    buildScopeVertices(nullptr, nullptr, 0, 100, 64, ScopeMode::XY, false, 1.0f, out);
    REQUIRE(out.size() == (std::size_t)64 * 3);
    for (float v : out) CHECK(v == doctest::Approx(0.0f));        // null/empty -> all zeros

    std::vector<float> ramp(100);
    for (int i = 0; i < 100; ++i) ramp[i] = (float)i;
    buildScopeVertices(ramp.data(), ramp.data(), 100, 10, 1, ScopeMode::Waveform, false, 1.0f, out);
    REQUIRE(out.size() == (std::size_t)3);                        // pointCount 1: no /0
    CHECK(out[0] == doctest::Approx(-1.0f));                      // x at t=0
    CHECK(out[1] == doctest::Approx(90.0f));                      // y == ramp[start], start = 100-10
}
