#pragma once
#include <algorithm>

namespace oss {

// Per-channel gains for the audio pan/downmix nodes. GL-free.
struct PanGains { float l, r; };

// Mono -> stereo pan (the Audio Mix balance law): centre (pan 0) keeps both at
// unity; pan -1 mutes the right, pan +1 mutes the left. pan in [-1, 1].
inline PanGains panGains(float pan) {
    return { 1.0f - std::max(0.0f, pan), 1.0f + std::min(0.0f, pan) };
}

// Stereo -> mono downmix crossfade: balance 0 averages (0.5L + 0.5R);
// balance -1 keeps only L, +1 keeps only R. balance in [-1, 1].
inline PanGains downmixGains(float balance) {
    return { 0.5f * (1.0f - balance), 0.5f * (1.0f + balance) };
}

} // namespace oss
