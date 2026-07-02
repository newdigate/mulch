#pragma once
#include <cmath>

namespace oss {

// The transport-synced image index: which of `count` images shows at song position `beats`,
// given `durationBeats` beats per image. Loops; handles negative beats. count<=0 -> 0.
inline int syncedImageIndex(double beats, float durationBeats, int count) {
    if (count <= 0) return 0;
    double d = (double)(durationBeats > 1e-4f ? durationBeats : 1e-4f);
    long long step = (long long)std::floor(beats / d);   // long long: MSVC's long is 32-bit
    long long idx  = ((step % count) + count) % count;   // positive modulo
    return (int)idx;
}

// The cross-fade blend factor (0 = from, 1 = to). Caps the effective fade at `interval` so a
// fade never outlasts the gap between images; fadeDur <= 0 -> 1 (instant). Clamped to [0,1].
inline float crossfadeMix(float elapsed, float fadeDur, float interval) {
    float eff = fadeDur < interval ? fadeDur : interval;   // cap to the image interval
    if (eff <= 0.0f) return 1.0f;
    float m = elapsed / eff;
    return m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
}

} // namespace oss
