#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace oss {

constexpr int    kAudioMaxBlock = 8192;   // max audio frames produced per evaluate (safety bound)
constexpr double kMaxAudioDt    = 0.085;  // clamp dt for AUDIO block sizing (~4080 frames @48k)

// Frames of audio a source should produce this frame: round(sampleRate * min(dt, kMaxAudioDt)),
// clamped to [1, kAudioMaxBlock]. The dt clamp is local to audio (the transport keeps its own dt),
// so a multi-second stall yields a bounded ~85 ms catch-up block, not a runaway allocation.
inline int audioBlockFrames(double sampleRate, double dt) {
    double d = dt < 0.0 ? 0.0 : (dt > kMaxAudioDt ? kMaxAudioDt : dt);
    long n = std::lround(sampleRate * d);
    if (n < 1) n = 1;
    if (n > kAudioMaxBlock) n = kAudioMaxBlock;
    return (int)n;
}

// Output-ring capacity in interleaved-stereo floats for a buffer length in milliseconds.
// (SpscRingBuffer rounds this up to a power of two internally.)
inline std::size_t audioRingFloats(int bufferMs, int sampleRate) {
    if (bufferMs < 1) bufferMs = 1;
    long frames = (long)((long long)bufferMs * sampleRate / 1000);
    if (frames < 1) frames = 1;
    return (std::size_t)frames * 2;   // L + R interleaved
}

} // namespace oss
