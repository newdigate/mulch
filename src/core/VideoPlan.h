#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace oss {

// Playback decisions for the Video Player's background decoder (gfx/VideoStream), kept as pure
// functions so they are unit-tested without FFmpeg, threads or GL. GL-free and FFmpeg-free.
//
// Times are UNWRAPPED seconds: u = lap * D + position, where D is the clip duration. The node's
// playhead keeps counting past the end of the clip (and below 0 in reverse) instead of jumping
// back, so the worker can decode into the next lap before the playhead gets there.

constexpr std::size_t kVideoPoolBytes     = std::size_t(512) * 1024 * 1024;  // frame buffers per node
constexpr int         kVideoPoolMinFrames = 4;
constexpr int         kVideoPoolMaxFrames = 64;
constexpr double      kVideoCatchUpFrames = 2.0;   // further behind than this many frames: catch up or seek
constexpr double      kVideoSeekNoIndex   = 2.0;   // no keyframe index: seek when more than this far ahead (s)
constexpr double      kVideoSeekMinJump   = 1.0;   // a seek restarts the decoder's pipeline: only for longer jumps (s)
constexpr double      kVideoAudioLead     = 1.0;   // keep decoded audio this far ahead of the playhead (s)
constexpr double      kVideoAudioKeep     = 2.0;   // keep already-played audio this long (s)
constexpr double      kVideoTimeEps       = 1e-6;  // frame-time comparisons: absorbs rounding, not accumulated drift
constexpr double      kVideoCatchUpSlice  = 0.1;   // live: show the best frame reached at least this often (s)

// How many RGBA frames of w x h fit the budget, clamped to [kVideoPoolMinFrames, kVideoPoolMaxFrames].
inline int videoPoolFrames(std::size_t budgetBytes, int w, int h) {
    if (w <= 0 || h <= 0) return kVideoPoolMinFrames;
    const std::size_t n = budgetBytes / ((std::size_t)w * (std::size_t)h * 4u);
    if (n < (std::size_t)kVideoPoolMinFrames) return kVideoPoolMinFrames;
    if (n > (std::size_t)kVideoPoolMaxFrames) return kVideoPoolMaxFrames;
    return (int)n;
}

// Start of the lap containing u: D * floor(u / D). 0 when the duration is unknown.
inline double videoLapStart(double u, double duration) {
    return duration > 0.0 ? duration * std::floor(u / duration) : 0.0;
}

// Position within the clip of a looping playhead, in [0, D) up to rounding.
inline double videoWrapped(double u, double duration) { return u - videoLapStart(u, duration); }

// The node's playhead: unwrapped, plus the lap it is clamped to while loop is off.
struct VideoPlayhead {
    double u        = 0.0;
    double lapLo    = 0.0;    // loop off: u stays within [lapLo, lapLo + D)
    bool   loopPrev = true;   // loop was on last frame (the lap is captured when loop goes off)
};

// Advance by rate * dt while playing, then apply the loop rule.
//  - Loop on runs freely (the display wraps).
//  - Loop off clamps to the lap the playhead was in when loop went off, pinned from the position
//    BEFORE this step, so a step that crosses the end cannot pin the next lap. The clamp stops just
//    short of the lap's end: lapLo + D is where the next lap's first frame sits -- the worker decodes
//    it early while looping -- so the end holds this lap's last frame.
//  - An unknown duration (0) has no laps: the playhead only clamps at 0.
inline VideoPlayhead videoAdvance(VideoPlayhead p, bool play, double rate, bool loop, double dt,
                                  double duration) {
    if (duration > 0.0 && !loop && p.loopPrev) p.lapLo = videoLapStart(p.u, duration);
    if (play) p.u += rate * dt;
    if (duration > 0.0) {
        if (!loop) p.u = std::clamp(p.u, p.lapLo, p.lapLo + std::max(0.0, duration - 2.0 * kVideoTimeEps));
    } else if (p.u < 0.0) {
        p.u = 0.0;
    }
    p.loopPrev = loop;
    return p;
}

// The position shown for a playhead: wrapped while looping, relative to the clamped lap otherwise.
inline double videoPosition(const VideoPlayhead& p, bool loop, double duration) {
    if (duration <= 0.0) return p.u;
    return loop ? videoWrapped(p.u, duration) : p.u - p.lapLo;
}

// Offline renders pass dt = 1.0f / fps. Accumulating that float drifts off the frame grid (at 25 fps
// by ~9e-10 s a frame, past kVideoTimeEps after ~45 s), so the node snaps it back to the exact step
// 1 / fps when fps is a whole number -- every rate the renderer offers is. Any other dt passes through.
inline double videoFrameStep(float dt) {
    if (!(dt > 0.0f)) return 0.0;
    const double fps = 1.0 / (double)dt;
    const double whole = std::round(fps);
    return (whole >= 1.0 && std::fabs(fps - whole) < 1e-3) ? 1.0 / whole : (double)dt;
}

// The frame to show for playhead u: the index of the greatest time <= u among n ascending times
// (`timeOf(i)` returns the i-th), or -1 when every frame is later than u or there are none.
template <class TimeOf>
inline int videoSelectFrame(int n, double u, TimeOf timeOf) {
    int best = -1;
    for (int i = 0; i < n; ++i) {
        if (timeOf(i) <= u + kVideoTimeEps) best = i;
        else break;
    }
    return best;
}

} // namespace oss
