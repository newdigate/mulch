#include "audio/SignalGenerator.h"
#include <cmath>

namespace oss {

void SignalGenerator::generate(float* out, std::size_t count) {
    const double kTwoPi = 2.0 * 3.14159265358979323846;
    const double inc = kTwoPi * freq_ / sampleRate_;
    for (std::size_t i = 0; i < count; ++i) {
        double s = 0.6 * std::sin(phase_)
                 + 0.3 * std::sin(2.0 * phase_)
                 + 0.1 * std::sin(3.0 * phase_);
        out[i] = static_cast<float>(s);
        phase_ += inc;
        if (phase_ > kTwoPi) phase_ -= kTwoPi;
    }
}

} // namespace oss
