#include <doctest/doctest.h>
#include "audio/SignalGenerator.h"
#include <vector>

TEST_CASE("generate fills the requested number of samples in [-1,1]") {
    oss::SignalGenerator g(48000, 440.0f);
    std::vector<float> buf(256);
    g.generate(buf.data(), buf.size());
    for (float s : buf) { CHECK(s >= -1.0001f); CHECK(s <= 1.0001f); }
}

TEST_CASE("generate is phase-continuous across calls") {
    oss::SignalGenerator whole(48000, 440.0f), split(48000, 440.0f);
    std::vector<float> w(200), p1(100), p2(100);
    whole.generate(w.data(), 200);
    split.generate(p1.data(), 100);
    split.generate(p2.data(), 100);
    for (int i = 0; i < 100; ++i) {
        CHECK(w[i]       == doctest::Approx(p1[i]).epsilon(1e-4));
        CHECK(w[100 + i] == doctest::Approx(p2[i]).epsilon(1e-4));
    }
}
