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

} // namespace oss
