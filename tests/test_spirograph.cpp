#include <doctest/doctest.h>
#include "audio/Spirograph.h"
#include <cmath>
#include <vector>

using namespace oss;

TEST_CASE("Spirograph output is bounded to [-1,1] across the parameter space") {
    for (int curve = 0; curve <= 1; ++curve)
        for (float ratio = 2.0f; ratio <= 12.0f; ratio += 0.5f)
            for (float pen = 0.0f; pen <= 1.0f; pen += 0.1f) {
                Spirograph s;
                s.setSampleRate(48000); s.setCurveType(curve);
                s.setFrequency(220.0f); s.setRatio(ratio); s.setPen(pen); s.setLevel(1.0f);
                std::vector<float> L(256), R(256);
                s.process(L.data(), R.data(), 256);
                for (int i = 0; i < 256; ++i) {
                    CHECK(L[i] >= -1.0001f); CHECK(L[i] <= 1.0001f);
                    CHECK(R[i] >= -1.0001f); CHECK(R[i] <= 1.0001f);
                }
            }
}

TEST_CASE("Spirograph with pen=0 is quadrature sine (L=cos, R=sin, L^2+R^2 constant)") {
    Spirograph s;
    s.setSampleRate(48000); s.setCurveType(0);
    s.setFrequency(300.0f); s.setRatio(5.0f); s.setPen(0.0f); s.setLevel(1.0f);
    std::vector<float> L(512), R(512);
    s.process(L.data(), R.data(), 512);
    const double inc = Spirograph::kTwoPi * 300.0 / 48000.0;
    for (int i = 0; i < 512; ++i) {
        CHECK(L[i] == doctest::Approx(std::cos(inc * i)).epsilon(1e-3));
        CHECK(R[i] == doctest::Approx(std::sin(inc * i)).epsilon(1e-3));
        CHECK(L[i]*L[i] + R[i]*R[i] == doctest::Approx(1.0).epsilon(1e-3));
    }
}

TEST_CASE("Spirograph phase is continuous across process() blocks") {
    // Two 200-sample blocks must equal one 400-sample block, sample-for-sample.
    Spirograph split;
    split.setSampleRate(48000); split.setFrequency(440.0f);
    split.setRatio(4.0f); split.setPen(0.5f); split.setCurveType(0); split.setLevel(1.0f);
    std::vector<float> aL(200), aR(200), bL(200), bR(200);
    split.process(aL.data(), aR.data(), 200);
    split.process(bL.data(), bR.data(), 200);

    Spirograph whole;
    whole.setSampleRate(48000); whole.setFrequency(440.0f);
    whole.setRatio(4.0f); whole.setPen(0.5f); whole.setCurveType(0); whole.setLevel(1.0f);
    std::vector<float> wL(400), wR(400);
    whole.process(wL.data(), wR.data(), 400);

    for (int i = 0; i < 200; ++i) {
        CHECK(wL[i]       == doctest::Approx(aL[i]).epsilon(1e-4));
        CHECK(wL[200 + i] == doctest::Approx(bL[i]).epsilon(1e-4));
        CHECK(wR[i]       == doctest::Approx(aR[i]).epsilon(1e-4));
        CHECK(wR[200 + i] == doctest::Approx(bR[i]).epsilon(1e-4));
    }
}

TEST_CASE("Spirograph left channel oscillates at the requested frequency") {
    // pen=0 -> pure cos on L; count rising zero-crossings over 1 s ~= freq.
    Spirograph s;
    s.setSampleRate(48000); s.setFrequency(100.0f);
    s.setRatio(5.0f); s.setPen(0.0f); s.setCurveType(0); s.setLevel(1.0f);
    std::vector<float> L(48000), R(48000);
    s.process(L.data(), R.data(), 48000);
    int rising = 0;
    for (int i = 1; i < 48000; ++i)
        if (L[i-1] < 0.0f && L[i] >= 0.0f) ++rising;
    CHECK(rising >= 98);
    CHECK(rising <= 102);
}

TEST_CASE("Spirograph curve type changes the waveform") {
    Spirograph hypo;
    hypo.setSampleRate(48000); hypo.setFrequency(220.0f);
    hypo.setRatio(4.0f); hypo.setPen(0.7f); hypo.setCurveType(0); hypo.setLevel(1.0f);
    Spirograph epi;
    epi.setSampleRate(48000); epi.setFrequency(220.0f);
    epi.setRatio(4.0f); epi.setPen(0.7f); epi.setCurveType(1); epi.setLevel(1.0f);
    std::vector<float> hL(256), hR(256), eL(256), eR(256);
    hypo.process(hL.data(), hR.data(), 256);
    epi.process(eL.data(), eR.data(), 256);
    double diff = 0.0;
    for (int i = 0; i < 256; ++i) diff += std::abs(hL[i] - eL[i]);
    CHECK(diff > 1.0);   // materially different waveforms
}
