#include "audio/FFT.h"
#include <cmath>

namespace oss {

void fft(std::vector<std::complex<float>>& a) {
    const std::size_t n = a.size();
    if (n < 2) return;
    // bit-reversal permutation
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    const float kPi = 3.14159265358979323846f;
    for (std::size_t len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * kPi / (float)len;
        std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                std::complex<float> u = a[i + k];
                std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

std::vector<float> magnitudeSpectrum(const std::vector<float>& samples) {
    std::vector<std::complex<float>> a(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i)
        a[i] = std::complex<float>(samples[i], 0.0f);
    fft(a);
    std::vector<float> mag(samples.size() / 2);
    for (std::size_t i = 0; i < mag.size(); ++i) mag[i] = std::abs(a[i]);
    return mag;
}

} // namespace oss
