#pragma once
#include <vector>
#include <complex>

namespace oss {

// In-place iterative radix-2 FFT. a.size() MUST be a power of two.
void fft(std::vector<std::complex<float>>& a);

// Magnitude spectrum (size N/2) of N real samples (N a power of two).
std::vector<float> magnitudeSpectrum(const std::vector<float>& samples);

} // namespace oss
