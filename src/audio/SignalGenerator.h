#pragma once
#include <cstddef>

namespace oss {

// Phase-continuous synthesizer: a fundamental sine plus a couple of harmonics,
// so the spectrograph shows structure. Stateful: each generate() advances phase.
class SignalGenerator {
public:
    explicit SignalGenerator(int sampleRate = 48000, float freq = 220.0f)
        : sampleRate_(sampleRate), freq_(freq) {}

    void generate(float* out, std::size_t count);
    int  sampleRate() const { return sampleRate_; }

private:
    int    sampleRate_;
    float  freq_;
    double phase_ = 0.0;   // radians, accumulated for continuity
};

} // namespace oss
