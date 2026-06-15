#include <doctest/doctest.h>
#include "audio/FFT.h"
#include <vector>
#include <cmath>

TEST_CASE("magnitudeSpectrum peaks at a pure sine's bin") {
    constexpr float kPi = 3.14159265358979323846f;
    const int N = 64, k = 4;
    std::vector<float> s(N);
    for (int n = 0; n < N; ++n) s[n] = std::sin(2.0f * kPi * k * n / N);
    auto mag = oss::magnitudeSpectrum(s);
    REQUIRE(mag.size() == (size_t)N / 2);
    int peak = 0;
    for (int i = 1; i < (int)mag.size(); ++i) if (mag[i] > mag[peak]) peak = i;
    CHECK(peak == k);
}

TEST_CASE("magnitudeSpectrum of DC has all energy in bin 0") {
    std::vector<float> s(32, 1.0f);
    auto mag = oss::magnitudeSpectrum(s);
    for (size_t i = 1; i < mag.size(); ++i) CHECK(mag[i] == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(mag[0] > 1.0f);
}
