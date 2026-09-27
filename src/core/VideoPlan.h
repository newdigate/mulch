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
constexpr double      kVideoRestartLeads  = 3.0;   // reverse: a moving playhead further above what is covered than
                                                   // this many leads (plus a frame) restarts rather than waits
constexpr double      kVideoIndexLead     = 8.0;   // an index may list a keyframe by its decode time: up to this
                                                   // many frames early (B-frames: x264 2, x265's open GOP up to 6)

// How many RGBA frames of w x h fit the budget, clamped to [kVideoPoolMinFrames, kVideoPoolMaxFrames].
inline int videoPoolFrames(std::size_t budgetBytes, int w, int h) {
    if (w <= 0 || h <= 0) return kVideoPoolMinFrames;
    const std::size_t n = budgetBytes / ((std::size_t)w * (std::size_t)h * 4u);
    if (n < (std::size_t)kVideoPoolMinFrames) return kVideoPoolMinFrames;
    if (n > (std::size_t)kVideoPoolMaxFrames) return kVideoPoolMaxFrames;
    return (int)n;
}

// Start of the lap containing u: the multiple of D at or below u, exactly as computed, so start <= u <
// start + D and a lap start maps to itself (the division alone can come out a lap low: 1.4 * 3 / 1.4 is
// just below 3). 0 when the duration is unknown.
inline double videoLapStart(double u, double duration) {
    if (!(duration > 0.0)) return 0.0;
    double k = std::floor(u / duration);
    if (duration * (k + 1.0) <= u) k += 1.0;
    else if (duration * k > u)     k -= 1.0;
    return duration * k;
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

// A reverse stretch: the frames from a keyframe up to `end`, decoded forward in one pass. Live keeps
// every `stride`-th frame counting back from `top` -- the nominal time of the frame containing `end`,
// on the keyframe's grid -- so the stretch stays evenly covered within the `keep` budget. Offline
// (`contiguous`) keeps the `keep` frames nearest `end`, all of them, so reverse renders are exact.
struct VideoStretch {
    double end        = 0.0;
    double top        = 0.0;   // live: nominal time of the frame containing `end`, on the keyframe's grid
    int    keep       = 1;
    int    stride     = 1;
    bool   contiguous = false;
};

inline VideoStretch videoPlanStretch(double keyTime, double end, double frameDur, int budget,
                                     bool offline) {
    VideoStretch s;
    s.end        = end;
    s.top        = end;
    s.keep       = budget < 1 ? 1 : budget;
    s.contiguous = offline;
    if (offline || !(frameDur > 0.0)) return s;
    double last = std::floor((end - keyTime) / frameDur + kVideoTimeEps);   // the frame containing `end`
    if (!(last >= 0.0)) last = 0.0;                                         // end before the key, or NaN
    if (last > 1e9) last = 1e9;                                             // keep the casts defined
    const long long n = (long long)last + 1;                                // frames in [keyTime, end]
    s.top    = keyTime + (double)(n - 1) * frameDur;
    s.stride = (int)((n + s.keep - 1) / s.keep);                            // ceil(n / keep)
    return s;
}

// Whether a live stretch keeps the frame at time t: every stride-th, counting back from the top frame.
// The count ROUNDS onto the keyframe's grid: container time bases (MKV/WebM milliseconds, QuickTime
// 1/600) put frames up to half a frame off it, and flooring made two frames share a count and skip
// others, so a stride could keep nothing. A frame half a frame or more past the top is never kept.
// Contiguous stretches convert every frame (their ring keeps only the newest `keep`).
inline bool videoStretchKeeps(const VideoStretch& s, double t, double frameDur) {
    if (s.contiguous || !(frameDur > 0.0)) return true;
    const double x = (s.top - t) / frameDur;
    if (!(x > -0.5) || x > 1e9) return false;                               // past the top, NaN, absurd
    const long long k = std::llround(x);
    return s.stride <= 1 || k % s.stride == 0;
}

enum class VideoStepKind { Wait, Fill, CatchUp, Seek, Wrap, Reverse };

struct VideoStep {
    VideoStepKind kind  = VideoStepKind::Wait;
    double        to    = 0.0;     // CatchUp / Seek: the target. Reverse, fresh: the stretch's end
                                   // (inclusive). Reverse, not fresh: the start of the stretch above --
                                   // the new stretch lies just below it.
    bool          fresh = false;   // Reverse: start a new run (flush) rather than the next stretch down
};

// A reverse stretch's frame budget: half the pool (at least one frame), so the stretch below can decode
// while this one is shown.
inline int videoStretchBudget(int poolSize) { return poolSize / 2 > 1 ? poolSize / 2 : 1; }

// After a seek for `target` issued with the decoder's head at `before`, landing on `landedAt`: when a
// seek ahead lands at or behind the head (no keyframe index, or a wrong one -- MPEG-TS lists its seek
// probes as keyframes), no keyframe lies in (landedAt, target], so a seek for a nearby target would
// land there again. It returns the planner's noSeekBelow: catch up until the target has moved on by that
// gap (at least kVideoSeekNoIndex). -inf when the seek got ahead, or went back on purpose.
inline double videoNoSeekBelow(double target, double before, double landedAt) {
    if (!(target > before && landedAt <= before + kVideoTimeEps)) return -std::numeric_limits<double>::infinity();
    return target + std::max(kVideoSeekNoIndex, target - landedAt);
}

// Everything the worker's next decision depends on (times unwrapped).
struct VideoPlanInput {
    double target      = 0.0;      // requested playhead
    int    dir         = 1;        // +1 forward, -1 reverse (a paused request keeps the last direction)
    bool   dirChanged  = false;    // the direction flipped since the last decision
    bool   loop        = true;     // looping -- which needs a known duration
    double duration    = 0.0;      // D; 0 = unknown (no laps)
    double frameDur    = 1.0 / 30.0;
    double lapLo       = -std::numeric_limits<double>::infinity();   // not looping: the playhead's lap, which
    double lapHi       =  std::numeric_limits<double>::infinity();   // must start at a finite time ([0, inf)
                                   // when the duration is unknown)
    double head        = 0.0;      // time of the next frame the decoder will produce
    bool   eof         = false;    // the decoder has produced the last frame of this lap
    bool   keyKnown    = false;    // the stream has a keyframe index...
    double nextKey     = 0.0;      // ...and this is the first keyframe in it after `head`, maybe listed by its
                                   // decode time (kVideoIndexLead); +inf: none known (an index built as the
                                   // file is read, NUT's, may not have got there)
    double lapEnd      = std::numeric_limits<double>::infinity();   // where the decoder's lap ends, loop or
                                   // not (+inf: unknown duration) -- the next lap starts with a keyframe
    double lowest      = 0.0;      // earliest frame held (on screen or queued); `head` if none
    bool   seekPinned  = false;    // the last seek could not land at or before its target...
    double pinnedFrom  = 0.0;      // ...so do not seek again for targets at or after this
    double noSeekBelow = -std::numeric_limits<double>::infinity();   // videoNoSeekBelow() of the last seek:
                                   // until the target passes this, a seek within the lap would land behind
    int    freeBuffers = 0;
    int    poolSize    = 0;
    bool   coverValid  = false;    // reverse: this run's stretches reach down to coverLo...
    double coverLo     = 0.0;
    double coverHi     = 0.0;      // ...from here, the end of the run's first stretch
    double lead        = 0.0;      // reverse, live: how far the playhead moves while a stretch decodes (s);
                                   // 0 when it does not move (paused) or nothing leads (offline)
};

// The worker's next step.
//  Forward: seek when the direction changed, when the target is behind everything held (unless the last
//  seek was pinned there), or -- loop off -- when the decoder is in an earlier lap than the playhead (it
//  fell behind while looping): nothing it decodes there can be shown. When the target is ahead of the
//  decoder by more than kVideoCatchUpFrames, seek if a keyframe lies between (the next lap's start
//  counts, exactly; an index keyframe counts kVideoIndexLead frames later, since it may be listed by its
//  decode time) and the jump is longer than kVideoSeekMinJump -- a seek restarts the decoder's
//  frame-threading pipeline, which costs more than decoding through a short gap -- or, with no index, if
//  it is more than kVideoSeekNoIndex away; but within the lap not while noSeekBelow says a seek would
//  land behind the decoder again (no index, or a wrong one); otherwise catch up by decoding without
//  converting. At the end of the lap wrap (loop) or wait; with loop off and the decoder at or past the
//  end of the playhead's lap (it had decoded ahead, or just wrapped, while looping), wait -- those
//  frames can never be shown; otherwise fill a free buffer, or wait.
//  Reverse: start a fresh stretch when the run is new, when the playhead fell below what is covered, or
//  when it is stranded above it -- stopped (no lead: it never arrives), or more than kVideoRestartLeads
//  leads (plus a frame) above, so arriving would take several stretches' time -- aimed `lead` below the
//  playhead (where it will be once the stretch is decoded, so a slow stretch does not land behind it and
//  restart forever). Otherwise prefetch the stretch below once a stretch's budget of buffers is free;
//  wait at the start of the clip when loop is off.
inline VideoStep videoNextStep(const VideoPlanInput& in) {
    const double eps = kVideoTimeEps;
    const bool loops = in.loop && in.duration > 0.0;
    if (in.dir < 0) {
        const double above = in.lead > 0.0 ? kVideoRestartLeads * in.lead + in.frameDur : 0.0;
        const bool stranded = in.target > in.coverHi + above + eps;
        if (in.dirChanged || !in.coverValid || in.target < in.coverLo - eps || stranded) {
            double aim = in.target - std::max(0.0, in.lead);
            if (!loops && aim < in.lapLo) aim = std::min(in.target, in.lapLo);
            return VideoStep{VideoStepKind::Reverse, aim, true};
        }
        if (!loops && in.coverLo <= in.lapLo + eps) return VideoStep{};    // nothing earlier to decode
        if (in.freeBuffers >= videoStretchBudget(in.poolSize)) return VideoStep{VideoStepKind::Reverse, in.coverLo, false};
        return VideoStep{};
    }
    const bool pinned = in.seekPinned && in.target >= in.pinnedFrom - eps;
    if (in.dirChanged || (in.target < in.lowest - eps && !pinned))
        return VideoStep{VideoStepKind::Seek, in.target};
    if (!loops && in.lapEnd <= in.lapLo + eps)                             // the decoder is a lap behind
        return VideoStep{VideoStepKind::Seek, in.target};
    if (!in.eof && in.target > in.head + kVideoCatchUpFrames * in.frameDur) {
        const bool   known  = in.keyKnown || in.target >= in.lapEnd;
        const double listed = in.keyKnown ? in.nextKey + kVideoIndexLead * in.frameDur : std::numeric_limits<double>::infinity();
        const double key    = std::min(listed, in.lapEnd);
        const double gap    = in.target - in.head;
        const bool   jump   = known ? (key <= in.target && gap > kVideoSeekMinJump) : gap > kVideoSeekNoIndex;
        const bool   futile = in.target < in.lapEnd && in.target < in.noSeekBelow;
        return VideoStep{jump && !futile ? VideoStepKind::Seek : VideoStepKind::CatchUp, in.target};
    }
    if (in.eof) return loops ? VideoStep{VideoStepKind::Wrap} : VideoStep{};
    if (!loops && in.head >= in.lapHi - eps) return VideoStep{};
    if (in.freeBuffers > 0) return VideoStep{VideoStepKind::Fill};
    return VideoStep{};
}

} // namespace oss
