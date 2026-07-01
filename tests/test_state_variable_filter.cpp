#include <doctest/doctest.h>
#include "audio/StateVariableFilter.h"
#include <cmath>

using namespace oss;

static const float kPi = 3.14159265358979323846f;

// RMS of one band tap (0=low, 1=band, 2=high) after driving a sine of `hz` and
// letting the filter settle. Uses the filter's current coefficients.
static float bandRms(StateVariableFilter& f, float hz, int sr, int which) {
    double sumsq = 0.0;
    int warm = sr / 10;   // ~100 ms warm-up
    int meas = sr / 10;   // ~100 ms measurement
    int phase = 0;
    for (int i = 0; i < warm + meas; ++i, ++phase) {
        float in = std::sin(2.0f * kPi * hz * (float)phase / (float)sr);
        SvfOut o = f.process(in);
        float y = which == 0 ? o.low : (which == 1 ? o.band : o.high);
        if (i >= warm) sumsq += (double)y * y;
    }
    return (float)std::sqrt(sumsq / (double)meas);
}

static float measureRms(float cutoff, float res, float hz, int sr, int which) {
    StateVariableFilter f;
    f.setCoefficients(cutoff, res, sr);
    return bandRms(f, hz, sr, which);
}

TEST_CASE("SVF: DC passes the lowpass, is blocked by the highpass") {
    StateVariableFilter f;
    f.setCoefficients(1000.0f, 0.0f, 48000);
    SvfOut o{};
    for (int i = 0; i < 48000; ++i) o = f.process(1.0f);   // settle on a constant
    CHECK(o.low  == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(o.high == doctest::Approx(0.0f).epsilon(0.01));
}

TEST_CASE("SVF: a tone below cutoff passes the lowpass, not the highpass") {
    int sr = 48000;
    float inRms = 1.0f / std::sqrt(2.0f);            // RMS of a unit sine
    float lp = measureRms(2000.0f, 0.0f, 100.0f, sr, 0);   // 100 Hz << 2 kHz
    float hp = measureRms(2000.0f, 0.0f, 100.0f, sr, 2);
    CHECK(lp == doctest::Approx(inRms).epsilon(0.1));       // LP passes
    CHECK(hp < 0.15f * inRms);                              // HP blocks
}

TEST_CASE("SVF: a tone above cutoff passes the highpass, not the lowpass") {
    int sr = 48000;
    float inRms = 1.0f / std::sqrt(2.0f);
    float hp = measureRms(500.0f, 0.0f, 8000.0f, sr, 2);   // 8 kHz >> 500 Hz
    float lp = measureRms(500.0f, 0.0f, 8000.0f, sr, 0);
    CHECK(hp == doctest::Approx(inRms).epsilon(0.15));      // HP passes
    CHECK(lp < 0.15f * inRms);                              // LP blocks
}

TEST_CASE("SVF: stays finite and bounded at maximum resonance") {
    StateVariableFilter f;
    f.setCoefficients(1000.0f, 1.0f, 48000);
    for (int i = 0; i < 96000; ++i) {
        float in = (i == 0) ? 1.0f : 0.5f * std::sin(0.3f * (float)i);   // impulse then drive
        SvfOut o = f.process(in);
        REQUIRE(std::isfinite(o.low));
        REQUIRE(std::isfinite(o.band));
        REQUIRE(std::isfinite(o.high));
        REQUIRE(std::fabs(o.low)  < 100.0f);
        REQUIRE(std::fabs(o.band) < 100.0f);
        REQUIRE(std::fabs(o.high) < 100.0f);
    }
}
