# Video Player Background Decoding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop large videos from freezing the UI by moving the Video Player's decoding onto a per-node background worker, while keeping offline renders frame-exact.

**Architecture:** Pure, unit-tested playback decisions (`core/VideoPlan.h`) and a time-tagged audio store (`core/TimedAudio.h`) drive a worker thread (`gfx/VideoStream`) that owns a reworked `VideoDecoder` (threaded decode, split decode/convert, audio read-ahead) and a fixed pool of RGBA frames. The node (`modules/VideoPlayerNode`) only advances an unwrapped playhead, posts it, and uploads the newest ready frame through a flipped blit.

**Tech Stack:** C++17, FFmpeg ≥ 5.1 (libavformat/libavcodec/libswscale/libswresample), OpenGL 4.1 core, doctest (`core_tests`), the headless GL harness (`gl_smoke`).

**Spec:** [`docs/superpowers/specs/2026-09-26-video-player-background-decode-design.md`](../specs/2026-09-26-video-player-background-decode-design.md) — read it first; it explains *why* each rule exists.

**Status:** executed on `fix/video-player-ui-stall`. Once merged this is a historical record of how the feature was built; the code and `CLAUDE.md` are the reference after that.

---

## Before you start

- Work on the branch `fix/video-player-ui-stall` (it already holds the spec). Never commit to the base branch, `develop`.
- Run every command from the repository root; `gl_smoke` resolves `shaders/` and `tests/assets/` relative to it.
- The build directory is `build/` (configured by `cmake -S . -B build`). Every code block below is complete — copy it exactly.
- This plan was generated from a prototype that passed all of `core_tests`, `gl_smoke` and `render_cli`, and a ThreadSanitizer build of `gl_smoke` with zero reports; each task's "verify it fails" and "verify it passes" outputs were checked by replaying the plan on a fresh copy of the repository.

## File structure

| File | Responsibility |
|---|---|
| `src/core/VideoPlan.h` (new) | Pure playback decisions: pool size, laps and the unwrapped playhead, frame selection, reverse stretches, the worker's next step |
| `src/core/TimedAudio.h` (new) | Audio chunks tagged with unwrapped time; sampling identical to the old `emitAudio` |
| `src/gfx/VideoDecoder.{h,cpp}` (rewrite) | FFmpeg: threaded decode, `decodeNext`/`convert`, packet queue + audio read-ahead, keyframe lookup; times from the first frame (untimed frames, raw streams), audio in runs that follow its timestamps; legacy `decodeFrame` unchanged |
| `src/gfx/VideoEncoder.{h,cpp}` (small change) | Optional fixed keyframe interval, for test clips |
| `src/gfx/VideoStream.{h,cpp}` (new) | The worker thread: pool, ready queue, audio, request/frameAt/readAudio, offline readiness |
| `src/modules/VideoPlayerNode.{h,cpp}` (rewrite) | Playhead, requests, staging-texture upload + flipped blit, offline gating |
| `tests/test_video_plan.cpp`, `tests/test_timed_audio.cpp` (new) | Unit tests |
| `tests/gl_smoke.cpp` (modified) | Decoder, encoder, stream and node scenarios |
| `CMakeLists.txt`, `CLAUDE.md` (modified) | Registration; documentation |

### Task 1: Playback maths: pool size, laps, the playhead, frame selection

**Files:**
- Create: `src/core/VideoPlan.h`
- Create: `tests/test_video_plan.cpp`
- Modify: `CMakeLists.txt` (the `core_tests` source list)

`core/VideoPlan.h` holds every playback decision as a pure function, so it is tested without FFmpeg, threads or GL. Times are *unwrapped*: `u = lap * D + position`, so the playhead keeps counting past the end of the clip (and below 0 in reverse).

- [ ] **Step 1: Write the failing tests** — `tests/test_video_plan.cpp` (complete file)

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include <limits>
#include <vector>
#include "core/OfflineRender.h"
#include "core/VideoPlan.h"

using namespace oss;

TEST_CASE("videoPoolFrames: 512 MB holds 16 frames at 4K, caps at 64, floors at 4") {
    CHECK(videoPoolFrames(kVideoPoolBytes, 3840, 2160) == 16);
    CHECK(videoPoolFrames(kVideoPoolBytes, 1920, 1080) == 64);   // exactly 64 fit
    CHECK(videoPoolFrames(kVideoPoolBytes, 1280, 720)  == 64);   // 145 fit: capped
    CHECK(videoPoolFrames(kVideoPoolBytes, 7680, 4320) == 4);    // 4.04
    CHECK(videoPoolFrames(kVideoPoolBytes, 15360, 8640) == 4);   // 1 would fit: floored at 4
    CHECK(videoPoolFrames(kVideoPoolBytes, 0, 0) == 4);
}

TEST_CASE("videoLapStart / videoWrapped: laps of D, including negative laps") {
    CHECK(videoLapStart(5.0, 2.0) == doctest::Approx(4.0));
    CHECK(videoWrapped(5.0, 2.0) == doctest::Approx(1.0));
    CHECK(videoLapStart(6.0, 2.0) == doctest::Approx(6.0));      // an exact multiple starts the next lap
    CHECK(videoWrapped(6.0, 2.0) == doctest::Approx(0.0));
    CHECK(videoLapStart(-0.5, 2.0) == doctest::Approx(-2.0));    // reverse past 0 is the lap before
    CHECK(videoWrapped(-0.5, 2.0) == doctest::Approx(1.5));
    CHECK(videoLapStart(3.0, 0.0) == doctest::Approx(0.0));      // unknown duration: no laps
    CHECK(videoWrapped(3.0, 0.0) == doctest::Approx(3.0));
}

TEST_CASE("videoLapStart: a lap start maps to itself, however D rounds") {
    CHECK(videoLapStart(1.4 * 3, 1.4) == 1.4 * 3);             // 1.4 * 3 / 1.4 is just below 3
    int bad = 0;
    for (double D : {1.4, 0.1, 1.0 / 3.0, 4.04, 29.97, 3.3366666666666664})
        for (int k = -50; k <= 2000; ++k) {
            const double u = D * k + 0.5 * D;                  // mid-lap, as the node captures it
            const double lo = videoLapStart(u, D);
            if (!(lo <= u && u < lo + D) || videoLapStart(lo, D) != lo) ++bad;
        }
    CHECK(bad == 0);
}

TEST_CASE("videoAdvance: loop on keeps counting past the end; the position wraps") {
    VideoPlayhead p; p.u = 1.9;
    p = videoAdvance(p, true, 1.0, true, 0.2, 2.0);
    CHECK(p.u == doctest::Approx(2.1));
    CHECK(videoPosition(p, true, 2.0) == doctest::Approx(0.1));
    p = videoAdvance(p, true, -1.0, true, 2.5, 2.0);             // reverse runs below 0 too
    CHECK(p.u == doctest::Approx(-0.4));
    CHECK(videoPosition(p, true, 2.0) == doctest::Approx(1.6));
}

TEST_CASE("videoAdvance: loop off clamps to the current lap, so the end holds the last frame") {
    VideoPlayhead p; p.u = 1.9;
    p = videoAdvance(p, true, 1.0, false, 0.2, 2.0);
    CHECK(p.u == doctest::Approx(2.0));
    CHECK(videoPosition(p, false, 2.0) == doctest::Approx(2.0));
    p = videoAdvance(p, true, -1.0, false, 5.0, 2.0);            // and the start holds the first
    CHECK(p.u == doctest::Approx(0.0));

    VideoPlayhead q; q.u = 6.5;                                  // looping in lap 3...
    q = videoAdvance(q, true, 1.0, true, 0.1, 2.0);
    q = videoAdvance(q, true, 1.0, false, 5.0, 2.0);             // ...then loop goes off
    CHECK(q.lapLo == doctest::Approx(6.0));                      // captured from the lap it was in
    CHECK(q.u == doctest::Approx(8.0));
    CHECK(videoPosition(q, false, 2.0) == doctest::Approx(2.0));
}

TEST_CASE("videoAdvance: paused does not move; unknown duration clamps at 0 only") {
    VideoPlayhead p; p.u = 1.0;
    CHECK(videoAdvance(p, false, 1.0, true, 0.5, 2.0).u == doctest::Approx(1.0));
    CHECK(videoAdvance(p, true, 1.0, true, 5.0, 0.0).u == doctest::Approx(6.0));
    CHECK(videoAdvance(p, true, -1.0, true, 5.0, 0.0).u == doctest::Approx(0.0));
}

TEST_CASE("videoSelectFrame: greatest time at or before u, tolerant of float dt noise") {
    const double t[] = {0.0, 0.04, 0.08};
    auto timeOf = [&](int i) { return t[i]; };
    CHECK(videoSelectFrame(3, 0.05, timeOf) == 1);
    CHECK(videoSelectFrame(3, 0.08, timeOf) == 2);
    CHECK(videoSelectFrame(3, 2 * 0.039999999105930328, timeOf) == 2);   // 2 * (float)0.04 < 0.08
    CHECK(videoSelectFrame(3, 1.0, timeOf) == 2);
    CHECK(videoSelectFrame(3, -0.01, timeOf) == -1);
    CHECK(videoSelectFrame(0, 1.0, timeOf) == -1);
}

TEST_CASE("videoAdvance: loop off at the end holds the last frame, not the next lap's first") {
    // Looping, the worker decodes the next lap early: its first frame is tagged at exactly lapLo + D.
    const double t[] = {1.92, 1.96, 2.0};                       // lap 0's last two frames, then lap 1's first
    auto timeOf = [&](int i) { return t[i]; };
    VideoPlayhead p; p.u = 1.9;
    p = videoAdvance(p, true, 1.0, false, 0.2, 2.0);            // loop goes off on the step that crosses the end
    CHECK(videoSelectFrame(3, p.u, timeOf) == 1);
    CHECK(videoPosition(p, false, 2.0) == doctest::Approx(2.0));
    p = videoAdvance(p, true, 1.0, false, 0.5, 2.0);            // and it stays there
    CHECK(videoSelectFrame(3, p.u, timeOf) == 1);
}

// How many of `frames` offline steps land the playhead on the wrong frame of a `fps` grid: after step
// k the frame for u (the greatest k'/fps <= u + kVideoTimeEps) must be frame k.
static long offGridFrames(int fps, double step, long frames) {
    VideoPlayhead p;
    long wrong = 0;
    for (long k = 1; k <= frames; ++k) {
        p = videoAdvance(p, true, 1.0, true, step, 0.0);
        if ((long)std::floor((p.u + kVideoTimeEps) * fps) != k) ++wrong;
    }
    return wrong;
}

TEST_CASE("videoFrameStep: an offline playhead stays on the frame grid for an hour at every render rate") {
    for (int fps : kRenderFrameRates)                           // every rate the renderer offers
        CHECK(offGridFrames(fps, videoFrameStep(1.0f / (float)fps), 3600L * fps) == 0);
    CHECK(offGridFrames(25, (double)(1.0f / 25.0f), 1500) > 0); // the renderer's float step: wrong within a minute
    CHECK(videoFrameStep(0.0123f) == doctest::Approx((double)0.0123f));   // not a whole rate: unchanged
    CHECK(videoFrameStep(0.0f) == 0.0);
}

```

- [ ] **Step 2: Register the test file in `core_tests`, after `tests/test_async_loader.cpp`**. In `CMakeLists.txt`, replace:

```cmake
  tests/test_async_loader.cpp
```

with:

```cmake
  tests/test_async_loader.cpp
  tests/test_video_plan.cpp
```

- [ ] **Step 3: Build to verify it fails**

```bash
cmake --build build --target core_tests -j8
```

Expected: the build FAILS, with an error mentioning `core/VideoPlan.h' file not found`.

- [ ] **Step 4: Write the header** — `src/core/VideoPlan.h` (complete file)

```cpp
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

} // namespace oss
```

- [ ] **Step 5: Build and run the tests (9 test cases)**

```bash
cmake --build build --target core_tests -j8 && ./build/core_tests -tc='video*'
```

Expected output includes: `Status: SUCCESS!`

- [ ] **Step 6: Commit**

```bash
git add src/core/VideoPlan.h tests/test_video_plan.cpp CMakeLists.txt
git commit -F - <<'EOF'
feat(core): add VideoPlan time model, pool sizing and frame selection

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

### Task 2: Reverse stretches

**Files:**
- Modify: `src/core/VideoPlan.h` (append before the closing namespace)
- Modify: `tests/test_video_plan.cpp` (append)

Reverse playback decodes *stretches*: from a keyframe forward to the frame needed, keeping a subset. Live keeps every `stride`-th frame counted back from the stretch's end, within a budget of half the pool; offline keeps consecutive frames so reverse renders are exact. The end is a boundary (often just below the stretch above), so counting starts at the frame whose interval contains it.

- [ ] **Step 1: Append the failing tests** — add to the end of `tests/test_video_plan.cpp`:

```cpp
// The frames a live stretch keeps on an exact grid, as counts back from its top frame (0 = the top).
static std::vector<long> keptBack(const VideoStretch& s, double key, double fd) {
    std::vector<long> k;
    for (double t = key; t <= s.end + 1e-9; t += fd)
        if (videoStretchKeeps(s, t, fd)) k.push_back(std::lround((s.top - t) / fd));
    return k;
}

TEST_CASE("videoPlanStretch: every frame when the stretch fits the budget") {
    const double fd = 1.0 / 30.0;
    VideoStretch s = videoPlanStretch(0.0, 29 * fd, fd, 32, false);   // 30 frames, budget 32
    CHECK(s.stride == 1);
    CHECK(keptBack(s, 0.0, fd).size() == 30);
}

TEST_CASE("videoPlanStretch: every n-th frame otherwise, always keeping the top, within budget") {
    const double fd = 1.0 / 30.0;
    VideoStretch s = videoPlanStretch(0.0, 29 * fd, fd, 8, false);    // 30 frames, budget 8
    CHECK(s.stride == 4);
    std::vector<long> k = keptBack(s, 0.0, fd);
    CHECK(k.size() == 8);
    CHECK(k.front() == 28);                                           // ascending time: earliest first...
    CHECK(k.back() == 0);                                             // ...and the top frame is kept

    VideoStretch l = videoPlanStretch(0.0, 249 * fd, fd, 8, false);   // a 250-frame keyframe interval
    CHECK(l.stride == 32);
    std::vector<long> kl = keptBack(l, 0.0, fd);
    CHECK(kl.size() <= 8);
    CHECK(kl.back() == 0);
    for (std::size_t j = 1; j < kl.size(); ++j) CHECK(kl[j - 1] - kl[j] == l.stride);   // evenly spaced
}

TEST_CASE("videoStretchKeeps: an end between frames counts from the frame containing it") {
    const double fd = 1.0 / 30.0;
    const double end = 29 * fd - 1e-6;                 // just below a keyframe: the stretch above's start
    VideoStretch s = videoPlanStretch(0.0, end, fd, 8, false);
    CHECK(s.stride == 4);                              // 29 frames in [0, end]
    CHECK(videoStretchKeeps(s, 28 * fd, fd));          // the frame just below the boundary is kept
    CHECK_FALSE(videoStretchKeeps(s, 27 * fd, fd));
    CHECK(videoStretchKeeps(s, 24 * fd, fd));
    CHECK_FALSE(videoStretchKeeps(s, 29 * fd, fd));    // the stretch above's keyframe is not
}

TEST_CASE("videoStretchKeeps: stride 1 still rejects a frame past the end") {
    const double fd = 1.0 / 30.0;
    const VideoStretch s = videoPlanStretch(0.0, 29 * fd - kVideoTimeEps, fd, 32, false);
    CHECK(s.stride == 1);
    CHECK(videoStretchKeeps(s, 28 * fd, fd));
    CHECK_FALSE(videoStretchKeeps(s, 29 * fd, fd));
}

// A 60 fps MKV/WebM stores whole milliseconds, so frames sit up to half a millisecond off the grid.
static double msFrameTime(long i) { return (double)std::lround((double)i * 1000.0 / 60.0) / 1000.0; }

TEST_CASE("videoStretchKeeps: millisecond-rounded timestamps keep every stride-th frame, even 1 us below a keyframe") {
    // Counting back from a boundary by flooring made counts repeat and skip: at stride 15 it kept nothing.
    const double fd = 1.0 / 60.0;
    for (long keyAbove = 120; keyAbove <= 4800; keyAbove += 120) {  // 2 s keyframe intervals
        const double end = msFrameTime(keyAbove) - kVideoTimeEps;
        const VideoStretch s = videoPlanStretch(msFrameTime(keyAbove - 120), end, fd, 8, false);
        REQUIRE(s.stride == 15);
        std::vector<long> kept;
        for (long i = keyAbove - 120; msFrameTime(i) <= end + kVideoTimeEps; ++i)
            if (videoStretchKeeps(s, msFrameTime(i), fd)) kept.push_back(i);
        CHECK(!kept.empty());
        CHECK((int)kept.size() <= s.keep);
        CHECK_FALSE(videoStretchKeeps(s, msFrameTime(keyAbove), fd));   // the stretch above's keyframe
        for (std::size_t j = 1; j < kept.size(); ++j) CHECK(kept[j] - kept[j - 1] == s.stride);
    }
}

TEST_CASE("videoStretchKeeps: from the worker's half-frame anchor the top kept frame is just below the stretch above") {
    const double fd = 1.0 / 60.0;
    auto tOf = msFrameTime;
    for (long gop : {100L, 120L}) {                                   // keyframes off and on whole ms
        for (long keyAbove = gop; keyAbove <= 40 * gop; keyAbove += gop) {
            const double end = tOf(keyAbove) - fd / 2;               // the worker's live prefetch anchor
            const VideoStretch s = videoPlanStretch(tOf(keyAbove - gop), end, fd, 8, false);   // 4K budget
            std::vector<long> kept;
            for (long i = keyAbove - gop; tOf(i) <= end + kVideoTimeEps; ++i)
                if (videoStretchKeeps(s, tOf(i), fd)) kept.push_back(i);
            REQUIRE(!kept.empty());
            CHECK((int)kept.size() <= s.keep);
            CHECK(kept.back() == keyAbove - 1);                       // the frame just below the stretch above
            for (std::size_t j = 1; j < kept.size(); ++j) CHECK(kept[j] - kept[j - 1] == s.stride);
        }
    }
}

TEST_CASE("videoPlanStretch: degenerate inputs stay defined") {
    VideoStretch s = videoPlanStretch(0.0, 1.0, 1.0 / 30.0, 0, false);    // no budget: keep one
    CHECK(s.keep == 1);
    s = videoPlanStretch(0.0, 1.0, 0.0, 8, false);                      // no frame rate: keep them all
    CHECK(s.stride == 1);
    CHECK(videoStretchKeeps(s, 0.5, 0.0));
    s = videoPlanStretch(0.0, 1.0, std::nan(""), 8, false);
    CHECK(s.stride == 1);
    s = videoPlanStretch(2.0, 1.0, 1.0 / 30.0, 8, false);               // end before the key: one frame
    CHECK(s.stride == 1);
    CHECK(s.top == doctest::Approx(2.0));
}

TEST_CASE("videoPlanStretch: offline keeps consecutive frames (a ring of the newest)") {
    const double fd = 1.0 / 30.0;
    VideoStretch s = videoPlanStretch(0.0, 249 * fd, fd, 8, true);
    CHECK(s.contiguous);
    CHECK(s.stride == 1);
    CHECK(s.keep == 8);
    CHECK(keptBack(s, 0.0, fd).size() == 250);                        // all converted; the ring keeps 8
}

```

- [ ] **Step 2: Build to verify it fails**

```bash
cmake --build build --target core_tests -j8
```

Expected: the build FAILS, with an error mentioning `videoPlanStretch`.

- [ ] **Step 3: Add the stretch planner, just above `} // namespace oss`** — in `src/core/VideoPlan.h`:

```cpp
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

```

- [ ] **Step 4: Build and run the tests (17 test cases)**

```bash
cmake --build build --target core_tests -j8 && ./build/core_tests -tc='video*'
```

Expected output includes: `Status: SUCCESS!`

- [ ] **Step 5: Commit**

```bash
git add src/core/VideoPlan.h tests/test_video_plan.cpp
git commit -F - <<'EOF'
feat(core): plan reverse stretches for the video worker

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

### Task 3: The worker's next-step planner

**Files:**
- Modify: `src/core/VideoPlan.h` (append before the closing namespace)
- Modify: `tests/test_video_plan.cpp` (append)

`videoNextStep()` is the worker's whole decision table. Forward: seek when the target is behind everything held (unless the last seek was *pinned* because the target precedes the file's first frame); when the target is more than 2 frames ahead of the decoder, seek only if a keyframe lies between (the next lap's start counts) **and** the gap is over 1 s (a seek restarts FFmpeg's frame-threading pipeline), else catch up; wrap or wait at the lap's end; fill a free buffer; else wait. With loop off, a decoder left a lap behind the playhead (it fell behind while looping) seeks into the playhead's lap — `lapEnd` is the decoder's lap end whether looping or not. An index keyframe counts as `kVideoIndexLead` frames later than listed: indexes list B-frame streams' keyframes by decode time — x264 2 frames early, x265's open GOP up to 6 — and a seek decided inside that window lands a whole keyframe interval back (the next lap's start is exact). A seek ahead that landed at or behind the decoder anyway (no index) is not repeated within the lap until the target has moved on by the gap it revealed (`videoNoSeekBelow`). Reverse: a fresh stretch when the run is new, the target fell below what is covered, or the playhead is *stranded* above it — stopped (paused or offline: no lead), or more than `kVideoRestartLeads` leads above — aimed `lead` below a moving playhead; prefetch the stretch below once a stretch's budget (`videoStretchBudget`, half the pool) is free; wait at the start of the clip with loop off. First, one line of Task 2's anchor test changes so it admits frames exactly as the worker's prefetch does (strictly below the stretch above) — it passes before and after.

- [ ] **Step 1: In Task 2's anchor test, admit frames the way the worker's prefetch does: strictly below the stretch above, not up to the anchor**. In `tests/test_video_plan.cpp`, replace:

```cpp
            for (long i = keyAbove - gop; tOf(i) <= end + kVideoTimeEps; ++i)
```

with:

```cpp
            for (long i = keyAbove - gop; tOf(i) <= tOf(keyAbove) - kVideoTimeEps; ++i)   // what it admits
```

- [ ] **Step 2: Append the failing tests** — add to the end of `tests/test_video_plan.cpp`:

```cpp
static const double kInf = std::numeric_limits<double>::infinity();

// A forward input with a comfortable default state: the decoder is just past the target, in lap 0.
static VideoPlanInput fwd(double target) {
    VideoPlanInput in;
    in.target = target; in.dir = 1; in.loop = true; in.duration = 10.0; in.frameDur = 0.04;
    in.head = target + 0.04; in.lowest = target; in.keyKnown = true; in.nextKey = kInf; in.lapEnd = 10.0;
    in.freeBuffers = 3; in.poolSize = 16;
    return in;
}

TEST_CASE("videoStretchBudget: half the pool, at least one frame") {
    CHECK(videoStretchBudget(16) == 8);
    CHECK(videoStretchBudget(5) == 2);
    CHECK(videoStretchBudget(3) == 1);
    CHECK(videoStretchBudget(1) == 1);
    CHECK(videoStretchBudget(0) == 1);
}

TEST_CASE("videoNoSeekBelow: a seek ahead that landed at or behind the decoder is not worth repeating") {
    CHECK(videoNoSeekBelow(6.5, 2.56, 0.0) == 6.5 + 6.5);      // landed a whole gap back: wait that long again
    CHECK(videoNoSeekBelow(6.5, 5.0, 4.8) == 6.5 + kVideoSeekNoIndex);   // a short way back: at least this long
    CHECK(videoNoSeekBelow(6.5, 2.56, 2.56) == 6.5 + (6.5 - 2.56));      // exactly at the head counts
    CHECK(videoNoSeekBelow(6.5, 2.56, 6.0) == -kInf);          // it got ahead: seeking works here
    CHECK(videoNoSeekBelow(1.0, 2.56, 0.0) == -kInf);          // a seek back is meant to land behind
    CHECK(videoNoSeekBelow(6.5, 2.56, kInf) == -kInf);         // it found no frame at all
}

TEST_CASE("videoNextStep forward: fill while a buffer is free, else wait") {
    VideoPlanInput in = fwd(1.0);
    CHECK(videoNextStep(in).kind == VideoStepKind::Fill);
    in.freeBuffers = 0;
    CHECK(videoNextStep(in).kind == VideoStepKind::Wait);
}

TEST_CASE("videoNextStep forward: far behind -> catch up, or seek when a keyframe lies between") {
    VideoPlanInput in = fwd(3.0);
    in.head = 1.0; in.lowest = 0.96;                   // 2 s behind
    in.nextKey = 5.0;                                  // no keyframe before the target
    VideoStep s = videoNextStep(in);
    CHECK(s.kind == VideoStepKind::CatchUp);
    CHECK(s.to == 3.0);
    in.nextKey = 2.5;                                  // a keyframe on the way: jump to it
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
    in.head = 2.5; in.nextKey = 2.7;                   // but a short gap is cheaper to decode through
    CHECK(videoNextStep(in).kind == VideoStepKind::CatchUp);
    in.head = 2.95;                                    // within kVideoCatchUpFrames: just fill
    CHECK(videoNextStep(in).kind == VideoStepKind::Fill);
}

TEST_CASE("videoNextStep forward: an index keyframe just below the target may be a few frames later") {
    VideoPlanInput in = fwd(19.95);                    // x264's B-frames: the keyframe shown at 20.0 is
    in.duration = 30.0; in.lapEnd = 30.0;              // listed at its decode time, 19.92
    in.head = 12.04; in.lowest = 12.0; in.nextKey = 19.92;
    CHECK(videoNextStep(in).kind == VideoStepKind::CatchUp);   // a seek for 19.95 would land at 10
    in.nextKey = 19.76; in.target = 19.88;             // x265's open GOP: listed 6 frames early
    CHECK(videoNextStep(in).kind == VideoStepKind::CatchUp);
    in.nextKey = 19.92; in.target = 20.3;              // well past it: the keyframe is surely between
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
    in = fwd(10.05);                                   // the next lap's start is exact, index or not
    in.keyKnown = false; in.head = 8.9; in.lowest = 8.86;
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
}

TEST_CASE("videoNextStep forward: the seek thresholds are strict") {
    VideoPlanInput in = fwd(3.0);
    in.head = 2.0; in.lowest = 1.96; in.nextKey = 2.5; // a keyframe between, exactly kVideoSeekMinJump behind
    CHECK(videoNextStep(in).kind == VideoStepKind::CatchUp);
    in.head = 1.99;
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
    in.keyKnown = false;                               // no index: exactly kVideoSeekNoIndex behind
    in.head = 1.0; in.lowest = 0.96;
    CHECK(videoNextStep(in).kind == VideoStepKind::CatchUp);
    in.head = 0.99;
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
}

TEST_CASE("videoNextStep forward: without a keyframe index, seek only beyond kVideoSeekNoIndex") {
    VideoPlanInput in = fwd(3.0);
    in.keyKnown = false;
    in.head = 1.5; in.lowest = 1.46;
    CHECK(videoNextStep(in).kind == VideoStepKind::CatchUp);
    in.head = 0.5; in.lowest = 0.46;
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
}

TEST_CASE("videoNextStep forward: a seek that landed behind the decoder is not repeated within the lap") {
    VideoPlanInput in = fwd(6.5);                      // the target jumped 4 s ahead; no index (MPEG-TS)
    in.duration = 30.0; in.lapEnd = 30.0; in.keyKnown = false;
    in.head = 1.63; in.lowest = 1.59;                  // an earlier seek for 6.5 landed on the keyframe at 0
    in.noSeekBelow = videoNoSeekBelow(6.5, 2.56, 0.0); // ...behind where the decoder was: catch up instead
    VideoStep s = videoNextStep(in);
    CHECK(s.kind == VideoStepKind::CatchUp);
    CHECK(s.to == 6.5);
    in.keyKnown = true; in.nextKey = 2.4;              // an index that lies (MPEG-TS lists the seek's probes)
    CHECK(videoNextStep(in).kind == VideoStepKind::CatchUp);
    in.target = 12.9;                                  // not until it has moved on by that gap
    CHECK(videoNextStep(in).kind == VideoStepKind::CatchUp);
    in.target = 13.1;                                  // then try again
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
    in.target = 30.3; in.noSeekBelow = 40.0;           // in the next lap a seek lands at its start, ahead
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
}

TEST_CASE("videoNextStep forward: a target in the next lap seeks -- the lap starts with a keyframe") {
    VideoPlanInput in = fwd(11.0);                     // looping; the decoder is still in lap 0
    in.head = 9.0; in.lowest = 8.96;                   // the frame on screen is just behind the head
    in.nextKey = kInf;                                 // the index has no keyframe left in this lap
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
    in.nextKey = 9.5;                                  // ...and a keyframe later in this lap still counts
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
    in.keyKnown = false;                               // no index: the next lap's start still counts
    in.target = 11.5; in.head = 9.8; in.lowest = 9.76;
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
    in.target = 10.3;                                  // just into the next lap: decode through the wrap
    CHECK(videoNextStep(in).kind == VideoStepKind::CatchUp);
}

TEST_CASE("videoNextStep forward: a target behind everything held seeks, unless pinned there") {
    VideoPlanInput in = fwd(1.0);
    in.lowest = 2.0; in.head = 2.5;                    // the target is behind the whole queue
    VideoStep s = videoNextStep(in);
    CHECK(s.kind == VideoStepKind::Seek);
    CHECK(s.to == 1.0);
    in.seekPinned = true; in.pinnedFrom = 0.5;         // the last seek for 0.5 landed after it
    CHECK(videoNextStep(in).kind == VideoStepKind::Fill);
    in.target = in.pinnedFrom - kVideoTimeEps / 2;     // at the pinned target, within rounding: still pinned
    CHECK(videoNextStep(in).kind == VideoStepKind::Fill);
    in.target = 0.2;                                   // but further back is a new request
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
}

TEST_CASE("videoNextStep forward: seeking back comes first, even at the end of the lap") {
    VideoPlanInput in = fwd(1.0);
    in.lowest = 9.0; in.head = 10.0; in.eof = true;    // the decoder finished the lap; the target is far behind
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
    in = fwd(1.0);                                     // loop off, the decoder past the playhead's lap
    in.loop = false; in.duration = 2.0; in.lapLo = 0.0; in.lapHi = 2.0; in.lapEnd = 4.0;
    in.head = 2.2; in.lowest = 1.96; in.dirChanged = true;
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
}

TEST_CASE("videoNextStep forward: end of the lap wraps when looping, else waits") {
    VideoPlanInput in = fwd(9.9);
    in.eof = true;
    CHECK(videoNextStep(in).kind == VideoStepKind::Wrap);
    in.loop = false; in.lapLo = 0.0; in.lapHi = 10.0;
    CHECK(videoNextStep(in).kind == VideoStepKind::Wait);
    in.loop = true; in.duration = 0.0; in.lapEnd = kInf;   // unknown duration cannot wrap
    CHECK(videoNextStep(in).kind == VideoStepKind::Wait);
    in.duration = 10.0; in.lapEnd = 10.0; in.target = 12.0;   // far into the next lap: wrap first
    CHECK(videoNextStep(in).kind == VideoStepKind::Wrap);
}

TEST_CASE("videoNextStep forward: loop off with the decoder already past the lap waits") {
    VideoPlanInput in = fwd(1.99);                     // loop just went off near the end of lap 0...
    in.loop = false; in.duration = 2.0; in.lapLo = 0.0; in.lapHi = 2.0;
    in.head = 2.2; in.lapEnd = 4.0;                    // ...but the decoder had run on into lap 1,
    CHECK(videoNextStep(in).kind == VideoStepKind::Wait);
    in.head = 2.0;                                     // ...or had just wrapped to its start
    CHECK(videoNextStep(in).kind == VideoStepKind::Wait);
    in.target = 1.9; in.lowest = 1.9; in.head = 1.94; in.lapEnd = 2.0;   // still inside the lap: keep filling
    CHECK(videoNextStep(in).kind == VideoStepKind::Fill);
}

TEST_CASE("videoNextStep forward: loop off with the decoder a lap behind seeks into the playhead's lap") {
    VideoPlanInput in = fwd(4.3);                      // loop went off after the playhead crossed into lap 1...
    in.loop = false; in.duration = 4.0; in.lapLo = 4.0; in.lapHi = 8.0;
    in.head = 4.0; in.eof = true; in.lapEnd = 4.0; in.lowest = 3.96;   // ...while the decoder finished lap 0
    VideoStep s = videoNextStep(in);
    CHECK(s.kind == VideoStepKind::Seek);
    CHECK(s.to == 4.3);
    in.eof = false; in.head = 2.56; in.lowest = 2.52;  // or before it got there
    CHECK(videoNextStep(in).kind == VideoStepKind::Seek);
    in.head = 4.36; in.lowest = 4.28; in.lapEnd = 8.0; // once in the playhead's lap, the usual rules
    CHECK(videoNextStep(in).kind == VideoStepKind::Fill);
}

TEST_CASE("videoNextStep forward: a direction change always seeks") {
    VideoPlanInput in = fwd(1.0);
    in.dirChanged = true;
    VideoStep s = videoNextStep(in);
    CHECK(s.kind == VideoStepKind::Seek);
    CHECK(s.to == 1.0);
}

static VideoPlanInput rev(double target) {
    VideoPlanInput in;
    in.target = target; in.dir = -1; in.loop = true; in.duration = 10.0; in.frameDur = 0.04;
    in.freeBuffers = 8; in.poolSize = 16;
    in.coverValid = true; in.coverLo = target - 1.0; in.coverHi = target + 0.5;   // the playhead is inside
    return in;
}

TEST_CASE("videoNextStep reverse: a new run starts a fresh stretch at the target") {
    VideoPlanInput in = rev(5.0);
    in.coverValid = false;
    VideoStep s = videoNextStep(in);
    CHECK(s.kind == VideoStepKind::Reverse);
    CHECK(s.fresh);
    CHECK(s.to == 5.0);
    in = rev(5.0); in.dirChanged = true;
    s = videoNextStep(in);
    CHECK(s.kind == VideoStepKind::Reverse);
    CHECK(s.fresh);
    CHECK(s.to == 5.0);
}

TEST_CASE("videoNextStep reverse: falling below the covered stretch jumps with a fresh stretch") {
    VideoPlanInput in = rev(5.0);
    in.coverLo = 5.5; in.coverHi = 6.0;
    VideoStep s = videoNextStep(in);
    CHECK(s.kind == VideoStepKind::Reverse);
    CHECK(s.fresh);
    CHECK(s.to == 5.0);
}

TEST_CASE("videoNextStep reverse: a fresh stretch aims where the playhead will be when it is decoded") {
    VideoPlanInput in = rev(5.0);
    in.coverLo = 5.5; in.coverHi = 6.0;                // fell below: the last stretch took 0.6 s at rate -1
    in.lead = 0.6;
    VideoStep s = videoNextStep(in);
    CHECK(s.fresh);
    CHECK(s.to == 5.0 - 0.6);
    in.loop = false; in.lapLo = 4.0; in.lapHi = 14.0;  // with loop off, inside the lap, it aims the same
    CHECK(videoNextStep(in).to == 5.0 - 0.6);
    in.target = 0.2; in.coverLo = 0.5;                 // but never before the clip starts
    in.lapLo = 0.0; in.lapHi = 10.0;
    CHECK(videoNextStep(in).to == 0.0);
    in.loop = true;                                    // looping, it may aim into the lap before
    CHECK(videoNextStep(in).to == 0.2 - 0.6);
}

TEST_CASE("videoNextStep reverse: a playhead still above the covered stretch is not restarted") {
    VideoPlanInput in = rev(5.0);
    in.coverLo = 4.0; in.coverHi = 4.6;                // a led stretch landed early: [4.0, 4.6] is covered
    in.target = 4.9; in.lead = 0.6;                    // and the moving playhead is on its way down into it
    VideoStep s = videoNextStep(in);
    CHECK(s.kind == VideoStepKind::Reverse);           // so the worker prefetches below instead
    CHECK_FALSE(s.fresh);
    CHECK(s.to == 4.0);
}

TEST_CASE("videoNextStep reverse: a stranded playhead above the covered stretch restarts there") {
    VideoPlanInput in = rev(5.0);
    in.coverLo = 4.0; in.coverHi = 4.6;                // the led stretch landed below the playhead...
    in.target = 4.9; in.lead = 0.0;                    // ...which then stopped (paused): it never arrives
    VideoStep s = videoNextStep(in);
    CHECK(s.kind == VideoStepKind::Reverse);
    CHECK(s.fresh);
    CHECK(s.to == 4.9);
    in.target = 4.6;                                   // at the top of what is covered, it is served
    CHECK_FALSE(videoNextStep(in).fresh);
    in.target = 5.5; in.lead = 0.05;                   // slowed to a crawl 0.9 s above: arriving would take
    s = videoNextStep(in);                             // many stretches' time
    CHECK(s.fresh);
    CHECK(s.to == 5.5 - 0.05);
    in.target = 4.6 + kVideoRestartLeads * 0.05 + 0.04 - 0.001;   // within a few leads: it arrives soon
    CHECK_FALSE(videoNextStep(in).fresh);
}

TEST_CASE("videoNextStep reverse: prefetch the stretch below once a stretch's budget is free") {
    VideoPlanInput in = rev(5.0);
    VideoStep s = videoNextStep(in);
    CHECK(s.kind == VideoStepKind::Reverse);
    CHECK_FALSE(s.fresh);
    CHECK(s.to == in.coverLo);                         // the stretch just below what is covered
    in.freeBuffers = videoStretchBudget(in.poolSize) - 1;
    CHECK(videoNextStep(in).kind == VideoStepKind::Wait);
}

TEST_CASE("videoNextStep reverse: the start of the clip waits with loop off, continues with loop on") {
    VideoPlanInput in = rev(0.3);
    in.coverLo = 0.0;
    in.loop = false; in.lapLo = 0.0; in.lapHi = 10.0;
    CHECK(videoNextStep(in).kind == VideoStepKind::Wait);
    in.loop = true;                                    // looping: the previous lap's end comes next
    VideoStep s = videoNextStep(in);
    CHECK(s.kind == VideoStepKind::Reverse);
    CHECK_FALSE(s.fresh);
    CHECK(s.to == 0.0);                                // just below the lap start: the lap before
    in.duration = 0.0; in.loop = false;                // unknown duration: no laps, the lap is [0, inf)
    in.lapHi = kInf;
    CHECK(videoNextStep(in).kind == VideoStepKind::Wait);
}
```

- [ ] **Step 3: Build to verify it fails**

```bash
cmake --build build --target core_tests -j8
```

Expected: the build FAILS, with an error mentioning `VideoPlanInput`.

- [ ] **Step 4: Add the planner, just above `} // namespace oss`** — in `src/core/VideoPlan.h`:

```cpp
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

```

- [ ] **Step 5: Build and run the tests (39 test cases)**

```bash
cmake --build build --target core_tests -j8 && ./build/core_tests -tc='video*'
```

Expected output includes: `Status: SUCCESS!`

- [ ] **Step 6: Commit**

```bash
git add src/core/VideoPlan.h tests/test_video_plan.cpp
git commit -F - <<'EOF'
feat(core): add the video worker's next-step planner

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

### Task 4: TimedAudio: a time-tagged audio store

**Files:**
- Create: `src/core/TimedAudio.h`
- Create: `tests/test_timed_audio.cpp`
- Modify: `CMakeLists.txt` (the `core_tests` source list)

The worker's audio, as chunks tagged with unwrapped time. `sample()` reproduces the node's old `emitAudio` mapping exactly; uncovered time is silence; a chunk covers up to the end of its last sample, so adjacent chunks tile; where chunks overlap the most recently begun one wins (the new lap at a loop seam; in reverse the stretch below, since the one above resumes its audio just before its keyframe out of a flushed decoder); `append(..., clipHi)` keeps the samples that start before the stretch above; `retain(lo, hi, u)` keeps the playhead's neighbourhood (trimming chunk backs in reverse), and a 180 s cap drops the audio farthest from the playhead first, never what plays at it — a whole chunk, or the far end of the one being filled (in live reverse that is the prefetch below the stretch that plays next). The worker calls `retain()` every step under its mutex, so it compacts in place and allocates nothing.

- [ ] **Step 1: Write the failing tests** — `tests/test_timed_audio.cpp` (complete file)

```cpp
#include <doctest/doctest.h>
#include <limits>
#include <vector>
#include "core/TimedAudio.h"

using namespace oss;

static const double kInfT = std::numeric_limits<double>::infinity();

// A chunk whose sample i holds the value i, starting at `start`.
static void addRamp(TimedAudio& a, double start, int n) {
    std::vector<float> s((std::size_t)n);
    for (int i = 0; i < n; ++i) s[(std::size_t)i] = (float)i;
    a.beginChunk(start);
    a.append(s.data(), s.size());
}

TEST_CASE("TimedAudio: sampling a span inside a chunk matches the old emitAudio mapping") {
    TimedAudio a(48000);
    addRamp(a, 1.0, 1000);
    std::vector<float> out(100);
    a.sample(1.0, 1.0 + 100.0 / 48000.0, out.data(), 100);      // one source sample per output sample
    for (int j = 0; j < 100; ++j) CHECK(out[(std::size_t)j] == doctest::Approx((float)j));
    a.sample(1.0, 1.0 + 1.0 / 48000.0, out.data(), 2);           // half-sample steps interpolate
    CHECK(out[1] == doctest::Approx(0.5f));
}

TEST_CASE("TimedAudio: a reversed span reads backwards") {
    TimedAudio a(48000);
    addRamp(a, 0.0, 1000);
    std::vector<float> out(10);
    a.sample(500.0 / 48000.0, 490.0 / 48000.0, out.data(), 10);
    for (int j = 0; j < 10; ++j) CHECK(out[(std::size_t)j] == doctest::Approx((float)(500 - j)));
}

TEST_CASE("TimedAudio: times no chunk covers are silence") {
    TimedAudio a(48000);
    addRamp(a, 1.0, 480);                                         // [1.0, 1.01)
    float v = -1.0f;
    a.sample(0.5, 0.5, &v, 1);    CHECK(v == 0.0f);                // before
    a.sample(1.02, 1.02, &v, 1);  CHECK(v == 0.0f);                // after
    TimedAudio empty;
    empty.sample(0.0, 1.0, &v, 1); CHECK(v == 0.0f);
}

TEST_CASE("TimedAudio: a chunk covers its last interval, so adjacent chunks tile (a loop seam)") {
    TimedAudio a(1000);
    std::vector<float> ones(1000, 1.0f), twos(1000, 2.0f);
    a.beginChunk(0.0); a.append(ones.data(), ones.size());       // lap 0's audio ends exactly at D = 1
    a.beginChunk(1.0); a.append(twos.data(), twos.size());       // lap 1 starts at D
    std::vector<float> out(40);
    a.sample(0.98, 1.02, out.data(), 40);                         // one output sample per source sample
    for (int j = 0; j < 40; ++j) CHECK(out[(std::size_t)j] == doctest::Approx(j < 20 ? 1.0f : 2.0f));
    float v = 0.0f;
    a.sample(0.9995, 0.9995, &v, 1); CHECK(v == doctest::Approx(1.0f));   // the last interval holds
}

TEST_CASE("TimedAudio: where chunks overlap, the most recently begun wins") {
    TimedAudio a(1000);
    std::vector<float> ones(2000, 1.0f), twos(2000, 2.0f);
    a.beginChunk(0.0); a.append(ones.data(), ones.size());       // [0, 2)
    a.beginChunk(1.5); a.append(twos.data(), twos.size());       // [1.5, 3.5): a loop seam, the new lap
    float v = 0.0f;
    a.sample(1.0, 1.0, &v, 1); CHECK(v == doctest::Approx(1.0f));
    a.sample(1.6, 1.6, &v, 1); CHECK(v == doctest::Approx(2.0f));
    a.sample(3.0, 3.0, &v, 1); CHECK(v == doctest::Approx(2.0f));
    // Reverse: the stretch above was decoded first, and its audio resumes a little before its keyframe
    // (1.0) out of a just-flushed decoder. The stretch below, decoded next and clipped at that keyframe,
    // runs on unbroken there -- and wins, though it starts earlier.
    TimedAudio r(1000);
    std::vector<float> above(2000, 2.0f), below(3000, 1.0f);
    r.beginChunk(0.9);  r.append(above.data(), above.size());
    r.beginChunk(-1.0); r.append(below.data(), below.size(), 1.0);
    r.sample(0.95, 0.95, &v, 1); CHECK(v == doctest::Approx(1.0f));
    r.sample(1.50, 1.50, &v, 1); CHECK(v == doctest::Approx(2.0f));
}

TEST_CASE("TimedAudio: append keeps the samples that start before clipHi") {
    TimedAudio a(1000);
    std::vector<float> s(100, 1.0f);
    a.beginChunk(0.0);
    a.append(s.data(), s.size(), 0.0105);                         // samples at 0..10 ms start before 10.5 ms
    CHECK(a.size() == 11);
    a.append(s.data(), s.size(), 0.0105);                         // the next would start at 11 ms: none
    CHECK(a.size() == 11);
    TimedAudio g(48000);                                          // a clip on the sample grid, computed
    std::vector<float> big(120000, 1.0f);                         // (2.19 - 0.01) * 48000 = 104640.000...015:
    g.beginChunk(0.01);                                           // the sample starting AT 2.19 stays out
    g.append(big.data(), big.size(), 2.19);
    CHECK(g.size() == 104640);
    TimedAudio none;
    none.append(s.data(), s.size());                              // no chunk begun: no-op
    CHECK(none.size() == 0);
}

TEST_CASE("TimedAudio: retain drops chunks outside the window and trims fronts once a second is stale") {
    TimedAudio a(1000);
    std::vector<float> s(5000, 1.0f);
    a.beginChunk(0.0);  a.append(s.data(), s.size());            // [0, 5)
    a.beginChunk(10.0); a.append(s.data(), s.size());            // [10, 15): current
    a.retain(6.0, kInfT, 8.0);                                    // the first chunk ends before 6
    CHECK(a.size() == 5000);
    a.retain(10.5, kInfT, 12.5);                                  // under a second into the chunk: kept
    CHECK(a.size() == 5000);
    a.retain(12.5, kInfT, 14.5);                                  // 2.5 s in: trimmed
    CHECK(a.size() == 2500);
    float v = 0.0f;
    a.sample(13.0, 13.0, &v, 1); CHECK(v == doctest::Approx(1.0f));
}

TEST_CASE("TimedAudio: in reverse, retain trims what the playhead has passed from the backs of chunks") {
    TimedAudio a(1000);
    std::vector<float> s(10000, 1.0f);
    a.beginChunk(10.0); a.append(s.data(), s.size());            // [10, 20): the stretch being played
    a.beginChunk(0.0);  a.append(s.data(), s.size(), 10.0);      // [0, 10): the stretch below, current
    a.retain(-kInfT, 12.0 + 0.5, 12.0);                           // the playhead at 12, keeping 0.5 s above
    CHECK(a.size() == 10000 + 2500);                              // [10, 12.5) and [0, 10)
    float v = 0.0f;
    a.sample(12.4, 12.4, &v, 1); CHECK(v == doctest::Approx(1.0f));
    a.sample(12.6, 12.6, &v, 1); CHECK(v == 0.0f);
    a.retain(-kInfT, 12.0, 11.5);                                 // under a second more: left alone
    CHECK(a.size() == 10000 + 2500);
}

TEST_CASE("TimedAudio: the cap drops the audio farthest from the playhead, never what plays there") {
    const int rate = 100;
    const double cap = TimedAudio::kMaxSeconds;
    float v = 0.0f;
    auto chunk = [&](TimedAudio& a, double start, double seconds, float value, double clipHi = kInfT) {
        std::vector<float> s((std::size_t)(seconds * rate), value);
        a.beginChunk(start);
        a.append(s.data(), s.size(), clipHi);
    };
    auto at = [&](const TimedAudio& a, double t) { a.sample(t, t, &v, 1); return v; };
    {                                                             // before any retain(): the oldest goes
        TimedAudio a(rate);
        chunk(a, 0.0, 0.5 * cap + 1.0, 1.0f);
        chunk(a, 0.5 * cap + 1.0, 0.5 * cap + 1.0, 2.0f);
        CHECK(a.size() == (std::size_t)((0.5 * cap + 1.0) * rate));
        CHECK(at(a, 1.0) == 0.0f);
        CHECK(at(a, cap) == doctest::Approx(2.0f));
    }
    {                                                             // whole chunks go farthest first
        TimedAudio a(rate);
        chunk(a, -140.0, 60.0, 1.0f);                             // 110 s below the playhead
        chunk(a, -70.0, 60.0, 2.0f);                              // 40 s below
        chunk(a, 0.0, 60.0, 3.0f);                                // playing
        a.retain(-kInfT, 30.0 + 2.0, 30.0);
        chunk(a, 60.0, 5.0, 4.0f);                                // being filled, 30 s above: over the cap
        CHECK(at(a, -110.0) == 0.0f);
        CHECK(at(a, -40.0) == doctest::Approx(2.0f));
        CHECK(at(a, 30.0) == doctest::Approx(3.0f));
        CHECK(at(a, 62.0) == doctest::Approx(4.0f));
    }
    {                                                             // live reverse: the prefetch being filled lies
        TimedAudio a(rate);                                       // below the stretch that plays next, so its
        chunk(a, 0.0, 3.0, 3.0f);                                 // far end goes before that stretch does
        a.retain(-kInfT, 1.5 + 2.0, 1.5);                         // (what is left of the stretch playing)
        chunk(a, -100.0, 100.0, 2.0f, 0.0);                       // the stretch below: plays next
        chunk(a, -200.0, 100.0, 1.0f, -100.0);                    // the prefetch below that: over the cap
        CHECK(at(a, 1.5) == doctest::Approx(3.0f));
        CHECK(at(a, -1.0) == doctest::Approx(2.0f));              // the next stretch is whole...
        CHECK(at(a, -99.0) == doctest::Approx(2.0f));
        CHECK(at(a, -101.0) == doctest::Approx(1.0f));            // ...the prefetch keeps its near end...
        CHECK(at(a, -199.0) == 0.0f);                             // ...and loses its far end
        CHECK(a.size() <= (std::size_t)(cap * rate));
    }
    {                                                             // measured from the playhead, not the window
        TimedAudio a(rate);                                       // (keyframes 89.5 s apart): the stretch playing
        chunk(a, 99.9, 90.0, 3.0f);                               // reaches down to the playhead's lap...
        chunk(a, 10.4, 90.0, 2.0f, 100.0);                        // ...where the one below now plays
        a.retain(-kInfT, 99.5 + 2.0, 99.5);
        chunk(a, -79.1, 90.0, 1.0f, 10.4);                        // over the cap
        CHECK(at(a, 99.5) == doctest::Approx(2.0f));
        CHECK(a.size() <= (std::size_t)(cap * rate));
    }
    {                                                             // one stretch alone over the cap keeps the
        TimedAudio a(rate);                                       // cap's worth nearest the playhead
        a.retain(-kInfT, 600.0 + 2.0, 600.0);
        chunk(a, 0.0, 600.0, 1.0f);                               // a keyframe 10 minutes below
        CHECK(a.size() == (std::size_t)(cap * rate));
        CHECK(at(a, 599.0) == doctest::Approx(1.0f));
        CHECK(at(a, 600.0 - cap - 1.0) == 0.0f);
    }
    {                                                             // the playhead near the start of the one being
        TimedAudio a(rate);                                       // filled: its back goes
        a.retain(10.0 - 2.0, kInfT, 10.0);
        chunk(a, 10.0, cap + 10.0, 1.0f);
        CHECK(a.size() == (std::size_t)(cap * rate));
        CHECK(at(a, 10.0) == doctest::Approx(1.0f));
        CHECK(at(a, cap + 5.0) == doctest::Approx(1.0f));        // [10, cap + 10) stays...
        CHECK(at(a, cap + 15.0) == 0.0f);                         // ...the last 10 s went
    }
    {                                                             // never past the playhead, at either end
        TimedAudio a(rate);
        a.retain(-kInfT, 260.0 + 2.0, 260.0);                     // front: the playhead at 52%
        chunk(a, 0.0, 500.0, 1.0f);
        CHECK(at(a, 260.0) == doctest::Approx(1.0f));
        CHECK(at(a, 259.0) == 0.0f);
        TimedAudio b(rate);
        b.retain(240.0 - 2.0, kInfT, 240.0);                      // back: the playhead at 48%
        chunk(b, 0.0, 500.0, 1.0f);
        CHECK(at(b, 240.0) == doctest::Approx(1.0f));
        CHECK(at(b, 241.0) == 0.0f);
    }
}

TEST_CASE("TimedAudio: the cap stops once only what plays is left, and a tie never costs it") {
    const int rate = 100;
    float v = 0.0f;
    {                                                             // over the cap with nothing to take but what
        TimedAudio a(rate);                                       // plays: the one being filled gives all it
        a.retain(-kInfT, 262.0, 260.0);                           // can, then the cap stops -- it neither erases
        std::vector<float> big(50000, 3.0f), small(5000, 1.0f);  // the chunk playing nor spins on the empty one
        a.beginChunk(0.0); a.append(big.data(), big.size());     // [0, 500) -> [260, 500): over the cap, playing
        a.beginChunk(-100.0); a.append(small.data(), small.size(), 0.0);   // being filled, far below: emptied
        a.sample(300.0, 300.0, &v, 1); CHECK(v == doctest::Approx(3.0f));
        a.sample(-60.0, -60.0, &v, 1); CHECK(v == 0.0f);
    }
    {                                                             // offline reverse at a stretch boundary: the
        TimedAudio a(rate);                                       // one being filled ends AT the playhead, as
        std::vector<float> p(10000, 2.0f), f(10000, 1.0f);       // near as the one playing
        a.beginChunk(10.0); a.append(p.data(), p.size());        // [10, 110): playing at 10
        a.retain(-kInfT, 12.0, 10.0);
        a.beginChunk(-90.0); a.append(f.data(), f.size(), 10.0); // [-90, 10): over the cap
        a.sample(10.0, 10.0, &v, 1);   CHECK(v == doctest::Approx(2.0f));
        a.sample(50.0, 50.0, &v, 1);   CHECK(v == doctest::Approx(2.0f));
        a.sample(-85.0, -85.0, &v, 1); CHECK(v == 0.0f);         // the one being filled lost its far end
        a.sample(5.0, 5.0, &v, 1);     CHECK(v == doctest::Approx(1.0f));
    }
}

TEST_CASE("TimedAudio: reverse trimming stops on the sample grid") {
    TimedAudio a(48000);                                          // (2.19 - 0.01) * 48000 = 104640.000...015
    std::vector<float> s(480000, 1.0f), t(10, 1.0f);
    a.beginChunk(0.01); a.append(s.data(), s.size());            // [0.01, 10.01): trimmed back to 2.19
    a.beginChunk(-1.0); a.append(t.data(), t.size());            // the one being filled
    a.retain(-kInfT, 2.19, 0.19);
    CHECK(a.size() == 104640 + 10);
}
```

- [ ] **Step 2: Register the test file in `core_tests`, after `tests/test_video_plan.cpp`**. In `CMakeLists.txt`, replace:

```cmake
  tests/test_video_plan.cpp
```

with:

```cmake
  tests/test_video_plan.cpp
  tests/test_timed_audio.cpp
```

- [ ] **Step 3: Build to verify it fails**

```bash
cmake --build build --target core_tests -j8
```

Expected: the build FAILS, with an error mentioning `core/TimedAudio.h' file not found`.

- [ ] **Step 4: Write the header** — `src/core/TimedAudio.h` (complete file)

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace oss {

// Decoded audio tagged with UNWRAPPED time (see core/VideoPlan.h), for the Video Player's worker.
// Audio is kept as contiguous chunks, each a start time plus mono samples at `rate`, covering
// [start, start + size / rate): a chunk's last interval holds its last sample, so adjacent chunks tile.
// A seek, a loop wrap or a reverse stretch begins a new chunk. sample() maps output samples onto source
// time exactly as the node's emitAudio always did (linear interpolation; a reversed span reads
// backwards). Times no chunk covers are silence. Where chunks overlap, the most recently begun one wins:
// at a loop seam that is the new lap; in reverse it is the stretch below, whose audio runs on unbroken
// up to where the stretch above resumed its own -- a little before that stretch's keyframe, with a
// just-flushed decoder fading in.
// GL-free. Not thread-safe (sample() included: it keeps scratch): VideoStream guards it with its mutex.
class TimedAudio {
public:
    static constexpr double kMaxSeconds = 180.0;  // cap on everything held (about 35 MB at 48 kHz)
    static constexpr double kGridEps    = 1e-6;   // samples: a time on the sample grid is not rounded past

    explicit TimedAudio(int rate = 48000) : rate_(rate) {}

    int  rate() const { return rate_; }
    bool empty() const { return chunks_.empty(); }
    void clear() { chunks_.clear(); }

    // Start a new chunk at unwrapped time `startU`; append() adds to it.
    void beginChunk(double startU) { chunks_.push_back(Chunk{startU, {}}); }

    // Append to the current chunk, keeping only the samples that start before `clipHi` (a reverse
    // stretch passes the start of the stretch above it). No-op with no chunk.
    void append(const float* s, std::size_t n,
                double clipHi = std::numeric_limits<double>::infinity()) {
        if (chunks_.empty() || n == 0) return;
        Chunk& c = chunks_.back();
        const double room = std::ceil((clipHi - end(c)) * rate_ - kGridEps);
        if (!(room > 0.0)) return;
        const std::size_t take = room < (double)n ? (std::size_t)room : n;
        c.s.insert(c.s.end(), s, s + take);
        capTotal();
    }

    // Keep the audio around the playhead `u`: [lo, hi] is forward [u - keep, +inf), reverse
    // (-inf, u + keep]. Chunks wholly outside go (never the current one). A chunk's front (forward) or back
    // (reverse) is cut once more than a second of it lies outside, so calling this every step does not
    // move memory every step. The cap spares what plays at `u`. Compacts in place: nothing is allocated.
    void retain(double lo, double hi, double u) {
        playhead_ = u;
        std::size_t kept = 0;
        for (std::size_t i = 0; i < chunks_.size(); ++i) {
            Chunk& c = chunks_[i];
            const bool current = i + 1 == chunks_.size();
            if (!current && (end(c) < lo || c.start > hi)) continue;
            if (std::isfinite(lo) && lo > c.start + 1.0) {
                const std::size_t drop = std::min(c.s.size(), (std::size_t)((lo - c.start) * rate_));
                c.s.erase(c.s.begin(), c.s.begin() + (std::ptrdiff_t)drop);
                c.start += (double)drop / rate_;
            }
            if (!current && std::isfinite(hi) && end(c) > hi + 1.0)
                c.s.resize((std::size_t)std::max(0.0, std::ceil((hi - c.start) * rate_ - kGridEps)));
            if (kept != i) chunks_[kept] = std::move(c);
            ++kept;
        }
        chunks_.erase(chunks_.begin() + (std::ptrdiff_t)kept, chunks_.end());
    }

    // n output samples spanning source time [u0, u1] (u1 < u0 reads backwards).
    void sample(double u0, double u1, float* out, int n) const {
        const double lo = std::min(u0, u1), hi = std::max(u0, u1);
        span_.clear();                                   // the chunks the span touches, newest first
        for (std::size_t k = chunks_.size(); k-- > 0;)
            if (chunks_[k].start <= hi && end(chunks_[k]) > lo) span_.push_back(&chunks_[k]);
        for (int j = 0; j < n; ++j) out[j] = at(u0 + (u1 - u0) * ((double)j / n));
    }

    // Total samples held (for tests and the cap).
    std::size_t size() const {
        std::size_t n = 0;
        for (const Chunk& c : chunks_) n += c.s.size();
        return n;
    }

private:
    struct Chunk { double start; std::vector<float> s; };

    double end(const Chunk& c) const { return c.start + (double)c.s.size() / rate_; }

    // The sample at t from the newest chunk of span_ covering it.
    float at(double t) const {
        for (const Chunk* c : span_) {
            const double idx = (t - c->start) * rate_;
            if (!(idx >= 0.0 && idx < (double)c->s.size())) continue;
            const std::size_t i = (std::size_t)idx;
            const float fr = (float)(idx - (double)i);
            const float next = i + 1 < c->s.size() ? c->s[i + 1] : c->s[i];
            return c->s[i] * (1.0f - fr) + next * fr;
        }
        return 0.0f;
    }

    // The index of the chunk that plays at t (the newest covering it), or chunks_.size() if none does.
    std::size_t playingAt(double t) const {
        for (std::size_t k = chunks_.size(); k-- > 0;) {
            const double idx = (t - chunks_[k].start) * rate_;
            if (idx >= 0.0 && idx < (double)chunks_[k].s.size()) return k;
        }
        return chunks_.size();
    }

    // How far c lies from the playhead (0 when it covers it).
    double distance(const Chunk& c) const {
        return playhead_ < c.start ? c.start - playhead_ : (playhead_ >= end(c) ? playhead_ - end(c) : 0.0);
    }

    // Cut up to n samples from the end of c farther from the playhead, never past it; how many went.
    std::size_t trimFarEnd(Chunk& c, std::size_t n) {
        const std::size_t before = c.s.size();
        const double at = (playhead_ - c.start) * rate_;          // the playhead's place in c, in samples
        if (!std::isfinite(playhead_) || at >= 0.5 * (double)before) {           // the front is farther
            const double room = std::isfinite(at) ? std::min(std::floor(at), (double)before) : (double)before;
            const std::size_t cut = std::min(n, (std::size_t)std::max(0.0, room));
            c.s.erase(c.s.begin(), c.s.begin() + (std::ptrdiff_t)cut);
            c.start += (double)cut / rate_;
        } else {                                                                 // the back is
            const std::size_t keep = (std::size_t)std::max(0.0, std::floor(at) + 1.0);
            c.s.resize(std::max(keep, before - std::min(n, before)));
        }
        return before - c.s.size();
    }

    // While more than kMaxSeconds is held, drop the audio farthest from the playhead, never what plays at
    // it: the farthest chunk goes whole -- or, if it is the one being filled (appends go to it), loses its
    // far end, never past the playhead. In live reverse that one is the prefetch below the stretch that
    // plays next, so its far end goes first. Before any retain() there is no playhead: the oldest audio
    // goes first.
    void capTotal() {
        const std::size_t cap = (std::size_t)(kMaxSeconds * rate_);
        std::size_t total = size();
        bool fillingSpent = false;                       // nothing more can come off the one being filled
        while (total > cap) {
            const std::size_t last = chunks_.size() - 1, playing = playingAt(playhead_);
            std::size_t victim = chunks_.size();
            double farthest = -1.0;
            for (std::size_t i = 0; i < chunks_.size(); ++i) {
                if (i == playing || (i == last && fillingSpent)) continue;
                const double d = distance(chunks_[i]);
                if (d > farthest) { farthest = d; victim = i; }
            }
            if (victim == chunks_.size()) break;
            if (victim == last) {
                const std::size_t cut = trimFarEnd(chunks_[last], total - cap);
                total -= cut;
                fillingSpent = cut == 0 || chunks_[last].s.empty();
            } else {
                total -= chunks_[victim].s.size();
                chunks_.erase(chunks_.begin() + (std::ptrdiff_t)victim);
            }
        }
        if (total > cap && playingAt(playhead_) == chunks_.size() - 1)          // the one playing is being filled
            trimFarEnd(chunks_.back(), total - cap);
    }

    std::vector<Chunk> chunks_;
    int rate_;
    double playhead_ = -std::numeric_limits<double>::infinity();   // as retain() last saw it
    mutable std::vector<const Chunk*> span_;                        // sample()'s scratch
};

} // namespace oss
```

- [ ] **Step 5: Build and run the tests (11 test cases)**

```bash
cmake --build build --target core_tests -j8 && ./build/core_tests -tc='TimedAudio*'
```

Expected output includes: `Status: SUCCESS!`

- [ ] **Step 6: Run the whole unit suite**

```bash
./build/core_tests
```

Expected output includes: `Status: SUCCESS!`

- [ ] **Step 7: Commit**

```bash
git add src/core/TimedAudio.h tests/test_timed_audio.cpp CMakeLists.txt
git commit -F - <<'EOF'
feat(core): add TimedAudio, a time-tagged audio store

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

### Task 5: VideoDecoder: split decode from conversion, thread it, read audio ahead

**Files:**
- Modify (full rewrite): `src/gfx/VideoDecoder.h`, `src/gfx/VideoDecoder.cpp`
- Modify: `tests/gl_smoke.cpp`

The worker must decode a frame, look at its time, and only then decide to convert it; convert with threads; and keep audio ahead of the video however few frames it buffers. So: `decodeNext()` returns a `DecodedFrame` (a counted reference, nothing copied); `convert()` writes TOP-DOWN RGBA with threaded swscale (the portable FFmpeg ≥ 5 path rejects a negative-stride destination, and FFmpeg 5–7 allocate a new buffer for a destination frame with none — so the caller's buffer is wrapped in a no-op-free `AVBufferRef`); video packets are queued (≤ 64 MB) so `pumpAudio()` can read audio ahead; `thread_count = 0`; an interrupt callback lets a stop abort blocking I/O — and since a stop interrupts `avformat_find_stream_info()` part-way, which then still succeeds with the pixel format unknown, `open()` fails ("stopped") when the flag is set once the probe returns. The legacy `decodeFrame()` keeps its exact behaviour (bottom-up, single-threaded conversion) — the 15 `gl_smoke` uses rely on it — but builds its converter from the first decoded frame's format, like `convert()`: the stream's may be unknown until a frame decodes (a stopped probe, or a video that starts past what the probe reads, which aborted libswscale). Every time in and out counts from the first video frame, which `open()` decodes (and the first `decodeNext()` hands out): a container that starts its clock late (MPEG-TS) or shows a B-frame delay with no edit list (FLV, fragmented MP4) would otherwise put the first frame after the playhead's 0 — an offline render could never have its first frame. `seek(t <= 0)` goes to the very start of the file, since a timestamp search (MPEG-TS) can overshoot the first frame's time by a keyframe interval; audio from before the first frame is dropped. `open()` seeks to the start before decoding that first frame, because some demuxers read their keyframe index only when first asked to seek (Matroska and WebM cues) — without it the worker could not seek ahead in the first lap. MPEG-TS and MPEG-PS indexes are ignored (`nextKeyframeAfter` reports none known): their demuxers list every packet they probe while seeking as a keyframe. A frame with no timestamp (the B-frame tail a decoder flushes from an AVI or MPEG-PS, every frame of a raw H.264 stream) takes the previous frame's time plus its duration; one with nothing before it since a seek into the file cannot be placed and is skipped. A raw stream has no times to seek by (a search reads the whole file, then fails), so `seek()` takes it to its first byte, falls back there whenever a seek fails, and returns false only when not even the start can be reached; `open()` fails when no frame decodes. Audio comes out in runs that follow its timestamps — a hole or an overlap starts a new run, which `takeAudio()` flags — the resampler is rebuilt when the audio format changes and reset by a seek, and once a caller waiting for the audio (an offline render: `pumpAudio(t, true)`) is stuck at the 64 MB cap — no packet taken off the queue since it last stopped there — the audio counts as settled up to the last packet read, so an offline render cannot wait forever (while packets are still being taken it only waits: a fragmented MOV puts each fragment's audio after its video; and a caller not waiting gives nothing up, since a live pause stops decoding too). A raw stream's frame duration comes from its decoder (its demuxer knows only 25 fps), and a frame's own duration is `AVFrame::duration` from FFmpeg 6.0 but `pkt_duration` in 5.1, the oldest FFmpeg supported. The second scenario writes the awkward files itself; one needs FFmpeg's API directly (a remux that leaves a hole in the audio), and the FLV and raw H.264 cases SKIP where there is no H.264 encoder.

- [ ] **Step 1: Add `#include <cstring>` to `tests/gl_smoke.cpp`, after `#include <cstdlib>`**. In `tests/gl_smoke.cpp`, replace:

```cpp
#include <cstdlib>
```

with:

```cpp
#include <cstdlib>
#include <cstring>
```

- [ ] **Step 2: Include FFmpeg's `libavformat` in `tests/gl_smoke.cpp`, after `#include "gfx/VideoEncoder.h"` (one test clip needs a remux `VideoEncoder` cannot do)**. In `tests/gl_smoke.cpp`, replace:

```cpp
#include "gfx/VideoEncoder.h"
```

with:

```cpp
#include "gfx/VideoEncoder.h"
extern "C" {
#include <libavformat/avformat.h>   // remuxWithAudioHole: a clip VideoEncoder cannot write
}
```

- [ ] **Step 3: Add the two failing scenarios (and the audio-hole clip writer the `VideoStream` scenarios reuse), just above `// --- Scenario 10: Video Player decodes a file to texture + audio ---`**:

```cpp
// --- Scenario: VideoDecoder's split decode -- decodeNext() + convert() match decodeFrame() ---
// The worker decodes a frame, looks at its time, and only then converts it (threaded, top-down).
// That must produce exactly decodeFrame()'s pixels, flipped; the keyframe index, the frame rate and
// the audio read-ahead are what its decisions rest on.
static bool scenario_video_decoder_split_decode() {
    {
        VideoDecoder a, b; std::string err;
        if (!a.open("tests/assets/test.mp4", err) || !b.open("tests/assets/test.mp4", err)) {
            return failed(("split decode: open: " + err).c_str());
        }
        if (std::fabs(a.frameDuration() - 0.1) > 1e-9) { return failed("split decode: test.mp4 is 10 fps"); }
        const int W = a.width(), H = a.height();
        std::vector<unsigned char> top((std::size_t)W * H * 4);
        VideoFrame vf; std::vector<float> au; double aS = 0; bool aV = false;
        for (int i = 0; i < 10; ++i) {
            DecodedFrame f;
            if (!a.decodeNext(f) || !b.decodeFrame(vf, au, aS, aV)) { return failed("split decode: ran out of frames"); }
            if (f.t != vf.t) { return failed("split decode: decodeNext and decodeFrame disagree on the time"); }
            if (a.convert(f, top.data(), W * 4 - 4)) { return failed("split decode: convert() must refuse a stride shorter than a row"); }
            if (!a.convert(f, top.data(), W * 4)) { return failed("split decode: convert failed"); }
            for (int y = 0; y < H; ++y)
                if (std::memcmp(top.data() + (std::size_t)y * W * 4, vf.rgba + (std::size_t)(H - 1 - y) * W * 4,
                                (std::size_t)W * 4) != 0) {
                    return failed("split decode: convert() is not decodeFrame() flipped top-down");
                }
        }
        double key = 0.0;
        if (!a.nextKeyframeAfter(0.1, key) || !(key > 0.1 && key <= 0.55)) { return failed("split decode: the next keyframe after 0.1 s is the one at 0.5 s"); }
        if (!a.nextKeyframeAfter(1.0, key) || std::isfinite(key)) { return failed("split decode: nothing after the last keyframe should read +inf"); }

        VideoDecoder c;                                   // one frame decoded, then the audio read ahead
        if (!c.open("tests/assets/test.mp4", err)) { return failed("split decode: reopen"); }
        DecodedFrame f;
        if (!c.decodeNext(f)) { return failed("split decode: first frame"); }
        c.pumpAudio(1.0);
        std::vector<float> s; double st = 0.0; bool cont = true;
        if (!c.takeAudio(s, st, cont) || cont || s.size() < 48000 || c.audioSettledUpTo() < 1.0) {
            return failed("split decode: pumpAudio did not read a second of audio ahead of one video frame");
        }

        // A file whose video starts late -- an FLV with B-frames puts its first frame a frame in --
        // counts from that frame: it is at 0, and the keyframe index, the duration, seek() and the
        // audio count from it too. (FLV carries H.264, not VideoEncoder's MPEG-4 fallback.)
        const std::string late = "build/_late_start.flv";
        bool flv = false;
        {
            VideoEncoder enc;
            if (enc.open(late, 64, 48, 25, 48000, 1, err)) {
                std::vector<unsigned char> px((std::size_t)64 * 48 * 4);
                std::vector<float> tone(48000 / 25);
                for (int f = 0; f < 50; ++f) {
                    std::fill(px.begin(), px.end(), (unsigned char)(f * 5));
                    for (std::size_t i = 0; i < tone.size(); ++i)
                        tone[i] = 0.5f * (float)std::sin(0.05 * (double)(f * tone.size() + i));
                    if (!enc.addVideoFrame(px.data(), f / 25.0) || !enc.addAudio(tone.data(), (int)tone.size())) {
                        return failed("late start: encode a frame");
                    }
                }
                if (!enc.close(err)) { return failed(("late start: close: " + err).c_str()); }
                flv = true;
            }
        }
        if (!flv) {
            std::fprintf(stderr, "gl_smoke SKIP: late-start FLV (no H.264 encoder: %s)\n", err.c_str());
        } else {
            VideoDecoder d;
            if (!d.open(late, err)) { return failed(("late start: open: " + err).c_str()); }
            DecodedFrame f0, f1;
            if (!d.decodeNext(f0) || !d.decodeNext(f1) || f0.t != 0.0 || std::fabs(f1.t - 0.04) > 1e-6) {
                std::fprintf(stderr, "late start: the first frames are at %.4f and %.4f\n", f0.t, f1.t);
                return failed("late start: times must count from the first frame (0, then 0.04 s)");
            }
            double lateKey = 0.0;
            if (!d.nextKeyframeAfter(0.1, lateKey) || !(lateKey > 0.9 && lateKey < 1.05)) {
                return failed("late start: the keyframe index must count from the first frame (the next keyframe is at 1 s)");
            }
            if (std::fabs(d.duration() - 2.0) > 0.05) { return failed("late start: the duration must count from the first frame (2 s)"); }
            DecodedFrame g;
            if (!d.seek(0.0) || !d.decodeNext(g) || g.t != 0.0) { return failed("late start: seek(0) must return to the first frame"); }
            d.pumpAudio(1.0);
            std::vector<float> la; double laStart = -1.0; bool laCont = true;
            if (!d.takeAudio(la, laStart, laCont) || laCont || laStart < 0.0 || laStart > 0.05) {
                return failed("late start: the audio must count from the first frame");
            }
        }

        // A Matroska file's cues -- its keyframe index -- are read only when the demuxer is first asked to
        // seek; open() asks, so keyframes past what probing read (about 5 s) are known from the start.
        const std::string cues = "build/_cues.mkv";
        {
            VideoEncoder enc;
            if (!enc.open(cues, 64, 48, 25, 0, 0, err)) { return failed(("cues: encode: " + err).c_str()); }
            std::vector<unsigned char> px((std::size_t)64 * 48 * 4);
            for (int f = 0; f < 300; ++f) {                // 12 s, keyframes every second or sooner
                std::fill(px.begin(), px.end(), (unsigned char)(f % 256));
                if (!enc.addVideoFrame(px.data(), f / 25.0)) { return failed("cues: encode a frame"); }
            }
            if (!enc.close(err)) { return failed(("cues: close: " + err).c_str()); }
        }
        VideoDecoder m;
        double cueKey = 0.0;
        if (!m.open(cues, err) || !m.nextKeyframeAfter(7.0, cueKey) || !(cueKey > 7.0 && cueKey < 12.0)) {
            std::fprintf(stderr, "cues: the keyframe after 7 s reads %g\n", cueKey);
            return failed("cues: a Matroska file's keyframe index must be read at open (there are keyframes after 7 s)");
        }

        // An MPEG-TS demuxer lists every packet it reads while searching for a seek position as a keyframe:
        // the decoder must not pass those on -- the worker would seek towards keyframes that are not there.
        const std::string probes = "build/_probes.ts";
        {
            VideoEncoder enc;
            if (!enc.open(probes, 64, 48, 25, 0, 0, err)) { return failed(("probes: encode: " + err).c_str()); }
            std::vector<unsigned char> px((std::size_t)64 * 48 * 4);
            for (int f = 0; f < 300; ++f) {
                std::fill(px.begin(), px.end(), (unsigned char)(f % 256));
                if (!enc.addVideoFrame(px.data(), f / 25.0)) { return failed("probes: encode a frame"); }
            }
            if (!enc.close(err)) { return failed(("probes: close: " + err).c_str()); }
        }
        VideoDecoder p;
        DecodedFrame pf;
        double probeKey = 0.0;
        if (!p.open(probes, err)) { return failed(("probes: open: " + err).c_str()); }
        // Its first frame is 0.04 s into the clock (the B-frame delay): times count from that frame, and so
        // does the duration (counted from the clock's start it would read 12.04 s).
        if (!p.decodeNext(pf) || pf.t != 0.0 || std::fabs(p.duration() - 12.0) > 0.02) {
            std::fprintf(stderr, "probes: first frame at %.4f, duration %.4f\n", pf.t, p.duration());
            return failed("probes: an MPEG-TS whose clock starts late must count from its first frame (at 0; 12 s long)");
        }
        p.seek(5.5);                                        // the search reads packets around 5.5 s
        if (!p.decodeNext(pf) || !p.nextKeyframeAfter(0.1, probeKey) || std::isfinite(probeKey)) {
            std::fprintf(stderr, "probes: after a seek, the keyframe after 0.1 s reads %g\n", probeKey);
            return failed("probes: an MPEG-TS index lists seek probes, not keyframes: it must read as none known");
        }
        std::fprintf(stderr, "gl_smoke OK: decodeNext+convert == decodeFrame flipped; keyframe lookup; %.2f s of audio read ahead; "
                     "a late-starting file counts from its first frame; Matroska cues are read at open; "
                     "MPEG-TS seek probes are not keyframes\n", s.size() / 48000.0);
    }
    return true;
}

// Copy `src` to `dst` packet for packet, leaving out the audio packets that start in [from, to) seconds.
static bool remuxWithAudioHole(const std::string& src, const std::string& dst, double from, double to) {
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, src.c_str(), nullptr, nullptr) < 0) return false;
    AVFormatContext* out = nullptr;
    bool ok = avformat_find_stream_info(in, nullptr) >= 0 &&
              avformat_alloc_output_context2(&out, nullptr, nullptr, dst.c_str()) >= 0;
    for (unsigned i = 0; ok && i < in->nb_streams; ++i) {
        AVStream* o = avformat_new_stream(out, nullptr);
        ok = o && avcodec_parameters_copy(o->codecpar, in->streams[i]->codecpar) >= 0;
        if (ok) { o->codecpar->codec_tag = 0; o->time_base = in->streams[i]->time_base; }
    }
    ok = ok && avio_open(&out->pb, dst.c_str(), AVIO_FLAG_WRITE) >= 0 && avformat_write_header(out, nullptr) >= 0;
    AVPacket* p = av_packet_alloc();
    while (ok && p && av_read_frame(in, p) >= 0) {
        const AVStream* st = in->streams[p->stream_index];
        const double t = p->pts * av_q2d(st->time_base);
        if (!(st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && t >= from && t < to)) {
            av_packet_rescale_ts(p, st->time_base, out->streams[p->stream_index]->time_base);
            ok = av_interleaved_write_frame(out, p) >= 0;
        }
        av_packet_unref(p);
    }
    av_packet_free(&p);
    ok = ok && av_write_trailer(out) >= 0;
    if (out) { avio_closep(&out->pb); avformat_free_context(out); }
    avformat_close_input(&in);
    return ok;
}

// A 3 s clip: 64x48 at 25 fps, a 440 Hz tone at `rate`.
static bool writeToneClip(const std::string& path, int rate = 48000) {
    VideoEncoder enc; std::string err;
    if (!enc.open(path, 64, 48, 25, rate, 1, err)) {
        std::fprintf(stderr, "writeToneClip: %s\n", err.c_str());
        return false;
    }
    std::vector<unsigned char> px((std::size_t)64 * 48 * 4, 128);
    std::vector<float> tone((std::size_t)rate / 25);
    for (int f = 0; f < 75; ++f) {
        for (std::size_t i = 0; i < tone.size(); ++i)
            tone[i] = 0.5f * (float)std::sin(6.283185307179586 * 440.0 * (double)(f * tone.size() + i) / rate);
        if (!enc.addVideoFrame(px.data(), f / 25.0) || !enc.addAudio(tone.data(), (int)tone.size())) return false;
    }
    return enc.close(err);
}

// writeToneClip's clip without the audio packets that start in [from, to) seconds: a hole in the audio's
// timestamps -- by default from 1.0 s to 1.6 s -- which VideoEncoder cannot write.
static bool writeAudioHoleClip(const std::string& path, double from = 1.0, double to = 1.6) {
    const std::string whole = path + ".whole.mkv";
    return writeToneClip(whole) && remuxWithAudioHole(whole, path, from, to);
}

// The runs of audio `d` hands out (see VideoDecoder::takeAudio).
struct AudioRunSeen { double start, end; bool continues; };
static std::vector<AudioRunSeen> takeRuns(VideoDecoder& d) {
    std::vector<AudioRunSeen> runs;
    std::vector<float> a; double s = 0.0; bool c = false;
    while (d.takeAudio(a, s, c)) runs.push_back({s, s + a.size() / 48000.0, c});
    return runs;
}

// Every frame VideoDecoder `d` has left: false unless their times rise strictly; `n` counts them, `last` is the last time.
static bool decodeRest(VideoDecoder& d, int& n, double& last) {
    DecodedFrame f;
    bool rising = true;
    n = 0; last = -1.0;
    while (d.decodeNext(f)) { rising = rising && f.t > last; last = f.t; ++n; }
    return rising;
}

// --- Scenario: VideoDecoder on awkward files ---
// Frames with no timestamps (an AVI's B-frame tail, a raw stream), a seek in a file whose video starts
// late, a hole in the audio, and a stream whose audio format and picture size change midway.
static bool scenario_video_decoder_awkward_files() {
    std::string err;
    std::vector<unsigned char> px((std::size_t)64 * 48 * 4);

    // An AVI with B-frames: the decoder flushes its last frames with no timestamp. They follow the frame
    // before them (they used to read as before the first frame, and the worker sought forever).
    const std::string avi = "build/_untimed_tail.avi";
    {
        VideoEncoder enc;
        if (!enc.open(avi, 64, 48, 25, 0, 0, err)) { return failed(("untimed tail: encode: " + err).c_str()); }
        for (int f = 0; f < 50; ++f) {
            std::fill(px.begin(), px.end(), (unsigned char)(f * 5));
            if (!enc.addVideoFrame(px.data(), f / 25.0)) { return failed("untimed tail: encode a frame"); }
        }
        if (!enc.close(err)) { return failed(("untimed tail: close: " + err).c_str()); }
    }
    VideoDecoder a;
    if (!a.open(avi, err)) { return failed(("untimed tail: open: " + err).c_str()); }
    for (int lap = 0; lap < 2; ++lap) {
        int n = 0; double last = 0.0;
        if (!decodeRest(a, n, last) || n != 50 || std::fabs(last - 1.96) > 1e-6) {
            std::fprintf(stderr, "untimed tail: lap %d: %d frames, the last at %.4f\n", lap, n, last);
            return failed("untimed tail: 50 frames, 0.04 s apart, the last at 1.96 s -- every lap");
        }
        if (!a.seek(0.0)) { return failed("untimed tail: seek(0)"); }
    }

    // A raw H.264 stream has no timestamps at all, and no times to seek by: a search by time read the whole
    // file and failed, leaving nothing to decode. Its seeks go to the start instead. At 30 fps: its demuxer
    // knows only a default of 25, so the frame duration must come from the decoder.
    const std::string raw = "build/_raw.h264";
    bool rawWritten = false;
    {
        VideoEncoder enc;                                   // the raw H.264 muxer takes H.264 only
        if (enc.open(raw, 64, 48, 30, 0, 0, err)) {
            for (int f = 0; f < 50; ++f) {
                std::fill(px.begin(), px.end(), (unsigned char)(f * 5));
                if (!enc.addVideoFrame(px.data(), f / 30.0)) { return failed("raw stream: encode a frame"); }
            }
            if (!enc.close(err)) { return failed(("raw stream: close: " + err).c_str()); }
            rawWritten = true;
        }
    }
    if (!rawWritten) {
        std::fprintf(stderr, "gl_smoke SKIP: raw stream (no H.264 encoder: %s)\n", err.c_str());
    } else {
        VideoDecoder r;
        if (!r.open(raw, err)) { return failed(("raw stream: open: " + err).c_str()); }
        int n = 0; double last = 0.0, key = 0.0;
        DecodedFrame g;
        if (!decodeRest(r, n, last) || n != 50 || std::fabs(last - 49.0 / 30.0) > 1e-6 || std::fabs(r.frameDuration() - 1.0 / 30.0) > 1e-6) {
            std::fprintf(stderr, "raw stream: %d frames, the last at %.4f; frameDuration %.5f\n", n, last, r.frameDuration());
            return failed("raw stream: 50 frames, 1/30 s apart, and a frame duration of 1/30 s");
        }
        if (!r.seek(1.0) || !r.decodeNext(g) || g.t != 0.0) { return failed("raw stream: a seek goes back to the start"); }
        if (!r.nextKeyframeAfter(0.5, key) || std::isfinite(key)) { return failed("raw stream: its index is no guide to seeking"); }
    }

    // Video that starts 1 s into the file, its audio at 0: times count from the first frame, so seek(2.02)
    // lands on the keyframe 2 s after it -- not on the one 2 s into the file -- and the audio before the
    // first frame is dropped.
    const std::string lateVideo = "build/_late_video.mkv";
    {
        VideoEncoder enc;
        if (!enc.open(lateVideo, 64, 48, 25, 48000, 1, err)) { return failed(("late video: encode: " + err).c_str()); }
        std::vector<float> tone(48000 / 25, 0.25f);
        for (int f = 0; f < 100; ++f) {                    // audio from 0 s, video from 1 s (frame 25)
            std::fill(px.begin(), px.end(), (unsigned char)(f * 2));
            if ((f >= 25 && !enc.addVideoFrame(px.data(), f / 25.0)) || !enc.addAudio(tone.data(), (int)tone.size())) {
                return failed("late video: encode a frame");
            }
        }
        if (!enc.close(err)) { return failed(("late video: close: " + err).c_str()); }
    }
    VideoDecoder m;
    DecodedFrame m0, m2;
    std::vector<float> run; double runStart = -1.0; bool runCont = true;
    if (!m.open(lateVideo, err) || !m.decodeNext(m0) || m0.t != 0.0) { return failed("late video: the first frame is at 0"); }
    m.pumpAudio(0.5);
    if (!m.takeAudio(run, runStart, runCont) || runCont || runStart < 0.0 || runStart > 0.001) {
        std::fprintf(stderr, "late video: the first audio starts at %.4f\n", runStart);
        return failed("late video: the audio must start with the first frame (what came before it is dropped)");
    }
    if (!m.seek(2.02) || !m.decodeNext(m2) || std::fabs(m2.t - 2.0) > 1e-6) {
        std::fprintf(stderr, "late video: seek(2.02) landed at %.4f\n", m2.t);
        return failed("late video: seek(2.02) must land on the keyframe 2 s after the first frame");
    }

    // A hole in the audio's timestamps is kept: the audio after it resumes at its own time, as a new run
    // (it used to carry straight on from where the hole began, early by the hole's length ever after).
    // So is a hole of 64 ms -- three AAC packets -- which is more than timestamp rounding.
    for (const double to : {1.6, 1.06}) {
        const std::string hole = to > 1.5 ? "build/_audio_hole.mkv" : "build/_audio_small_hole.mkv";
        if (!writeAudioHoleClip(hole, 1.0, to)) { return failed("audio hole: writing the clip failed"); }
        VideoDecoder h;
        if (!h.open(hole, err)) { return failed(("audio hole: open: " + err).c_str()); }
        h.pumpAudio(1.3);
        if (h.audioSettledUpTo() < 1.3) { return failed("audio hole: pumpAudio must settle the audio through the hole"); }
        h.pumpAudio(2.5);
        const std::vector<AudioRunSeen> runs = takeRuns(h);
        bool resumed = false;
        for (std::size_t i = 1; i < runs.size(); ++i)
            resumed = resumed || (!runs[i].continues && std::fabs(runs[i - 1].end - 1.0) < 0.03 && std::fabs(runs[i].start - to) < 0.03);
        if (!resumed) {
            for (std::size_t i = 0; i < runs.size(); ++i)
                std::fprintf(stderr, "audio hole: run %zu [%.4f, %.4f)%s\n", i, runs[i].start, runs[i].end, runs[i].continues ? " continues" : "");
            std::fprintf(stderr, "audio hole: expected a new run at %.2f s\n", to);
            return failed("audio hole: the audio after a hole must resume at its own time, as a new run");
        }
        DecodedFrame hf;
        while (h.decodeNext(hf)) {}
        if (!std::isinf(h.audioSettledUpTo())) { return failed("audio hole: at the end of the input all the audio is settled"); }
    }

    // The read-ahead cap (1 byte here: full after a single packet), with audio that starts 1.5 s in. A caller
    // waiting for the audio (an offline render) that is still taking packets off the queue only waits at
    // the cap -- audio a file puts after a run of video (fragmented MOV) still arrives. Once it has taken
    // none since the last stop, nothing will drain the queue: what was read counts as settled (it would
    // wait forever). A caller that is not waiting (live) gives nothing up, even when it stops decoding.
    const std::string lateAudio = "build/_late_audio.mkv";
    if (!writeAudioHoleClip(lateAudio, -1.0, 1.5)) { return failed("read-ahead cap: writing the clip failed"); }
    VideoDecoder q;
    q.setMaxQueuedBytes(1);
    DecodedFrame q0, q1;
    if (!q.open(lateAudio, err) || !q.decodeNext(q0)) { return failed("read-ahead cap: open"); }
    q.pumpAudio(1.0, true);                              // stops at the cap
    const double atCap = q.audioSettledUpTo();
    if (!q.decodeNext(q1)) { return failed("read-ahead cap: decode"); }
    q.pumpAudio(1.0, true);                              // a packet was taken since: still draining
    const double draining = q.audioSettledUpTo();
    q.pumpAudio(1.0, true);                              // none since: stuck
    const double stuck = q.audioSettledUpTo();
    q.pumpAudio(1.0);                                    // not waiting: what was given up is forgotten...
    q.pumpAudio(1.0);                                    // ...and stuck again, nothing is given up
    const double live = q.audioSettledUpTo();
    q.pumpAudio(1.0, true);                              // waiting again: its first stop -- not stuck yet
    const double rewaiting = q.audioSettledUpTo();
    q.pumpAudio(1.0, true);                              // stuck
    const double restuck = q.audioSettledUpTo();
    if (atCap >= 0.0 || draining >= 0.0 || !(stuck >= q1.t) || live >= 0.0 || rewaiting >= 0.0 || !(restuck >= q1.t)) {
        std::fprintf(stderr, "read-ahead cap: settled up to %.3f at the cap, %.3f while draining, %.3f once stuck, "
                     "%.3f live, %.3f waiting again, %.3f stuck again\n", atCap, draining, stuck, live, rewaiting, restuck);
        return failed("read-ahead cap: the audio must settle through the cap once, and only once, a waiting caller is stuck");
    }

    // A seek resets the resampler: the audio after seek(1.0) is the same whatever played before it (at
    // 44.1 kHz the resampler holds samples from one call to the next).
    const std::string tone441 = "build/_tone441.mkv";
    if (!writeToneClip(tone441, 44100)) { return failed("resampler: writing the clip failed"); }
    VideoDecoder played, fresh;
    if (!played.open(tone441, err) || !fresh.open(tone441, err)) { return failed("resampler: open"); }
    DecodedFrame rf;
    for (int i = 0; i < 50 && played.decodeNext(rf); ++i) takeRuns(played);
    std::vector<float> afterPlayed, afterFresh;
    for (VideoDecoder* d : {&played, &fresh}) {
        if (!d->seek(1.0) || !d->decodeNext(rf)) { return failed("resampler: seek"); }
        d->pumpAudio(1.5);
        std::vector<float> a; double st = 0.0; bool c = false;
        if (!d->takeAudio(a, st, c)) { return failed("resampler: no audio after the seek"); }
        (d == &played ? afterPlayed : afterFresh) = a;
    }
    if (afterPlayed.size() < 4800 || afterPlayed != afterFresh) {
        return failed("resampler: the audio after a seek must not depend on what played before it");
    }

    // Two MPEG-TS files end to end: stereo 64x48, then mono 80x64. The resampler is rebuilt for the new
    // format (one built for two channels read a second plane that mono audio does not have: a crash),
    // and convert() refuses the frames of the new size.
    const std::string partA = "build/_fmt_a.ts", partB = "build/_fmt_b.ts", joined = "build/_fmt_change.ts";
    for (int part = 0; part < 2; ++part) {
        const int w = part ? 80 : 64, hgt = part ? 64 : 48, ch = part ? 1 : 2;
        VideoEncoder enc;
        if (!enc.open(part ? partB : partA, w, hgt, 25, 48000, ch, err)) { return failed(("format change: encode: " + err).c_str()); }
        std::vector<unsigned char> img((std::size_t)w * hgt * 4, 128);
        std::vector<float> tone((std::size_t)(48000 / 25) * ch, 0.25f);
        for (int f = 0; f < 25; ++f) {
            if (!enc.addVideoFrame(img.data(), f / 25.0) || !enc.addAudio(tone.data(), (int)tone.size())) {
                return failed("format change: encode a frame");
            }
        }
        if (!enc.close(err)) { return failed(("format change: close: " + err).c_str()); }
    }
    {
        std::ofstream out(joined, std::ios::binary);
        for (const std::string& part : {partA, partB}) { std::ifstream in(part, std::ios::binary); out << in.rdbuf(); }
    }
    VideoDecoder x;
    if (!x.open(joined, err)) { return failed(("format change: open: " + err).c_str()); }
    DecodedFrame xf;
    int converted = 0, refused = 0;
    std::size_t audio = 0;
    std::vector<float> xa; double xs = 0.0; bool xc = false;
    while (x.decodeNext(xf)) {
        ++(x.convert(xf, px.data(), 64 * 4) ? converted : refused);
        while (x.takeAudio(xa, xs, xc)) audio += xa.size();
    }
    while (x.takeAudio(xa, xs, xc)) audio += xa.size();
    if (converted < 20 || refused < 20 || audio < (std::size_t)(1.6 * 48000)) {
        std::fprintf(stderr, "format change: %d converted, %d refused, %.2f s of audio\n", converted, refused, audio / 48000.0);
        return failed("format change: both halves must decode -- the second half's frames refused (another size), its audio resampled");
    }
    std::fprintf(stderr, "gl_smoke OK: untimed frames follow the one before; %sa late start seeks from its first frame; "
                 "audio holes are kept; the read-ahead cap settles only when stuck; a seek resets the resampler; "
                 "a format change mid-stream decodes\n", rawWritten ? "a raw stream decodes and seeks to its start; " : "");
    return true;
}

```

- [ ] **Step 4: In `kScenarios`, take `scenario_video_player_decode` out of its place just above `scenario_text_geometry_renderers,`**. In `tests/gl_smoke.cpp`, replace:

```cpp
    scenario_video_player_decode,
    scenario_text_geometry_renderers,
```

with:

```cpp
    scenario_text_geometry_renderers,
```

- [ ] **Step 5: Register the two scenarios at the end of `kScenarios`, followed by `scenario_video_player_decode`: the video scenarios run last, so a slow machine tripping one of their time bounds cannot hide the scenarios after it (`gl_smoke` stops at its first failure)**. In `tests/gl_smoke.cpp`, replace:

```cpp
    scenario_offline_late_starting_loader_gate,
};
```

with:

```cpp
    scenario_offline_late_starting_loader_gate,
    scenario_video_decoder_split_decode,
    scenario_video_decoder_awkward_files,
    scenario_video_player_decode,
};
```

- [ ] **Step 6: Build to verify it fails**

```bash
cmake --build build --target gl_smoke -j8
```

Expected: the build FAILS, with an error mentioning `no member named 'decodeNext'`.

- [ ] **Step 7: Replace `src/gfx/VideoDecoder.h`** — `src/gfx/VideoDecoder.h` (complete file)

```cpp
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <vector>

// Forward-declare the FFmpeg types so this header stays free of <libav*> includes
// (only VideoDecoder.cpp pulls those in). These are C structs from FFmpeg.
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;

namespace oss {

// One decoded video frame: tightly-packed RGBA8 pixels valid until the next
// decodeFrame() call (the decoder reuses its conversion buffer).
struct VideoFrame {
    double               t = 0.0;   // presentation time in seconds, from the first frame
    int                  width = 0;
    int                  height = 0;
    // width*height*4 bytes, stored bottom row first (GL texture convention) so it
    // uploads straight to a texture and shows upright through the output blit.
    const std::uint8_t*  rgba = nullptr;
};

// A decoded video frame that has not been converted to RGBA yet: a counted reference to the
// decoder's picture, so holding one copies nothing. Move-only. VideoDecoder::decodeNext() fills it
// and VideoDecoder::convert() turns it into pixels, so a caller can decode a frame, look at its
// time, and only then decide whether it is worth converting.
class DecodedFrame {
public:
    DecodedFrame() = default;
    ~DecodedFrame();
    DecodedFrame(DecodedFrame&& o) noexcept;
    DecodedFrame& operator=(DecodedFrame&& o) noexcept;
    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;

    bool valid() const { return frame_ != nullptr; }
    void reset();

    double t = 0.0;   // presentation time in seconds, from the first frame

private:
    friend class VideoDecoder;
    AVFrame* frame_ = nullptr;
};

// Thin FFmpeg wrapper: decodes a media file's video and its audio resampled to 48 kHz mono float.
// NOT thread-safe and GL-free -- it only produces CPU buffers; the caller uploads frames to a
// texture and feeds the audio downstream. Drive it from one thread: open(), then seek() and
// decodeNext()/decodeFrame() to walk frames in source order. Reverse/variable-rate playback is the
// caller's job (re-seek to a keyframe and re-walk forward); this class only goes forward.
//
// Video packets are read into a bounded queue rather than decoded straight away, so pumpAudio()
// can read ahead and keep the audio ahead of the playhead however few frames the caller buffers.
//
// Every time in and out -- frames, audio, seek(), the keyframe index, duration() -- counts from the
// first video frame, which is at 0: a container may start its clock late (MPEG-TS), or a B-frame delay
// with no edit list to hide it (FLV, fragmented MP4) may put the first frame a frame or two in, and a
// playhead starts at 0. Audio from before the first frame is dropped. A frame with no timestamp -- the
// last few a decoder flushes from an AVI or MPEG-PS with B-frames, or every frame of a raw H.264 or
// HEVC stream -- follows the one before it; one that follows nothing since a seek into the file cannot
// be placed, and is skipped.
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Open `path`. Returns false and fills `err` on failure (bad path, no video
    // stream, unsupported codec, no frame that decodes). A file with no audio stream still opens.
    // `abort` (optional) is polled by FFmpeg during blocking I/O: once it reads true, opening or
    // reading gives up promptly (VideoStream uses it to stop its worker). Decodes the first frame,
    // to learn when it is; the first decodeNext() hands it out.
    bool open(const std::string& path, std::string& err, const std::atomic<bool>* abort = nullptr);

    bool   isOpen()   const { return fmt_ != nullptr; }
    int    width()    const { return width_; }
    int    height()   const { return height_; }
    double duration() const { return duration_; }      // seconds (0 if unknown)
    bool   hasAudio() const { return astream_ >= 0; }
    int    audioRate() const { return kOutRate; }      // we always resample to this
    int    audioChannels() const { return audioChannels_; }  // source channels (before our mono downmix)
    static constexpr int kOutRate = 48000;             // 48 kHz mono float out

    // Nominal seconds per frame, from the stream's average (else real) frame rate -- a raw stream's
    // from its decoder, which reads the rate from the stream itself -- or 1/30 if unknown.
    double frameDuration() const;

    // The first keyframe after time `t`, from the container's index -- possibly listed by its decode
    // time, a frame or two early. `key` is +inf when none is known after `t`: the index holds none, or it
    // is no guide (MPEG-TS and MPEG-PS list every packet they probe while seeking; a raw stream only
    // ever seeks to its start). False when the stream has no index (yet).
    bool nextKeyframeAfter(double t, double& key) const;

    // Seek so the next decodeNext() resumes at the keyframe at or before time `t` (seconds; at or
    // before 0, the start of the file). Where that fails -- or in a raw stream, which has no times to
    // search by -- it goes to the start of the file instead, and the caller decodes forward to its
    // target: slow, but right. Flushes the decoders, the queued packets and the pending audio, so
    // nothing stale leaks across. False when not even the start could be reached.
    bool seek(double t);

    // Decode the next video frame in source order WITHOUT converting it. Audio met on the way is
    // decoded into the pending buffer (see takeAudio). False at the end of the stream.
    bool decodeNext(DecodedFrame& out);

    // Convert a decoded frame to RGBA8 -- rows TOP-DOWN, `dstStride` (at least width * 4) bytes
    // apart -- with threaded swscale. False on failure (e.g. the frame's size changed mid-stream).
    bool convert(const DecodedFrame& f, std::uint8_t* dst, int dstStride);

    // Read ahead -- queueing video packets instead of decoding them -- until the audio is settled up
    // to time `t` (see audioSettledUpTo), the input ends, or kMaxQueuedBytes of video packets
    // are waiting. `waitingForAudio`: the caller cannot go on without this audio (an offline render),
    // so pumpAudio may give it up when stuck (see audioSettledUpTo). A call without it gives nothing
    // up, and forgets what an earlier call gave up.
    void pumpAudio(double t, bool waitingForAudio = false);

    // Tests only: cap the read-ahead at `bytes` instead of kMaxQueuedBytes.
    void setMaxQueuedBytes(std::size_t bytes) { maxQueuedBytes_ = bytes; }

    // The time up to which no more audio will arrive: the end of the decoded audio, or -- once
    // the demuxer has read kAudioSettleSlack past a point without meeting audio for it -- that
    // point. +inf at the end of the input or with no audio stream. A caller waiting for audio can
    // get stuck: when pumpAudio(t, true) finds the queue still at kMaxQueuedBytes with no packet
    // taken off it since it last stopped there, the caller has stopped decoding -- its frames all
    // wait on this audio -- and nothing will ever drain the queue, so the audio counts as settled up
    // to the latest packet read. (Reading further would take unbounded memory: 4K ProRes runs past
    // 64 MB in under a second.) While packets are still being taken it only waits: audio that a file
    // puts after a run of video (fragmented MOV writes each fragment's video, then its audio) still
    // arrives. Only a waiting caller gives audio up: a live pause stops decoding too, and audio given
    // up then would be merely late once a render starts.
    double audioSettledUpTo() const;

    // Move out the next run of audio decoded since the last call (48 kHz mono float): samples
    // back to back from `startT`, the time of the first. The source's timestamps are followed --
    // a gap or an overlap of more than kAudioJitter starts a new run where the audio resumes --
    // and `continues` says whether this run carries straight on from the previous one (false for
    // the first run after open() or a seek). False when there is none: call until then.
    bool takeAudio(std::vector<float>& out, double& startT, bool& continues);

    // Decode the next video frame in source order into `out`, converted to bottom-up RGBA. Any
    // audio decoded on the way (audio packets interleaved before this video frame) is appended
    // to `audio` as 48 kHz mono float, its runs back to back. The first time audio is appended
    // into a freshly-cleared `audio`, `audioStartT` is set to that audio's time and
    // `audioStartValid` to true (left untouched on later calls so a multi-call fill keeps one
    // contiguous timeline). Returns false at end of stream.
    bool decodeFrame(VideoFrame& out, std::vector<float>& audio,
                     double& audioStartT, bool& audioStartValid);

    static constexpr std::size_t kMaxQueuedBytes   = std::size_t(64) << 20;  // read-ahead cap
    static constexpr double      kAudioSettleSlack = 2.0;                    // seconds
    // Seconds an audio timestamp may stray from where the audio so far ends and still carry on
    // back to back: well above timestamp rounding (Matroska and FLV keep milliseconds), which must
    // not click, and under the ~45 ms by which audio leading the picture starts to show -- a hole
    // shorter than this is closed up, and what follows it plays that much early.
    static constexpr double      kAudioJitter      = 0.04;

private:
    // Decoded audio, samples back to back from `start` (container time).
    struct AudioRun { double start = 0.0; bool continues = false; std::vector<float> s; };

    void close();
    void resetStreamState();
    void clearQueue();
    bool readPacket();
    void drainAudio();
    void placeAudio(double start, const float* s, std::size_t n);

    AVFormatContext* fmt_     = nullptr;
    AVCodecContext*  vctx_    = nullptr;   // video decoder
    AVCodecContext*  actx_    = nullptr;   // audio decoder (null if no audio)
    SwsContext*      sws_     = nullptr;   // decodeFrame(): -> RGBA, single-threaded, bottom-up; built on first use
    SwsContext*      swsThr_  = nullptr;   // convert(): -> RGBA, threaded, top-down
    int              swsThrFmt_ = -1;      // the pixel format swsThr_ was built for
    SwrContext*      swr_     = nullptr;   // -> 48 kHz mono float, for the input below
    int              swrFormat_ = -1, swrRate_ = 0;
    std::uint64_t    swrLayout_ = 0;       // the input channel mask (0: not a mask)...
    int              swrChannels_ = 0;     // ...and count
    AVFrame*         frame_   = nullptr;   // reused decode target (video or audio)
    AVFrame*         dstFrame_ = nullptr;  // convert()'s destination wrapper
    AVPacket*        pkt_     = nullptr;

    std::deque<AVPacket*> vq_;             // video packets read ahead, not yet decoded
    std::size_t           queuedBytes_ = 0;
    std::size_t           maxQueuedBytes_ = kMaxQueuedBytes;

    int    vstream_ = -1;
    int    astream_ = -1;
    int    width_   = 0;
    int    height_  = 0;
    double duration_   = 0.0;
    int    audioChannels_ = 0;  // source audio channel count (0 if no audio)
    double vTimeBase_  = 0.0;   // seconds per video stream tick
    bool   raw_        = false; // a raw stream (H.264, HEVC...): no times to seek by
    bool   indexUsable_ = true; // false: the container's index lists more than keyframes (MPEG-TS/-PS)
    double startT_     = 0.0;   // the container's time of the first video frame: subtracted from
                                // every time handed out, added to every time taken in
    double nextT_      = 0.0;   // container time a frame with no timestamp is given: just after the
                                // last; NaN when unknown (see decodeNext)
    DecodedFrame first_;        // decoded by open() to find startT_; the first decodeNext() returns it
    double aTimeBase_  = 0.0;   // seconds per audio stream tick
    bool   demuxEof_   = false; // read the last packet (and flushed the audio decoder)
    bool   vflushed_   = false; // sent the video decoder its end of stream
    double demuxedT_   = -std::numeric_limits<double>::infinity();   // latest packet time read (container time)
    double audioEndT_  = -std::numeric_limits<double>::infinity();   // end of the decoded audio (container time)
    double capSettledT_ = -std::numeric_limits<double>::infinity();  // settled when the read-ahead got stuck at the cap
    std::uint64_t packetsTaken_ = 0;                                  // video packets taken off the queue so far...
    std::uint64_t takenAtCap_   = ~std::uint64_t(0);                  // ...when pumpAudio last stopped at the cap
    double audioFloorT_ = -std::numeric_limits<double>::infinity();  // audio before this is dropped (the first frame)
    bool   runOpen_    = false; // the last audio decoded was kept: audio carrying on from it continues its run

    std::deque<AudioRun>      audioRuns_;             // decoded since the last takeAudio(), oldest first
    std::vector<std::uint8_t> rgba_;                  // decodeFrame()'s RGBA target
    std::vector<float>        aScratch_;              // reused swr output scratch
    std::vector<float>        legacyAudio_;           // decodeFrame()'s takeAudio scratch
};

} // namespace oss
```

- [ ] **Step 8: Replace `src/gfx/VideoDecoder.cpp`** — `src/gfx/VideoDecoder.cpp` (complete file)

```cpp
#include "gfx/VideoDecoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace oss {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kSameTime = 1e-6;   // seconds: times this close are the same frame's

// FFmpeg polls this during blocking I/O; returning 1 aborts the read.
int abortRequested(void* opaque) {
    const auto* flag = static_cast<const std::atomic<bool>*>(opaque);
    return flag && flag->load(std::memory_order_relaxed) ? 1 : 0;
}

// sws_scale_frame() writes only into reference-counted frames, so convert() wraps the caller's
// buffer in a reference whose free callback leaves the memory alone.
void keepBuffer(void*, std::uint8_t*) {}

} // namespace

DecodedFrame::~DecodedFrame() { reset(); }

DecodedFrame::DecodedFrame(DecodedFrame&& o) noexcept : t(o.t), frame_(o.frame_) { o.frame_ = nullptr; }

DecodedFrame& DecodedFrame::operator=(DecodedFrame&& o) noexcept {
    if (this != &o) {
        reset();
        t = o.t;
        frame_ = o.frame_;
        o.frame_ = nullptr;
    }
    return *this;
}

void DecodedFrame::reset() {
    if (frame_) av_frame_free(&frame_);
}

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close() {
    first_.reset();
    clearQueue();
    if (sws_)      { sws_freeContext(sws_); sws_ = nullptr; }
    if (swsThr_)   { sws_freeContext(swsThr_); swsThr_ = nullptr; }
    swsThrFmt_ = -1;
    if (swr_)      { swr_free(&swr_); }
    swrFormat_ = -1; swrRate_ = 0; swrLayout_ = 0; swrChannels_ = 0;
    if (vctx_)     avcodec_free_context(&vctx_);
    if (actx_)     avcodec_free_context(&actx_);
    if (pkt_)      av_packet_free(&pkt_);
    if (frame_)    av_frame_free(&frame_);
    if (dstFrame_) av_frame_free(&dstFrame_);
    if (fmt_)      avformat_close_input(&fmt_);
    vstream_ = astream_ = -1;
    width_ = height_ = 0;
    audioChannels_ = 0;
    duration_ = vTimeBase_ = aTimeBase_ = startT_ = nextT_ = 0.0;
    raw_ = false;
    audioFloorT_ = -kInf;
    resetStreamState();
}

void VideoDecoder::resetStreamState() {
    demuxEof_ = vflushed_ = runOpen_ = false;
    demuxedT_ = audioEndT_ = capSettledT_ = -kInf;
    takenAtCap_ = ~std::uint64_t(0);
    audioRuns_.clear();
}

void VideoDecoder::clearQueue() {
    for (AVPacket* p : vq_) av_packet_free(&p);
    vq_.clear();
    queuedBytes_ = 0;
}

bool VideoDecoder::open(const std::string& path, std::string& err, const std::atomic<bool>* abort) {
    close();

    fmt_ = avformat_alloc_context();
    if (!fmt_) { err = "out of memory"; return false; }
    if (abort) {
        fmt_->interrupt_callback.callback = abortRequested;
        fmt_->interrupt_callback.opaque   = const_cast<std::atomic<bool>*>(abort);
    }
    if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) {   // frees fmt_ on failure
        err = "could not open file"; return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        err = "could not read stream info"; close(); return false;
    }
    // Stopped while probing: the probe gives up part-way but still succeeds, with the streams half known.
    if (abort && abort->load()) { err = "stopped"; close(); return false; }

    vstream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vstream_ < 0) { err = "no video stream"; close(); return false; }
    astream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, vstream_, nullptr, 0);  // the video's; may be < 0

    // --- Video decoder ---
    AVStream* vs = fmt_->streams[vstream_];
    const AVCodec* vcodec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!vcodec) { err = "unsupported video codec"; close(); return false; }
    vctx_ = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(vctx_, vs->codecpar);
    vctx_->pkt_timebase = vs->time_base;
    vctx_->thread_count = 0;   // automatic: a decode thread per core (libavcodec defaults to one)
    if (avcodec_open2(vctx_, vcodec, nullptr) < 0) {
        err = "could not open video decoder"; close(); return false;
    }
    width_     = vctx_->width;
    height_    = vctx_->height;
    vTimeBase_ = av_q2d(vs->time_base);
    if (width_ <= 0 || height_ <= 0) { err = "video has no dimensions"; close(); return false; }
    rgba_.assign((std::size_t)width_ * height_ * 4, 0);

    // --- Audio decoder (optional; swr is set up lazily on the first frame so it
    //     matches the decoder's real output format) ---
    if (astream_ >= 0) {
        AVStream* as = fmt_->streams[astream_];
        const AVCodec* acodec = avcodec_find_decoder(as->codecpar->codec_id);
        if (acodec) {
            actx_ = avcodec_alloc_context3(acodec);
            avcodec_parameters_to_context(actx_, as->codecpar);
            actx_->pkt_timebase = as->time_base;   // lets it re-time what it skips (AAC priming)
            if (avcodec_open2(actx_, acodec, nullptr) == 0) {
                aTimeBase_ = av_q2d(as->time_base);
                audioChannels_ = as->codecpar->ch_layout.nb_channels;
            } else {
                avcodec_free_context(&actx_); actx_ = nullptr; astream_ = -1;
            }
        } else {
            astream_ = -1;
        }
    }

    duration_ = (fmt_->duration > 0) ? (double)fmt_->duration / AV_TIME_BASE : 0.0;
    // A raw stream (H.264, HEVC, MPEG-2 video...) has no times to seek by: a search reads the whole file
    // and then fails. seek() takes it back to the start instead, so its index is no guide either; nor are
    // the MPEG-TS and MPEG-PS demuxers', which add every packet they read while searching for a seek
    // position to the index, flagged as a keyframe.
    const char* format = fmt_->iformat && fmt_->iformat->name ? fmt_->iformat->name : "";
    raw_ = fmt_->iformat && (fmt_->iformat->flags & AVFMT_NOTIMESTAMPS);
    indexUsable_ = !raw_ && std::strncmp(format, "mpegts", 6) != 0 && std::strcmp(format, "mpeg") != 0;

    frame_    = av_frame_alloc();
    dstFrame_ = av_frame_alloc();
    pkt_      = av_packet_alloc();
    resetStreamState();

    // Some demuxers read their keyframe index only when first asked to seek (Matroska and WebM cues):
    // ask now, at the start, so the index is there from the first lap.
    seek(0.0);

    // Count time from the first frame (see the class comment). The duration spans from the start of
    // the container's clock, so shorten it by however much later than that the first frame comes. (An
    // FLV's duration counts from 0 instead: its laps end a B-frame delay late -- the last frame is held
    // a frame or two longer, never lost.)
    DecodedFrame f;
    if (!decodeNext(f)) { err = "no video frame decodes"; close(); return false; }
    startT_ = f.t;
    f.t = 0.0;
    first_ = std::move(f);
    const double clockStart = fmt_->start_time != AV_NOPTS_VALUE ? (double)fmt_->start_time / AV_TIME_BASE : 0.0;
    if (duration_ > 0.0) duration_ = std::max(0.0, duration_ - (startT_ - clockStart));

    // Audio read while looking for that frame was kept whatever its time: keep only what follows it.
    audioFloorT_ = startT_;
    std::deque<AudioRun> early;
    early.swap(audioRuns_);
    audioEndT_ = -kInf;
    runOpen_ = false;
    for (const AudioRun& r : early) placeAudio(r.start, r.s.data(), r.s.size());
    return true;
}

double VideoDecoder::frameDuration() const {
    if (!fmt_ || vstream_ < 0) return 1.0 / 30.0;
    const AVStream* vs = fmt_->streams[vstream_];
    // A raw stream's demuxer knows only a default rate (25 fps); its decoder read the real one from the
    // stream (H.264 and HEVC timing) when open() decoded the first frame.
    AVRational r = raw_ && vctx_ ? vctx_->framerate : vs->avg_frame_rate;
    if (r.num <= 0 || r.den <= 0) r = vs->avg_frame_rate;
    if (r.num <= 0 || r.den <= 0) r = vs->r_frame_rate;
    if (r.num <= 0 || r.den <= 0) return 1.0 / 30.0;
    return (double)r.den / (double)r.num;
}

bool VideoDecoder::nextKeyframeAfter(double t, double& key) const {
    if (!fmt_ || vstream_ < 0 || vTimeBase_ <= 0.0) return false;
    if (!indexUsable_) { key = kInf; return true; }
    AVStream* vs = fmt_->streams[vstream_];
    if (avformat_index_get_entries_count(vs) <= 0) return false;
    // Strictly after t. t is often a frame's own time, worked out from its timestamp, and dividing it back
    // by the time base can land a hair below that tick (1.16 s at 12800 ticks/s, or a whole tick below where
    // a tick is a frame, as in AVI): so walk on from the first keyframe at or after t's tick past any that
    // are at t.
    const AVIndexEntry* e = avformat_index_get_entry_from_timestamp(vs, (int64_t)std::floor((t + startT_) / vTimeBase_), 0);
    while (e && e->timestamp * vTimeBase_ - startT_ <= t + kSameTime)                  // keyframes, >= the tick
        e = avformat_index_get_entry_from_timestamp(vs, e->timestamp + 1, 0);
    key = e ? e->timestamp * vTimeBase_ - startT_ : kInf;
    return true;
}

bool VideoDecoder::seek(double t) {
    if (!fmt_) return false;
    first_.reset();
    // At or before 0, the very start of the file rather than the first frame's time: a demuxer that
    // searches by timestamp (MPEG-TS) can overshoot that by a keyframe interval.
    const double src = t > 0.0 ? t + startT_ : 0.0;
    int64_t ts = (int64_t)(src / (vTimeBase_ > 0 ? vTimeBase_ : 1.0));
    // BACKWARD lands on the keyframe at or before ts -- exactly the GOP start the caller decodes
    // forward from. If that fails, restart from the beginning; and if even that fails -- or in a raw
    // stream, where a search by time reads the whole file and fails -- from the first byte of data.
    bool ok = !raw_ && av_seek_frame(fmt_, vstream_, ts, AVSEEK_FLAG_BACKWARD) >= 0;
    bool atStart = t <= 0.0;
    if (!ok && !raw_ && t > 0.0) {
        ok = av_seek_frame(fmt_, vstream_, 0, AVSEEK_FLAG_BACKWARD) >= 0;
        atStart = true;
    }
    if (!ok) {
        ok = av_seek_frame(fmt_, -1, 0, AVSEEK_FLAG_BYTE) >= 0;
        atStart = true;
    }
    if (vctx_) avcodec_flush_buffers(vctx_);
    if (actx_) avcodec_flush_buffers(actx_);
    if (swr_) swr_free(&swr_);   // it holds samples from before the seek
    clearQueue();
    resetStreamState();
    nextT_ = atStart ? startT_ : kNaN;   // a first frame with no timestamp: the file's first, or unknown
    return ok;
}

// Read one packet: a video packet joins the queue, an audio packet is decoded into the pending
// buffer. False at the end of the input, after flushing the audio decoder.
bool VideoDecoder::readPacket() {
    if (demuxEof_) return false;
    if (av_read_frame(fmt_, pkt_) < 0) {
        demuxEof_ = true;
        if (actx_) { avcodec_send_packet(actx_, nullptr); drainAudio(); }
        return false;
    }
    const AVStream* st = fmt_->streams[pkt_->stream_index];
    const int64_t ts = pkt_->pts != AV_NOPTS_VALUE ? pkt_->pts : pkt_->dts;
    const bool ours = pkt_->stream_index == vstream_ || pkt_->stream_index == astream_;   // not another program's
    if (ours && ts != AV_NOPTS_VALUE) demuxedT_ = std::max(demuxedT_, ts * av_q2d(st->time_base));
    if (pkt_->stream_index == vstream_) {
        if (AVPacket* q = av_packet_alloc()) {
            av_packet_move_ref(q, pkt_);
            queuedBytes_ += (std::size_t)q->size;
            vq_.push_back(q);
            return true;
        }
    } else if (actx_ && pkt_->stream_index == astream_) {
        avcodec_send_packet(actx_, pkt_);
        drainAudio();
    }
    av_packet_unref(pkt_);
    return true;
}

// Receive every audio frame the decoder has buffered, resample each to 48 kHz mono float, and
// place it in the pending runs at its source time.
void VideoDecoder::drainAudio() {
    if (!actx_) return;
    while (avcodec_receive_frame(actx_, frame_) == 0) {
        // (Re)build the resampler for this frame's format: a stream can change it midway (a broadcast
        // recording going from 5.1 to stereo), and one built for more channels reads planes that are gone.
        const AVChannelLayout& cl = frame_->ch_layout;
        const std::uint64_t mask = cl.order == AV_CHANNEL_ORDER_NATIVE ? cl.u.mask : 0;
        if (!swr_ || frame_->format != swrFormat_ || frame_->sample_rate != swrRate_ ||
            cl.nb_channels != swrChannels_ || mask != swrLayout_) {
            if (swr_) swr_free(&swr_);
            AVChannelLayout outLayout{}, inLayout{};
            av_channel_layout_default(&outLayout, 1);   // mono
            if (cl.nb_channels > 0) av_channel_layout_copy(&inLayout, &cl);
            else                    av_channel_layout_default(&inLayout, 1);
            int rc = swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_FLT, kOutRate,
                                         &inLayout, (AVSampleFormat)frame_->format,
                                         frame_->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&outLayout);
            av_channel_layout_uninit(&inLayout);
            if (rc < 0 || !swr_ || swr_init(swr_) < 0) {
                if (swr_) swr_free(&swr_);
                swrFormat_ = -1;
                av_frame_unref(frame_);
                continue;   // can't resample this frame; skip it
            }
            swrFormat_ = frame_->format; swrRate_ = frame_->sample_rate;
            swrChannels_ = cl.nb_channels; swrLayout_ = mask;
        }
        const double start = frame_->pts != AV_NOPTS_VALUE ? frame_->pts * aTimeBase_
                           : (std::isfinite(audioEndT_) ? audioEndT_ : 0.0);
        int outCount = swr_get_out_samples(swr_, frame_->nb_samples);
        if (outCount > 0) {
            if ((int)aScratch_.size() < outCount) aScratch_.resize(outCount);
            uint8_t* outptr = (uint8_t*)aScratch_.data();
            int got = swr_convert(swr_, &outptr, outCount,
                                  (const uint8_t**)frame_->extended_data, frame_->nb_samples);
            if (got > 0) placeAudio(start, aScratch_.data(), (std::size_t)got);
        }
        av_frame_unref(frame_);
    }
}

// Queue `n` samples whose first is at container time `start`, following the source's timestamps: within
// kAudioJitter of where the audio so far ends they carry straight on; further off -- a gap, or an overlap
// -- they start a new run at their own time. What precedes the first video frame is dropped.
void VideoDecoder::placeAudio(double start, const float* s, std::size_t n) {
    const bool near = std::isfinite(audioEndT_) && std::fabs(start - audioEndT_) <= kAudioJitter;
    if (near) start = audioEndT_;
    audioEndT_ = start + (double)n / kOutRate;
    std::size_t skip = 0;
    if (start < audioFloorT_) {
        skip = std::min(n, (std::size_t)std::llround((audioFloorT_ - start) * kOutRate));
        start = std::max(audioFloorT_, start + (double)skip / kOutRate);
    }
    const bool continues = near && runOpen_ && skip == 0;
    runOpen_ = skip < n;
    if (skip == n) return;
    if (continues && !audioRuns_.empty()) {
        std::vector<float>& run = audioRuns_.back().s;
        run.insert(run.end(), s + skip, s + n);
    } else {
        audioRuns_.push_back(AudioRun{start, continues, std::vector<float>(s + skip, s + n)});
    }
}

void VideoDecoder::pumpAudio(double t, bool waitingForAudio) {
    if (!fmt_ || !actx_) return;
    if (!waitingForAudio) { capSettledT_ = -kInf; takenAtCap_ = ~std::uint64_t(0); }   // gives nothing up
    while (audioSettledUpTo() < t) {
        if (queuedBytes_ >= maxQueuedBytes_) {
            // Full. A waiting caller that took nothing off the queue since the last time is stuck: what was
            // read counts as settled (see the header).
            if (waitingForAudio) {
                if (packetsTaken_ == takenAtCap_) capSettledT_ = std::max(capSettledT_, demuxedT_);
                takenAtCap_ = packetsTaken_;
            }
            return;
        }
        if (!readPacket()) return;
    }
}

double VideoDecoder::audioSettledUpTo() const {
    if (!actx_ || demuxEof_) return kInf;
    return std::max({audioEndT_, demuxedT_ - kAudioSettleSlack, capSettledT_}) - startT_;
}

bool VideoDecoder::takeAudio(std::vector<float>& out, double& startT, bool& continues) {
    out.clear();
    if (audioRuns_.empty()) return false;
    AudioRun& run = audioRuns_.front();
    out.swap(run.s);
    startT = run.start - startT_;
    continues = run.continues;
    audioRuns_.pop_front();
    return true;
}

bool VideoDecoder::decodeNext(DecodedFrame& out) {
    out.reset();
    if (first_.valid()) { out = std::move(first_); return true; }
    if (!fmt_ || !vctx_) return false;

    while (true) {
        const int r = avcodec_receive_frame(vctx_, frame_);
        if (r == 0) {
            const int64_t ts = (frame_->best_effort_timestamp != AV_NOPTS_VALUE)
                             ? frame_->best_effort_timestamp : frame_->pts;
            // A frame with no timestamp follows the one before it. With none before it since a seek into
            // the file there is no telling where it is: skip it (after a seek into the last keyframe interval
            // of an MPEG-PS, the decoder may put out nothing else).
            if (ts == AV_NOPTS_VALUE && std::isnan(nextT_)) { av_frame_unref(frame_); continue; }
            const double t = ts != AV_NOPTS_VALUE ? ts * vTimeBase_ : nextT_;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 30, 100)
            const int64_t dur = frame_->duration;                // FFmpeg 6.0 and later
#else
            const int64_t dur = frame_->pkt_duration;            // FFmpeg 5.1
#endif
            nextT_ = t + (dur > 0 ? dur * vTimeBase_ : frameDuration());
            out.t = t - startT_;
            out.frame_ = av_frame_alloc();
            if (!out.frame_) { av_frame_unref(frame_); return false; }
            av_frame_move_ref(out.frame_, frame_);
            return true;
        }
        if (r == AVERROR_EOF || vflushed_) return false;   // fully drained / nothing more coming

        // r == AVERROR(EAGAIN): feed the decoder its next video packet, reading more as needed.
        while (vq_.empty() && readPacket()) {}
        if (vq_.empty()) {                                  // end of input: flush the decoder
            avcodec_send_packet(vctx_, nullptr);
            vflushed_ = true;
            continue;                                       // receive the frames it still holds
        }
        AVPacket* p = vq_.front();
        vq_.pop_front();
        queuedBytes_ -= (std::size_t)p->size;
        ++packetsTaken_;
        avcodec_send_packet(vctx_, p);
        av_packet_free(&p);
    }
}

bool VideoDecoder::convert(const DecodedFrame& f, std::uint8_t* dst, int dstStride) {
    const AVFrame* src = f.frame_;
    if (!src || !dst || dstStride < width_ * 4 || src->width != width_ || src->height != height_) return false;
    if (!swsThr_ || swsThrFmt_ != src->format) {
        if (swsThr_) sws_freeContext(swsThr_);
        swsThr_ = sws_alloc_context();
        if (!swsThr_) return false;
        av_opt_set_int(swsThr_, "srcw", width_, 0);
        av_opt_set_int(swsThr_, "srch", height_, 0);
        av_opt_set_int(swsThr_, "src_format", src->format, 0);
        av_opt_set_int(swsThr_, "dstw", width_, 0);
        av_opt_set_int(swsThr_, "dsth", height_, 0);
        av_opt_set_int(swsThr_, "dst_format", AV_PIX_FMT_RGBA, 0);
        av_opt_set_int(swsThr_, "sws_flags", SWS_BILINEAR, 0);
        av_opt_set_int(swsThr_, "threads", 0, 0);   // automatic: a slice thread per core
        if (sws_init_context(swsThr_, nullptr, nullptr) < 0) {
            sws_freeContext(swsThr_); swsThr_ = nullptr; return false;
        }
        swsThrFmt_ = src->format;
    }
    av_frame_unref(dstFrame_);
    dstFrame_->format = AV_PIX_FMT_RGBA;
    dstFrame_->width  = width_;
    dstFrame_->height = height_;
    dstFrame_->buf[0] = av_buffer_create(dst, (std::size_t)dstStride * height_, keepBuffer, nullptr, 0);
    if (!dstFrame_->buf[0]) return false;
    dstFrame_->data[0]     = dst;
    dstFrame_->linesize[0] = dstStride;
    const int r = sws_scale_frame(swsThr_, dstFrame_, src);
    av_frame_unref(dstFrame_);   // drops our reference; keepBuffer leaves the memory alone
    return r >= 0;
}

bool VideoDecoder::decodeFrame(VideoFrame& out, std::vector<float>& audio,
                               double& audioStartT, bool& audioStartValid) {
    DecodedFrame f;
    const bool ok = decodeNext(f);
    double start = 0.0;
    bool continues = false;
    while (takeAudio(legacyAudio_, start, continues)) {
        if (!audioStartValid) { audioStartT = start; audioStartValid = true; }
        audio.insert(audio.end(), legacyAudio_.begin(), legacyAudio_.end());
    }
    if (!ok) return false;

    // Built from the frame's own format: the stream's may not be known until a frame decodes (a video that
    // starts past what the probe reads).
    sws_ = sws_getCachedContext(sws_, width_, height_, (AVPixelFormat)f.frame_->format,
                                width_, height_, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return false;
    // Flip vertically (negative stride from the last row) so the buffer is bottom-up, matching how
    // the rest of the app's textures are oriented.
    uint8_t* dst[4] = { rgba_.data() + (std::size_t)(height_ - 1) * width_ * 4,
                        nullptr, nullptr, nullptr };
    int dstStride[4] = { -width_ * 4, 0, 0, 0 };
    sws_scale(sws_, f.frame_->data, f.frame_->linesize, 0, height_, dst, dstStride);
    out.t      = f.t;
    out.width  = width_;
    out.height = height_;
    out.rgba   = rgba_.data();
    return true;
}

} // namespace oss
```

- [ ] **Step 9: Build and run the scenarios (the second prints only if the first passed: `gl_smoke` stops at a failure)**

```bash
cmake --build build --target gl_smoke -j8 && ./build/gl_smoke 2>&1 | grep -E 'decodeNext|untimed frames|FAIL'
```

Expected output includes: `untimed frames follow the one before`

- [ ] **Step 10: Run everything: the old Video Player still builds on the legacy API and every existing scenario (encoder round-trips, offline renders) still passes**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure
```

Expected output includes: `100% tests passed`

- [ ] **Step 11: Commit**

```bash
git add src/gfx/VideoDecoder.h src/gfx/VideoDecoder.cpp tests/gl_smoke.cpp
git commit -F - <<'EOF'
feat(gfx): split VideoDecoder decode from conversion, thread it, read audio ahead

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

### Task 6: VideoEncoder: an optional fixed keyframe interval

**Files:**
- Modify: `src/gfx/VideoEncoder.h`, `src/gfx/VideoEncoder.cpp`
- Modify: `tests/gl_smoke.cpp`

The regression clips need keyframes far apart (x264's default of 250 frames is what exposed the lock-up) or exactly placed. A trailing defaulted parameter keeps every existing caller unchanged. An interval is held exactly only by libx264 (with its scene-cut keyframes off) and by the MPEG-4 fallback (with its scene-change detection off) — the other H.264 encoders cannot be relied on to hold one (VideoToolbox keys every scene cut regardless) — so a clip with an interval is written with one of those two, and every Nth frame is forced to be a keyframe (MPEG-4's B-frames would otherwise move odd intervals by a frame). The scenario also walks the index of an MP4 and an AVI whose every frame is a keyframe: Task 5's `nextKeyframeAfter` must move strictly forward (a frame's time divided back into ticks can land a hair, or a whole tick, below its own).

- [ ] **Step 1: Add the failing scenario, just above `// --- Scenario 10: Video Player decodes a file to texture + audio ---`**:

```cpp
// --- Scenario: VideoEncoder places keyframes exactly every keyframeInterval frames ---
// Hard cuts every 10 frames would earn scene-cut keyframes; with an explicit interval there must be
// none -- the long-keyframe test clips depend on it.
static bool scenario_video_encoder_keyframe_interval() {
    {
        const std::string path = "build/_enc_keyint.mp4";
        VideoEncoder enc; std::string err;
        if (!enc.open(path, 64, 48, 25, 0, 0, err, 50)) { return failed(("keyint: open: " + err).c_str()); }
        std::vector<unsigned char> px((std::size_t)64 * 48 * 4);
        for (int f = 0; f < 120; ++f) {
            std::fill(px.begin(), px.end(), (unsigned char)(((f / 10) & 1) ? 255 : 0));
            if (!enc.addVideoFrame(px.data(), f / 25.0)) { return failed("keyint: add frame"); }
        }
        if (!enc.close(err)) { return failed(("keyint: close: " + err).c_str()); }
        VideoDecoder dec;
        if (!dec.open(path, err)) { return failed(("keyint: decode: " + err).c_str()); }
        double k1 = 0.0, k2 = 0.0;
        if (!dec.nextKeyframeAfter(0.1, k1) || std::fabs(k1 - 2.0) > 0.1 ||
            !dec.nextKeyframeAfter(2.1, k2) || std::fabs(k2 - 4.0) > 0.1) {
            std::fprintf(stderr, "keyint: keyframes after 0.1 s and 2.1 s at %.3f and %.3f\n", k1, k2);
            return failed("keyint: expected keyframes 50 frames apart -- at 2 s and 4 s (the index may list them by "
                          "decode time, a frame or two early) -- and none at the cuts between");
        }

        // Walking the index: nextKeyframeAfter(the key it gave last) moves strictly forward and ends. Every frame
        // is a keyframe here, so every frame's time is asked about -- and 1.16 s, divided back into ticks, lands a
        // hair below its tick (1/12800 s, MP4) or a whole tick below (a frame, AVI), which used to return that
        // same keyframe again, forever.
        for (const std::string every : {"build/_enc_keyint_every.mp4", "build/_enc_keyint_every.avi"}) {
            VideoEncoder e1;
            if (!e1.open(every, 64, 48, 25, 0, 0, err, 1)) { return failed(("keyint: open: " + err).c_str()); }
            for (int f = 0; f < 100; ++f) {
                std::fill(px.begin(), px.end(), (unsigned char)(f * 2));
                if (!e1.addVideoFrame(px.data(), f / 25.0)) { return failed("keyint: add frame"); }
            }
            if (!e1.close(err)) { return failed(("keyint: close: " + err).c_str()); }
            VideoDecoder w;
            if (!w.open(every, err)) { return failed(("keyint: decode: " + err).c_str()); }
            double k = -1.0, next = 0.0;
            int walked = 0;
            while (walked <= 100 && w.nextKeyframeAfter(k, next) && std::isfinite(next)) {
                if (!(next > k)) {
                    std::fprintf(stderr, "keyint: %s: nextKeyframeAfter(%.17g) gave %.17g\n", every.c_str(), k, next);
                    return failed("keyint: the keyframe after t must come strictly after it");
                }
                k = next;
                ++walked;
            }
            if (walked != 100) {
                std::fprintf(stderr, "keyint: %s: walked %d keyframes of 100\n", every.c_str(), walked);
                return failed("keyint: walking the index must visit every keyframe once");
            }
        }
        std::fprintf(stderr, "gl_smoke OK: an explicit keyframe interval places keyframes exactly (%.2f s, %.2f s); "
                     "walking the index visits every keyframe once\n", k1, k2);
    }
    return true;
}

```

- [ ] **Step 2: Register it in `kScenarios`, just above `scenario_video_player_decode,`**. In `tests/gl_smoke.cpp`, replace:

```cpp
    scenario_video_player_decode,
```

with:

```cpp
    scenario_video_encoder_keyframe_interval,
    scenario_video_player_decode,
```

- [ ] **Step 3: Build to verify it fails**

```bash
cmake --build build --target gl_smoke -j8
```

Expected: the build FAILS, with an error mentioning `too many arguments to function call`.

- [ ] **Step 4: In `src/gfx/VideoEncoder.h`, extend `open()`**. In `src/gfx/VideoEncoder.h`, replace:

```cpp
    // Open `path` for writing `width`x`height` video at a nominal `fps`. If
    // `audioRate` > 0 an AAC audio stream is added at that sample rate with
    // `audioChannels` channels (1 = mono, 2 = stereo). Returns false on failure.
    bool open(const std::string& path, int width, int height, int fps,
              int audioRate, int audioChannels, std::string& err);
```

with:

```cpp
    // Open `path` for writing `width`x`height` video at a nominal `fps`. If
    // `audioRate` > 0 an AAC audio stream is added at that sample rate with
    // `audioChannels` channels (1 = mono, 2 = stereo). `keyframeInterval` > 0 places
    // a keyframe exactly every that many frames and nowhere else -- tests use it to write
    // clips with widely spaced keyframes. Only libx264 and the MPEG-4 fallback can be held
    // to that (the other H.264 encoders cannot be relied on to: VideoToolbox keys every scene
    // cut regardless), so such a clip is written with one of those two.
    // At or below 0, keyframes are at most a second apart (scene cuts can add more).
    // Returns false on failure.
    bool open(const std::string& path, int width, int height, int fps,
              int audioRate, int audioChannels, std::string& err, int keyframeInterval = 0);
```

- [ ] **Step 5: In `src/gfx/VideoEncoder.h`, add the keyframe counters after `lastVpts_`**. In `src/gfx/VideoEncoder.h`, replace:

```cpp
    int64_t lastVpts_ = -1;          // last video pts (codec time base = 1/fps)
```

with:

```cpp
    int64_t lastVpts_ = -1;          // last video pts (codec time base = 1/fps)
    int     keyframeInterval_ = 0;   // > 0: every this many frames is forced to be a keyframe...
    int64_t framesSent_ = 0;         // ...counting the frames sent
```

- [ ] **Step 6: In `src/gfx/VideoEncoder.cpp`, the definition's signature**. In `src/gfx/VideoEncoder.cpp`, replace:

```cpp
bool VideoEncoder::open(const std::string& path, int width, int height, int fps,
                        int audioRate, int audioChannels, std::string& err) {
```

with:

```cpp
bool VideoEncoder::open(const std::string& path, int width, int height, int fps,
                        int audioRate, int audioChannels, std::string& err, int keyframeInterval) {
```

- [ ] **Step 7: In `src/gfx/VideoEncoder.cpp`, the choice of encoder**. In `src/gfx/VideoEncoder.cpp`, replace:

```cpp
    // --- Video stream (H.264, falling back to MPEG-4) ---
    const AVCodec* vc = avcodec_find_encoder_by_name("libx264");
    if (!vc) vc = avcodec_find_encoder(AV_CODEC_ID_H264);
```

with:

```cpp
    // --- Video stream (H.264, falling back to MPEG-4) ---
    // An exact keyframe interval rules out the H.264 encoders other than libx264: they cannot be relied on
    // to hold one (VideoToolbox keys every scene cut regardless).
    const AVCodec* vc = avcodec_find_encoder_by_name("libx264");
    if (!vc && keyframeInterval <= 0) vc = avcodec_find_encoder(AV_CODEC_ID_H264);
```

- [ ] **Step 8: In `src/gfx/VideoEncoder.cpp`, the GOP size**. In `src/gfx/VideoEncoder.cpp`, replace:

```cpp
    vctx_->gop_size  = fps;
```

with:

```cpp
    vctx_->gop_size  = keyframeInterval > 0 ? keyframeInterval : fps;
```

- [ ] **Step 9: In `src/gfx/VideoEncoder.cpp`, after the x264 `crf` option**. In `src/gfx/VideoEncoder.cpp`, replace:

```cpp
        av_opt_set(vctx_->priv_data, "crf",    "23",       0);
    }
```

with:

```cpp
        av_opt_set(vctx_->priv_data, "crf",    "23",       0);
        if (keyframeInterval > 0)   // exactly every N frames: no extra keyframes at scene cuts
            av_opt_set(vctx_->priv_data, "x264-params", "scenecut=0", 0);
    }
    if (keyframeInterval > 0 && vc->id == AV_CODEC_ID_MPEG4)   // likewise its scene-change keyframes
        av_opt_set(vctx_, "sc_threshold", "1000000000", AV_OPT_SEARCH_CHILDREN);
```

- [ ] **Step 10: In `src/gfx/VideoEncoder.cpp`, where `open()` resets its counters**. In `src/gfx/VideoEncoder.cpp`, replace:

```cpp
    pkt_ = av_packet_alloc();
    lastVpts_ = -1;
```

with:

```cpp
    pkt_ = av_packet_alloc();
    lastVpts_ = -1;
    keyframeInterval_ = keyframeInterval > 0 ? keyframeInterval : 0;
    framesSent_ = 0;
```

- [ ] **Step 11: In `src/gfx/VideoEncoder.cpp`, where `addVideoFrame()` sends the frame**. In `src/gfx/VideoEncoder.cpp`, replace:

```cpp
    vframe_->pts = pts;
    return encodeWrite(vctx_, vst_, vframe_);
```

with:

```cpp
    vframe_->pts = pts;
    // An exact interval forces its keyframes: MPEG-4's B-frames would otherwise move them by a frame.
    vframe_->pict_type = keyframeInterval_ > 0 && framesSent_ % keyframeInterval_ == 0 ? AV_PICTURE_TYPE_I
                                                                                        : AV_PICTURE_TYPE_NONE;
    ++framesSent_;
    return encodeWrite(vctx_, vst_, vframe_);
```

- [ ] **Step 12: Build and run the scenario**

```bash
cmake --build build --target gl_smoke -j8 && ./build/gl_smoke 2>&1 | grep -E 'keyframe interval|FAIL'
```

Expected output includes: `an explicit keyframe interval places keyframes exactly`

- [ ] **Step 13: Commit**

```bash
git add src/gfx/VideoEncoder.h src/gfx/VideoEncoder.cpp tests/gl_smoke.cpp
git commit -F - <<'EOF'
feat(gfx): let VideoEncoder place keyframes at a fixed interval

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

### Task 7: VideoStream: the background decoder

**Files:**
- Create: `src/gfx/VideoStream.h`, `src/gfx/VideoStream.cpp`
- Modify: `CMakeLists.txt` (`APP_SOURCES` and the `gl_smoke` sources)
- Modify: `tests/gl_smoke.cpp`

One worker thread per file. It opens the file, sizes a pool of RGBA buffers from the 512 MB budget, then loops: snapshot the request, recycle frames that can never be shown, read audio ahead, ask `videoNextStep()` what to do, and do it — never holding the mutex while decoding or converting. The graph thread only calls `request()`, `frameAt()` (the frame it returns is *checked out* until the next call), `readAudio()`, and offline `frameReadyFor()` / `waitForFrame()`. Details that the prototype showed matter: live catch-up is sliced to 100 ms so a decoder slower than the playhead still moves the picture; a seek keeps the newest frame at or before its target; a seek lands where its decode loop admits frames, retrying further back (1 s, 2 s, 4 s… to the file's start) when it lands late — a decode-time index lands a seek just below a keyframe ON it, a timestamp search (MPEG-TS) overshoots a keyframe interval, and some demuxers find nothing near the end — and is *pinned* only if even the start lands late; reverse stretches are published whole (they decode forwards); offline, a stretch whose ring evicted its lower frames covers only down to its oldest; a frame at or past the duration ends the lap. With loop off, frames past the playhead's lap are kept (they are the next ones if loop comes back on) and what reverse covered below the lap is dropped with its frames; runs end at the lap's end whether looping or not; a wrap forgets what the last seek taught about keyframes (`noSeekBelow`), which was about the old lap. An offline render that starts in reverse restarts the run — live stretches kept every stride-th frame — and until an offline stretch lands, reverse readiness says no. The decoder's audio runs follow its timestamps: the worker begins a new chunk wherever a run does not continue the one before (a hole in the source's audio then reads as silence), and a seek that cannot reach even the file's start fails the stream rather than being tried again every step. The worker passes its offline flag to `pumpAudio()`, so only an offline render — which cannot go on without the audio — lets the decoder give it up; `VideoStream` takes a read-ahead size so a test can cap it. The new scenarios write the awkward files themselves — a long first keyframe interval, an FLV with B-frames (those parts SKIP without an H.264 encoder), an AVI whose last keyframe is its last frame (a seek there yields only an untimed frame, which the decoder skips), a one-keyframe clip for loop toggles — and count reverse stretches (`reverseStretches()`; `seeks()` is its forward twin) to prove live reverse cannot spin. Offline readiness names the frame for u exactly: each queued frame carries `until`, the time of the next frame decoded after it, and a held frame must have t ≤ u < until — the node's prefetch asks for its GUESS at the next playhead, and an automated rate can move the real one onto a frame recycling released for the guess. While the guess's frame is held, recycling keeps the frames between the frame on screen and the guess (when the frame on screen is on the way to it); playing forward, when the real playhead's frame was released anyway while frames beyond it are held, or is held but lies before the run (a catch-up to a guess far ahead restarted it, and the playhead stopped short), the worker seeks back to decode it again — reverse needs no such help, since a guess below what the stretches cover gets a fresh stretch and leaves the real playhead stranded above it. Readiness says no while the worker has yet to flush for a new direction: the direction it planned for is guarded by the mutex and set with the flush. Tearing a stream down takes 0.1–0.5 s at 4K, so `retire()` hands it to a reaper thread (joined at exit) and returns at once. The fourth scenario drives offline renders as the node does — show the frame the moment it is ready, and readiness must still hold a moment later — through rate changes, flips, a jump, a loop-off end and an MPEG-TS, and destroys and retires streams mid-open.

- [ ] **Step 1: Add `#include "gfx/VideoStream.h"` to `tests/gl_smoke.cpp`, after `#include "gfx/VideoDecoder.h"`**. In `tests/gl_smoke.cpp`, replace:

```cpp
#include "gfx/VideoDecoder.h"
```

with:

```cpp
#include "gfx/VideoDecoder.h"
#include "gfx/VideoStream.h"
```

- [ ] **Step 2: Add a timing helper, the indexed test clip (each frame paints its own number), and the four failing scenarios, just above `// --- Scenario 10: Video Player decodes a file to texture + audio ---`**:

```cpp
// Seconds since t0.
static double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// A clip whose every frame paints its own index as 9 horizontal black/white bands (the top band is
// bit 8), 160x90 at 25 fps with a 440 Hz tone (unless `audio` is false), and keyframes exactly `gop`
// frames apart. Reading the bands back says which frame is on screen -- and a wrong vertical flip reads
// a different number.
static const int kIdxW = 160, kIdxH = 90, kIdxFps = 25;
static bool writeIndexedClip(const std::string& path, int frames, int gop, int w = kIdxW, int h = kIdxH,
                             bool audio = true) {
    VideoEncoder enc; std::string err;
    if (!enc.open(path, w, h, kIdxFps, audio ? 48000 : 0, 1, err, gop)) {
        std::fprintf(stderr, "writeIndexedClip: %s\n", err.c_str());
        return false;
    }
    std::vector<unsigned char> px((std::size_t)w * h * 4);
    std::vector<float> tone(48000 / kIdxFps);
    for (int f = 0; f < frames; ++f) {
        for (int y = 0; y < h; ++y) {                      // y = 0 is the BOTTOM row (GL order)
            const int bandFromTop = (h - 1 - y) * 9 / h;
            const unsigned char v = ((f >> (8 - bandFromTop)) & 1) ? 255 : 0;
            for (int x = 0; x < w; ++x) {
                std::size_t i = ((std::size_t)y * w + x) * 4;
                px[i] = px[i + 1] = px[i + 2] = v; px[i + 3] = 255;
            }
        }
        if (!enc.addVideoFrame(px.data(), (double)f / kIdxFps)) return false;
        if (!audio) continue;
        for (std::size_t i = 0; i < tone.size(); ++i)
            tone[i] = 0.5f * (float)std::sin(6.283185307179586 * 440.0 *
                                             (double)(f * tone.size() + i) / 48000.0);
        if (!enc.addAudio(tone.data(), (int)tone.size())) return false;
    }
    return enc.close(err);
}

// --- Scenario: VideoStream -- open, the exact frame for a playhead, its audio, a failed open ---
static bool scenario_video_stream_basics() {
    {
        VideoStream bad("tests/assets/does_not_exist.mp4");
        const auto t0 = std::chrono::steady_clock::now();
        while (bad.state() == VideoStream::State::Opening && secondsSince(t0) < 5.0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (bad.state() != VideoStream::State::Failed || bad.error().empty()) {
            return failed("video stream: a missing file should end Failed with a reason");
        }

        VideoStream s("tests/assets/test.mp4");
        while (s.state() == VideoStream::State::Opening && secondsSince(t0) < 10.0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (s.state() != VideoStream::State::Ready) { return failed("video stream: test.mp4 did not open"); }
        const VideoStream::Info inf = s.info();
        if (inf.width != 128 || inf.height != 96 || std::fabs(inf.frameDur - 0.1) > 1e-9 || !inf.hasAudio) {
            return failed("video stream: wrong info for test.mp4");
        }
        VideoRequest r;
        r.u = 0.73; r.rate = 1.0f; r.offline = true; r.loop = false; r.lapLo = 0.0; r.lapHi = inf.duration;
        s.request(r);
        if (!s.waitForFrame(0.73, 5.0)) { return failed("video stream: the frame for 0.73 s never became ready"); }
        VideoStream::FrameView fv;
        if (!s.frameAt(0.73, fv) || std::fabs(fv.t - 0.7) > 1e-6) {
            return failed("video stream: the frame for 0.73 s must be the one at 0.7 s");
        }
        std::vector<float> a(4800);
        s.readAudio(0.63, 0.73, a.data(), (int)a.size());
        bool loud = false;
        for (float v : a) if (v > 0.01f || v < -0.01f) { loud = true; break; }
        if (!loud) { return failed("video stream: no audio for the slice before the playhead"); }

        // Audio with a hole in its timestamps plays with the hole: silent across it, and what follows at
        // its own time. Rendered offline from 0, reading each frame's audio the way the node does: the
        // slice since the last frame, up to the playhead (as far as readiness says it is settled).
        const std::string hole = "build/_stream_audio_hole.mkv";
        if (!writeAudioHoleClip(hole)) { return failed("video stream: writing the audio-hole clip failed"); }
        VideoStream h(hole);
        while (h.state() == VideoStream::State::Opening && secondsSince(t0) < 20.0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (h.state() != VideoStream::State::Ready) { return failed("video stream: the audio-hole clip did not open"); }
        std::vector<float> out, blk(1920);
        for (int k = 1; k <= 50; ++k) {
            VideoRequest hr;
            hr.u = k / 25.0; hr.rate = 1.0f; hr.offline = true;
            h.request(hr);
            VideoStream::FrameView hv;
            if (!h.waitForFrame(hr.u, 5.0) || !h.frameAt(hr.u, hv)) { return failed("video stream: the audio-hole clip stalled"); }
            h.readAudio(hr.u - 0.04, hr.u, blk.data(), (int)blk.size());
            out.insert(out.end(), blk.begin(), blk.end());
        }
        auto rms = [&](double t0s, double t1s) {
            double e = 0.0;
            const std::size_t i0 = (std::size_t)(t0s * 48000), i1 = (std::size_t)(t1s * 48000);
            for (std::size_t i = i0; i < i1; ++i) e += (double)out[i] * out[i];
            return std::sqrt(e / (double)(i1 - i0));
        };
        if (rms(1.1, 1.5) > 1e-3 || rms(1.7, 1.9) < 0.1) {
            std::fprintf(stderr, "video stream: audio RMS %.4f inside the hole, %.4f after it\n", rms(1.1, 1.5), rms(1.7, 1.9));
            return failed("video stream: the audio after a hole must play at its own time, with silence across the hole");
        }

        // An offline render must not wait forever for audio the read-ahead cannot reach: with it capped at a
        // byte, 4 buffers and the audio starting 1.5 s in, the worker's frames all wait on audio nothing will
        // read, so it gives that audio up (VideoDecoder::pumpAudio) -- and only then.
        const std::string lateAudio = "build/_stream_late_audio.mkv";
        if (!writeAudioHoleClip(lateAudio, -1.0, 1.5)) { return failed("video stream: writing the late-audio clip failed"); }
        VideoStream l(lateAudio, (std::size_t)64 * 48 * 4 * 4, 1);
        while (l.state() == VideoStream::State::Opening && secondsSince(t0) < 30.0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (l.state() != VideoStream::State::Ready) { return failed("video stream: the late-audio clip did not open"); }
        for (int k = 0; k <= 50; ++k) {
            VideoRequest lr;
            lr.u = k / 25.0; lr.rate = 1.0f; lr.offline = true;
            l.request(lr);
            VideoStream::FrameView lv;
            if (!l.waitForFrame(lr.u, 5.0) || !l.frameAt(lr.u, lv)) {
                std::fprintf(stderr, "video stream: the late-audio clip stalled at u=%.2f\n", lr.u);
                return failed("video stream: an offline render must not wait forever for audio the read-ahead cannot reach");
            }
        }
        std::fprintf(stderr, "gl_smoke OK: VideoStream opens, reports its info, fails cleanly, and serves the exact frame + audio\n");
    }
    return true;
}

// --- Scenario: VideoStream reverse through awkward files ---
// Offline reverse must stay exact, and live reverse must not spin, whatever the file does:
//  (a) a first keyframe interval longer than a stretch's ring: offline, the ring evicts the stretch's
//      lower frames, so the stretch covers only down to its oldest -- claiming the lap start stalled a
//      loop-off render and, looping, served the previous lap's frames;
//  (b) an FLV with B-frames: its first frame comes a frame in (times count from it), a seek just below
//      a keyframe lands ON it (an empty stretch, planned again forever), and a seek into its last
//      frames finds nothing (the loop seam raced through laps with the picture frozen);
//  (c) paused in reverse, the picture settles on the playhead's frame and decoding stops.
// A frame's time names it, so the checks read FrameView::t.
static bool openStream(VideoStream& s) {
    const auto t0 = std::chrono::steady_clock::now();
    while (s.state() == VideoStream::State::Opening && secondsSince(t0) < 10.0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return s.state() == VideoStream::State::Ready;
}

// The frame for u in a writeIndexedClip of `n` frames: the greatest frame time i/kIdxFps <= u.
static double indexedFrameFor(double u, double duration, bool loop, int n) {
    const double lap = loop ? duration * std::floor(u / duration) : 0.0;
    return lap + std::min(std::floor((u - lap) * kIdxFps + 1e-6), n - 1.0) / kIdxFps;
}

// Drive `s` offline in reverse the way the node does, from u0 down to u1 at kIdxFps, checking every
// frame is the one for the playhead.
static bool reverseExact(VideoStream& s, double u0, double u1, bool loop, int n, const char* what) {
    const double D = s.info().duration;
    for (double u = u0; u >= u1; u -= 1.0 / kIdxFps) {
        VideoRequest r;
        r.u = u; r.rate = -1.0f; r.offline = true; r.loop = loop;
        if (!loop) { r.lapLo = 0.0; r.lapHi = D; }
        s.request(r);
        VideoStream::FrameView fv;
        const double want = indexedFrameFor(u, D, loop, n);
        if (!s.waitForFrame(u, 5.0) || !s.frameAt(u, fv) || std::fabs(fv.t - want) > 1e-4) {
            std::fprintf(stderr, "%s: at u=%.3f showed %.4f, expected %.4f\n", what, u, fv.t, want);
            return false;
        }
    }
    return true;
}

static bool scenario_video_stream_reverse_files() {
    {
        const std::string gop = "build/_rev_long_gop.mp4", flv = "build/_rev_bframes.flv", avi = "build/_rev_untimed_tail.avi";
        if (!writeIndexedClip(gop, 60, 60) || !writeIndexedClip(avi, 26, 25)) {
            return failed("reverse files: could not write the clips");
        }
        const bool haveFlv = writeIndexedClip(flv, 100, 25);   // FLV carries H.264, not VideoEncoder's MPEG-4 fallback
        // (a) 8 buffers -- a ring of 4 -- against a first keyframe interval of 60 frames.
        const std::size_t eight = (std::size_t)kIdxW * kIdxH * 4 * 8;
        {
            VideoStream s(gop, eight);
            if (!openStream(s) || !reverseExact(s, 2.30, 0.0, false, 60, "long keyframe interval, loop off")) {
                return failed("reverse files: offline reverse through a long first keyframe interval (loop off)");
            }
        }
        {
            VideoStream s(gop, eight);
            if (!openStream(s) || !reverseExact(s, 2.30, -1.0, true, 60, "long keyframe interval, loop on")) {
                return failed("reverse files: offline reverse through a long first keyframe interval (loop on)");
            }
        }
        VideoStream::FrameView fv;
        // Live reverse from u0 for `seconds` of wall time at -1x, the way the node drives it; the playhead
        // ends in `u`. How many stretches it decoded tells whether it spun.
        auto liveReverse = [&](VideoStream& s, double u0, double seconds, double& u) {
            u = u0;
            const auto t0 = std::chrono::steady_clock::now();
            auto last = t0;
            while (secondsSince(t0) < seconds) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                const auto now = std::chrono::steady_clock::now();
                u -= std::chrono::duration<double>(now - last).count();
                last = now;
                VideoRequest r;
                r.u = u; r.rate = -1.0f; r.loop = true;
                s.request(r);
                s.frameAt(u, fv);
            }
            return (unsigned long long)s.reverseStretches();
        };
        unsigned long long live = 0;
        if (!haveFlv) {
            std::fprintf(stderr, "gl_smoke SKIP: reverse across an FLV's loop seam (no H.264 encoder)\n");
        } else {
            // (b) Offline across the FLV's loop seam...
            {
                VideoStream s(flv);
                if (!openStream(s) || !reverseExact(s, 3.90, -1.0, true, 100, "FLV")) {
                    return failed("reverse files: offline reverse across an FLV's loop seam");
                }
            }
            // ...then live: across the seam in 2.5 s, a handful of stretches (a spinning one decodes thousands).
            VideoStream s(flv);
            if (!openStream(s)) { return failed("reverse files: the FLV did not open"); }
            const double D = s.info().duration;
            double u = 0.0;
            live = liveReverse(s, 1.2, 2.5, u);
            if (live > 40 || !(std::fabs(fv.t - u) < 0.75)) {
                std::fprintf(stderr, "reverse files: live: %llu stretches; u=%.3f showing %.3f\n", live, u, fv.t);
                return failed("reverse files: live reverse over an FLV must not spin, and must follow the playhead across the seam");
            }
            // (c) Paused: the picture settles on the playhead's frame, and decoding stops.
            VideoRequest paused;
            paused.u = u; paused.rate = 0.0f; paused.loop = true;
            s.request(paused);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const std::uint64_t settled = s.reverseStretches();
            for (int i = 0; i < 30; ++i) {
                s.request(paused);
                s.frameAt(u, fv);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            if (std::fabs(fv.t - indexedFrameFor(u, D, true, 100)) > 1e-4 || s.reverseStretches() != settled) {
                std::fprintf(stderr, "reverse files: paused at u=%.3f showing %.4f; %llu stretches after settling, %llu now\n",
                             u, fv.t, (unsigned long long)settled, (unsigned long long)s.reverseStretches());
                return failed("reverse files: paused in reverse, the picture must settle on the playhead's frame and decoding stop");
            }
        }
        // (d) An offline render straight after live reverse: the live stretches kept every 15th frame of the
        // long keyframe interval, so offline starts a fresh, whole run -- the first frame included.
        {
            VideoStream g(gop, eight);
            if (!openStream(g)) { return failed("reverse files: the long clip did not open"); }
            double gu = 2.30;
            for (int i = 0; i < 20; ++i) {
                gu -= 0.016;
                VideoRequest gr;
                gr.u = gu; gr.rate = -1.0f; gr.loop = true;
                g.request(gr);
                g.frameAt(gu, fv);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            if (!reverseExact(g, gu - 1.0 / kIdxFps, gu - 1.0, true, 60, "offline after live reverse")) {
                return failed("reverse files: an offline render straight after live reverse must be exact");
            }
        }
        // (e) An AVI whose last keyframe is its last frame: a seek there yields only a frame with no timestamp
        // and nothing before it, which cannot be placed and is skipped, so the seek backs off. Placed at the
        // seek's target instead, every stretch would cover just that frame and be planned again forever.
        unsigned long long tail = 0;
        {
            VideoStream a(avi);
            if (!openStream(a)) { return failed("reverse files: the AVI did not open"); }
            double au = 0.0;
            tail = liveReverse(a, a.info().duration - 0.02, 1.5, au);
            if (tail > 40) {
                std::fprintf(stderr, "reverse files: live reverse from the AVI's last frame: %llu stretches in 1.5 s\n", tail);
                return failed("reverse files: live reverse from an AVI's untimed last frame must not spin");
            }
        }
        std::fprintf(stderr, "gl_smoke OK: reverse stays exact through a long first keyframe interval%s, and offline straight "
                     "after live; live it does not spin (%llu stretches in 2.5 s; %llu from an AVI's untimed last frame); "
                     "paused it settles\n", haveFlv ? " and across an FLV's seam" : "", live, tail);
    }
    return true;
}

// --- Scenario: VideoStream -- toggling loop ---
// Loop is a live control, and the worker must follow it wherever the decoder happens to be:
//  (a) loop off with the playhead a lap ahead of the decoder (a hitch, or a decoder slower than the
//      playhead) pins the playhead's lap: the worker must move there -- it used to wait at the end of the
//      decoder's lap for ever, and offline report the earlier lap's frames ready;
//  (b) loop off and back on near the end of the lap: the next lap's frames, decoded ahead, stay -- they
//      were recycled, and the worker went on filling from past them, leaving a hole (loop goes off only
//      once the next lap's first frame is ready, so a slow machine cannot pass it without the wrap);
//  (c) the same at the start of the lap in reverse: the frames below the lap start are recycled, so what
//      reverse covered must shrink with them -- or it waits for a playhead that has nothing to show.
static bool scenario_video_stream_loop_toggles() {
    {
        const std::string path = "build/_loop_toggles.mp4";      // 4 s, one keyframe
        if (!writeIndexedClip(path, 100, 100)) { return failed("loop toggles: could not write the clip"); }
        VideoStream::FrameView fv;
        auto drive = [&fv](VideoStream& s, VideoRequest r, double seconds) {   // live, the playhead held
            const auto t0 = std::chrono::steady_clock::now();
            while (secondsSince(t0) < seconds) {
                s.request(r);
                s.frameAt(r.u, fv);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        };
        auto exactFrom = [&fv](VideoStream& s, VideoRequest r, int frames, double D, const char* what) {
            r.offline = true;
            for (int k = 0; k < frames; ++k, r.u += r.rate / kIdxFps) {
                s.request(r);
                const double want = indexedFrameFor(r.u, D, true, 100);
                if (!s.waitForFrame(r.u, 5.0) || !s.frameAt(r.u, fv) || std::fabs(fv.t - want) > 1e-4) {
                    std::fprintf(stderr, "%s: at u=%.3f showed %.4f, expected %.4f\n", what, r.u, fv.t, want);
                    return false;
                }
            }
            return true;
        };
        {   // (a)
            VideoStream s(path);
            if (!openStream(s)) { return failed("loop toggles: the clip did not open"); }
            const double D = s.info().duration;
            VideoRequest r;
            r.u = 0.0; r.rate = 1.0f;
            drive(s, r, 0.3);                                     // the pool fills; the decoder waits in lap 0
            r.u = D + 0.30; r.loop = false; r.lapLo = D; r.lapHi = 2.0 * D;
            if (!exactFrom(s, r, 10, D, "loop off, a lap behind")) {
                return failed("loop toggles: loop off with the decoder a lap behind must move it into the playhead's lap");
            }
        }
        {   // (b)
            VideoStream s(path);
            if (!openStream(s)) { return failed("loop toggles: the clip did not open"); }
            const double D = s.info().duration;
            VideoRequest r;
            r.u = 3.0; r.rate = 1.0f;
            const auto t0 = std::chrono::steady_clock::now();
            while (!s.frameReadyFor(D + 0.02) && secondsSince(t0) < 5.0) drive(s, r, 0.05);   // through the wrap
            if (!s.frameReadyFor(D + 0.02)) { return failed("loop toggles: the worker never decoded through the wrap"); }
            const std::uint64_t seeks = s.seeks();
            r.u = 3.5; r.loop = false; r.lapLo = 0.0; r.lapHi = D;
            drive(s, r, 0.2);
            r.u = D + 0.02; r.loop = true; r.lapLo = -std::numeric_limits<double>::infinity();
            r.lapHi = std::numeric_limits<double>::infinity();
            if (!exactFrom(s, r, 10, D, "loop off and on at the lap's end") || s.seeks() != seeks) {
                return failed("loop toggles: loop off and back on must keep the next lap's frames, not decode them again");
            }
        }
        {   // (c)
            VideoStream s(path);
            if (!openStream(s)) { return failed("loop toggles: the clip did not open"); }
            const double D = s.info().duration;
            VideoRequest r;
            r.u = D + 0.6; r.rate = -1.0f;
            drive(s, r, 0.3);                                     // covered down into lap 0
            r.u = D; r.loop = false; r.lapLo = D; r.lapHi = 2.0 * D;
            drive(s, r, 0.2);                                     // loop off: lap 0's frames are recycled
            r.loop = true; r.lapLo = -std::numeric_limits<double>::infinity();
            r.lapHi = std::numeric_limits<double>::infinity();
            const auto t0 = std::chrono::steady_clock::now();
            auto last = t0;
            while (secondsSince(t0) < 0.8) {                      // on again, playing on down into lap 0
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                const auto now = std::chrono::steady_clock::now();
                r.u -= std::chrono::duration<double>(now - last).count();
                last = now;
                s.request(r);
                s.frameAt(r.u, fv);
            }
            if (!(r.u < D - 0.5) || std::fabs(fv.t - r.u) > 0.3) {
                std::fprintf(stderr, "loop toggles: reverse at u=%.3f shows %.3f\n", r.u, fv.t);
                return failed("loop toggles: after loop off and on at the lap's start, reverse must go on into the lap below");
            }
        }
        std::fprintf(stderr, "gl_smoke OK: loop off follows a playhead a lap ahead; loop off and on keeps the next lap and "
                     "reverse's lap below\n");
    }
    return true;
}

// --- Scenario: VideoStream -- offline readiness is exact; a stream can go at any time ---
// Offline, readiness must mean the frame for the playhead is held: the frame decoded at or before it, with
// none decoded between. The node asks for its guess at the next frame's playhead ahead of time, so nothing
// recycled or planned for the guess may pass for the real playhead's frame:
//  (a) an automated rate moves the real playhead off the guess: recycling for the guess released the real
//      one's frame, and the older frame on screen still counted as ready (the frames the guess passed over
//      now stay, so a rate change costs no seek -- unless the guess lies beyond every frame held: then they
//      make room for it, and the real playhead's frame is decoded again); a jump ahead must release the
//      frames it passed, or a full pool leaves the worker nowhere to decode the frame it jumped to; and a
//      playhead that stops after a guess far ahead -- whose catch-up restarted the run past the frame on
//      screen -- gets that frame decoded again (readiness waited for ever);
//  (b) reverse, then forward (and back): the first frame after a flip repeated the last one before it;
//  (c) a loop-off render that runs into the end of the clip (or, in reverse, its start) holds that frame;
//  (d) reverse through an MPEG-TS whose keyframes are 3 s apart, where a seek lands a keyframe late and
//      must back off further than 1 s;
//  (e) a stream destroyed -- or retired -- while it opens goes quietly: stopping the probe part-way used to
//      leave the pixel format unknown, and building the colour converter for it aborted the process.

// Render offline the way the node does: advance the playhead, request it, wait for its frame and show it at
// once, then request the guess at the next playhead -- at this frame's rate -- and wait, as the renderer's gate
// does, until the guess is ready. Readiness must also hold while a request stands (the renderer does its own
// work between its gate and the node's evaluate()), so look again a moment later -- and on a render's first
// frame, which follows a jump or a flip that the worker answers by planning afresh, before showing it too.
template <class RateAt>
static bool renderExact(VideoStream& s, double u0, int frames, bool loop, int n, RateAt rateAt, const char* what) {
    const double D = s.info().duration, dt = 1.0 / kIdxFps;
    VideoPlayhead ph;
    ph.u = u0;
    for (int k = 0; k < frames; ++k) {
        const double rate = rateAt(k);
        if (k > 0) ph = videoAdvance(ph, true, rate, loop, dt, D);
        VideoRequest r;
        r.u = ph.u; r.rate = (float)rate; r.offline = true; r.loop = loop;
        if (!loop) { r.lapLo = ph.lapLo; r.lapHi = ph.lapLo + D; }
        s.request(r);
        VideoStream::FrameView fv;
        const double want = indexedFrameFor(ph.u, D, loop, n);
        bool ok = s.waitForFrame(ph.u, 5.0);
        if (k == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ok = ok && s.frameReadyFor(ph.u);
        }
        ok = ok && s.frameAt(ph.u, fv) && std::fabs(fv.t - want) <= 1e-4;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool still = s.frameReadyFor(ph.u);
        ok = ok && still && s.frameAt(ph.u, fv) && std::fabs(fv.t - want) <= 1e-4;
        if (!ok) {
            std::fprintf(stderr, "%s: frame %d, u=%.4f at %+.1fx: showed %.4f (ready a moment later: %d), expected %.4f\n",
                         what, k, ph.u, rate, fv.t, (int)still, want);
            return false;
        }
        r.u = videoAdvance(ph, true, rate, loop, dt, D).u;
        s.request(r);
        if (!s.waitForFrame(r.u, 5.0)) {
            std::fprintf(stderr, "%s: frame %d: the guess u=%.4f never became ready\n", what, k, r.u);
            return false;
        }
    }
    return true;
}

static bool scenario_video_stream_exactness() {
    {
        const std::string clip = "build/_exact_g25.mp4", quiet = "build/_exact_g25_quiet.mp4", ts = "build/_exact_keys3s.ts";
        if (!writeIndexedClip(clip, 150, 25) || !writeIndexedClip(quiet, 150, 25, kIdxW, kIdxH, false) ||
            !writeIndexedClip(ts, 150, 75)) {
            return failed("exactness: could not write the clips");
        }
        auto twoOne = [](int k) { return k % 2 ? 1.0 : 2.0; };
        auto flipAt = [](double first) { return [first](int k) { return k < 6 ? first : -first; }; };
        const std::size_t four = (std::size_t)kIdxW * kIdxH * 4 * 4;
        for (const std::string& path : {clip, quiet}) {
            for (const std::size_t pool : {four, kVideoPoolBytes}) {   // a pool always full, and one never full
                VideoStream s(path, pool);
                if (!openStream(s)) { return failed("exactness: a clip did not open"); }
                const double D = s.info().duration;
                // (a)
                if (!renderExact(s, 0.0, 40, true, 150, twoOne, "alternating 2x and 1x")) {
                    return failed("exactness: an offline render whose rate changes between frames must show the frame for each playhead");
                }
                if (s.seeks() != 0) {
                    std::fprintf(stderr, "exactness: %llu seeks\n", (unsigned long long)s.seeks());
                    return failed("exactness: a rate change must not cost a seek");
                }
                if (!renderExact(s, 4.0, 40, true, 150, [&](int k) { return -twoOne(k); }, "alternating -2x and -1x")) {
                    return failed("exactness: an offline render whose rate changes between frames must show the frame for each playhead");
                }
                if (!renderExact(s, 0.2, 5, true, 150, [](int) { return 1.0; }, "before a jump") ||
                    !s.waitForFrame(0.48, 5.0) ||                 // the four-frame pool is full: 0.36 to 0.48
                    !renderExact(s, 3.5, 5, true, 150, [](int) { return 1.0; }, "after a jump")) {
                    return failed("exactness: an offline render must go on exactly after a jump ahead");
                }
                if (!renderExact(s, 1.0, 20, true, 150, [](int k) { return k % 2 ? 1.0 : 4.0; }, "alternating 4x and 1x") ||
                    !renderExact(s, 5.0, 20, true, 150, [](int k) { return k % 2 ? -1.0 : -4.0; }, "alternating -4x and -1x")) {
                    return failed("exactness: a guess beyond the frames held must not cost the real playhead its frame");
                }
                if (!renderExact(s, 1.0, 6, true, 150, [](int k) { return k < 3 ? 8.0 : 0.0; }, "8x, then paused")) {
                    return failed("exactness: a render that stops after a guess far ahead must go on");
                }
                // (b)
                if (!renderExact(s, 2.0, 12, true, 150, flipAt(-1.0), "reverse, then forward") ||
                    !renderExact(s, 3.0, 12, true, 150, flipAt(1.0), "forward, then reverse")) {
                    return failed("exactness: the first frame after a direction flip must be the one for its playhead");
                }
                // (c)
                if (!renderExact(s, D - 0.4, 20, false, 150, [](int) { return 1.0; }, "loop off into the end") ||
                    !renderExact(s, 0.4, 20, false, 150, [](int) { return -1.0; }, "loop off into the start")) {
                    return failed("exactness: a loop-off render must hold the clip's last (or, in reverse, first) frame");
                }
            }
        }
        {   // (d)
            VideoStream s(ts);
            if (!openStream(s) || !reverseExact(s, 5.9, 0.0, false, 150, "MPEG-TS")) {
                return failed("exactness: offline reverse through an MPEG-TS must be exact");
            }
        }
        // (e) A crash here takes gl_smoke down with it.
        for (int us = 0; us <= 3000; us += 100) {
            { VideoStream s("tests/assets/test.mp4"); std::this_thread::sleep_for(std::chrono::microseconds(us)); }
            auto r = std::make_unique<VideoStream>("tests/assets/test.mp4");
            std::this_thread::sleep_for(std::chrono::microseconds(us));
            VideoStream::retire(std::move(r));
        }
        std::fprintf(stderr, "gl_smoke OK: offline readiness is exact through rate changes and direction flips, into a "
                     "loop-off end, and in reverse through an MPEG-TS; a stream destroyed or retired while it opens goes quietly\n");
    }
    return true;
}

```

- [ ] **Step 3: Register them in `kScenarios`, just above `scenario_video_player_decode,`**. In `tests/gl_smoke.cpp`, replace:

```cpp
    scenario_video_player_decode,
```

with:

```cpp
    scenario_video_stream_basics,
    scenario_video_stream_reverse_files,
    scenario_video_stream_loop_toggles,
    scenario_video_stream_exactness,
    scenario_video_player_decode,
```

- [ ] **Step 4: Build to verify it fails**

```bash
cmake --build build --target gl_smoke -j8
```

Expected: the build FAILS, with an error mentioning `gfx/VideoStream.h' file not found`.

- [ ] **Step 5: Write the header** — `src/gfx/VideoStream.h` (complete file)

```cpp
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "core/TimedAudio.h"
#include "core/VideoPlan.h"
#include "gfx/VideoDecoder.h"

namespace oss {

// What the Video Player node asks its stream for each frame. Times are unwrapped (core/VideoPlan.h).
struct VideoRequest {
    double u       = 0.0;    // the playhead
    float  rate    = 0.0f;   // signed speed; 0 while paused (keeps the last direction)
    bool   loop    = true;
    bool   offline = false;  // an offline render: reverse stretches keep consecutive frames
    double lapLo   = -std::numeric_limits<double>::infinity();   // loop off: the playhead's lap
    double lapHi   =  std::numeric_limits<double>::infinity();
};

// A background decoder for one video file. A worker thread opens the file, then keeps a fixed pool
// of RGBA frames decoded ahead of the requested playhead -- or, in reverse, in keyframe-to-frame
// stretches -- plus the audio around it. The graph thread only posts requests and picks up finished
// frames, so it never waits on FFmpeg during live playback. GL-free: the node uploads the pixels.
//
// Threading: one mutex guards the request, the pool, the ready queue, the audio and the status. The
// worker holds it only for bookkeeping, never while decoding or converting. The frame frameAt()
// returns is "checked out": the worker will not reuse its buffer until the next frameAt().
class VideoStream {
public:
    enum class State { Opening, Ready, Failed };
    struct Info {
        int    width = 0, height = 0;
        double duration = 0.0;            // seconds; 0 = unknown
        double frameDur = 1.0 / 30.0;     // nominal seconds per frame
        bool   hasAudio = false;
    };
    // The frame to display: tightly packed RGBA8 rows, TOP row first, width * 4 bytes apart.
    struct FrameView {
        const std::uint8_t* rgba   = nullptr;
        double              t      = 0.0;   // unwrapped time
        std::uint64_t       serial = 0;     // changes whenever a different frame is shown
    };

    // `readAheadBytes` caps the decoder's read-ahead (tests set it small; see VideoDecoder::pumpAudio).
    explicit VideoStream(std::string path, std::size_t poolBytes = kVideoPoolBytes,
                         std::size_t readAheadBytes = VideoDecoder::kMaxQueuedBytes);
    ~VideoStream();                        // stops and joins the worker

    // Destroy `s` on a background thread and return at once: tearing a stream down -- joining its worker,
    // freeing its frames, closing its decoder -- takes 0.1-0.5 s at 4K, too long for the UI thread. Its
    // worker is told to stop straight away; streams go in the order retired, the last before the process exits.
    // Never throws (destructors call it): should that thread fail to start, `s` is destroyed here instead.
    // Not during static destruction: the reaper is itself a function-local static.
    static void retire(std::unique_ptr<VideoStream> s);
    VideoStream(const VideoStream&) = delete;
    VideoStream& operator=(const VideoStream&) = delete;

    State       state() const;
    std::string error() const;
    Info        info() const;             // valid once Ready

    void request(const VideoRequest& r);

    // The frame for u -- the greatest decoded time <= u -- or, when none is decoded yet, whatever is
    // already on screen (or the earliest ready frame if nothing is). False when there is nothing to
    // show. The pointer stays valid until the next frameAt() call or destruction.
    bool frameAt(double u, FrameView& out);

    // Offline: the frame for u is held -- the frame decoded at or before u with none decoded between it
    // and u -- and, playing forward, no more audio can arrive for times up to u. True once Failed.
    bool frameReadyFor(double u) const;
    // Wait until frameReadyFor(u), at most timeoutSeconds; its answer (false on a timeout, or once the
    // stream is being destroyed).
    bool waitForFrame(double u, double timeoutSeconds);

    // n output samples spanning source time [u0, u1] (see TimedAudio::sample).
    void readAudio(double u0, double u1, float* out, int n) const;

    // Diagnostics: seeks made and reverse stretches decoded so far. Both grow with the ground the
    // playhead covers; a step that achieved nothing and was planned again would spin the worker, and
    // show here.
    std::uint64_t seeks() const            { return seeks_.load(); }
    std::uint64_t reverseStretches() const { return reverseStretches_.load(); }

private:
    // A queued frame. `until` is the time of the next frame decoded after it: the frame for u covers [t, until).
    struct Slot { double t = 0.0; std::uint64_t serial = 0; int buf = -1; double until = 0.0; };

    // Worker thread.
    void run();
    void step();
    void waitForWork();
    bool peekNext();
    void takeAudio();
    void pumpAudio(double u, bool offline);
    void seekDecoder(double t);
    bool seekLanding(double end, double slack);
    void seekTo(double target);
    void catchUp(double target);
    void fill();
    void wrap();
    void reverseStretch(double to, bool fresh, bool offline);
    bool publish(DecodedFrame& f, double t, bool runStart);
    bool publishSlot(DecodedFrame& f, double t, double until);
    double nextFrameTime() const;
    bool directionChanged() const;
    void setFailed(const std::string& msg);

    // Under m_.
    int  readyFrameLocked(double u) const;   // index in ready_ of the frame for u, or -1
    bool holdsLocked(double u) const;
    bool readyLocked(double u) const;
    int  acquireLocked();
    void releaseLocked(int buf);
    void insertReadyLocked(const Slot& s);
    void flushReadyLocked();
    void recycleLocked(const VideoRequest& r, int dir);
    void setRunHiLocked();

    const std::string path_;
    const std::size_t poolBytes_;
    const std::size_t readAheadBytes_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> seeks_{0}, reverseStretches_{0};

    // --- guarded by m_ ---
    mutable std::mutex      m_;
    std::condition_variable cv_;
    State         state_ = State::Opening;
    std::string   error_;
    Info          info_;
    VideoRequest  req_;
    int           dir_ = 1;                // direction of the latest request (a pause keeps it)
    int           planDir_ = 1;            // the direction the worker last planned for...
    bool          planOffline_ = false;    // ...and whether for an offline render
    std::uint64_t reqSerial_ = 0, freedSerial_ = 0;
    std::vector<std::unique_ptr<std::uint8_t[]>> pool_;
    std::vector<int>  free_;
    std::vector<Slot> ready_;              // ascending t
    Slot          shown_;                  // checked out by frameAt() (buf -1: none)
    std::uint64_t nextSerial_ = 1;
    TimedAudio    audio_;
    double        audioSettledU_ = -std::numeric_limits<double>::infinity();
    double        audioLapStart_ = 0.0;    // audio for earlier laps is complete
    bool          runValid_ = false;       // consecutive frames decided over [runLo_, runHi_)
    bool          runOffline_ = false;     // ...by offline stretches (reverse: live ones keep every stride-th)
    double        runLo_ = 0.0, runHi_ = 0.0;

    // --- worker only ---
    VideoDecoder  dec_;
    Info          winfo_;                  // copy of info_ (constant once Ready)
    DecodedFrame  next_;                   // decoded, not yet converted: one frame of lookahead
    double        lapOffset_ = 0.0;        // unwrapped time of the decoder's lap start
    double        lastT_ = 0.0;            // unwrapped time of the last frame taken from the decoder
    bool          eof_ = false;
    bool          coverValid_ = false;     // reverse: this run's stretches reach down to coverLo_...
    double        coverLo_ = 0.0;
    double        coverHi_ = 0.0;          // ...from here, the end of the run's first stretch
    double        lastStretchSeconds_ = 0.0;   // wall time the last reverse stretch took to decode
    bool          seekPinned_ = false;     // the last seek could not land at or before...
    double        pinnedFrom_ = 0.0;       // ...this target (it precedes the file's first frame)
    double        noSeekBelow_ = -std::numeric_limits<double>::infinity();   // see seekTo()
    bool          audioChunkOpen_ = false;
    double        audioClipHi_ = std::numeric_limits<double>::infinity();
    std::uint64_t seenReq_ = 0, seenFreed_ = 0;
    std::vector<float> audioTmp_;

    std::thread thread_;                   // LAST: started once everything above exists
};

} // namespace oss
```

- [ ] **Step 6: Write the implementation** — `src/gfx/VideoStream.cpp` (complete file)

```cpp
#include "gfx/VideoStream.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <new>
#include <stdexcept>

namespace oss {

namespace { constexpr double kInf = std::numeric_limits<double>::infinity(); }

VideoStream::VideoStream(std::string path, std::size_t poolBytes, std::size_t readAheadBytes)
    : path_(std::move(path)), poolBytes_(poolBytes), readAheadBytes_(readAheadBytes), audio_(VideoDecoder::kOutRate) {
    thread_ = std::thread([this] { run(); });
}

VideoStream::~VideoStream() {
    { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();   // bounded: the worker checks stop_ between frames,
}                                               // and FFmpeg's I/O polls it while blocked

namespace {

// Destroys retired streams one after another on its own thread; joined at exit.
class Reaper {
public:
    ~Reaper() {
        { std::lock_guard<std::mutex> lk(m_); done_ = true; }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    // Never throws: it runs in destructors. With no thread (or no room to queue), the stream goes here.
    void add(std::unique_ptr<VideoStream> s) {
        std::unique_lock<std::mutex> lk(m_);
        try {
            if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
            queue_.push_back(std::move(s));
        } catch (...) {
            lk.unlock();
            s.reset();
            return;
        }
        cv_.notify_all();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lk(m_);
        for (;;) {
            cv_.wait(lk, [this] { return done_ || !queue_.empty(); });
            if (queue_.empty()) return;                // done, and nothing left to destroy
            std::unique_ptr<VideoStream> s = std::move(queue_.front());
            queue_.pop_front();
            lk.unlock();
            s.reset();
            lk.lock();
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<VideoStream>> queue_;
    bool done_ = false;
    std::thread thread_;
};

} // namespace

void VideoStream::retire(std::unique_ptr<VideoStream> s) {
    if (!s) return;
    { std::lock_guard<std::mutex> lk(s->m_); s->stop_ = true; }   // stop decoding now, not when its turn comes
    s->cv_.notify_all();
    try {
        static Reaper reaper;                      // (building it can allocate)
        reaper.add(std::move(s));
    } catch (...) {
        s.reset();
    }
}

VideoStream::State VideoStream::state() const { std::lock_guard<std::mutex> lk(m_); return state_; }
std::string VideoStream::error() const        { std::lock_guard<std::mutex> lk(m_); return error_; }
VideoStream::Info VideoStream::info() const   { std::lock_guard<std::mutex> lk(m_); return info_; }

void VideoStream::request(const VideoRequest& r) {
    {
        std::lock_guard<std::mutex> lk(m_);
        req_ = r;
        if (r.rate > 0.0f)      dir_ = 1;
        else if (r.rate < 0.0f) dir_ = -1;        // 0 (paused) keeps the direction
        ++reqSerial_;
    }
    cv_.notify_all();
}

bool VideoStream::frameAt(double u, FrameView& out) {
    bool freed = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (state_ != State::Ready) return false;
        int idx = readyFrameLocked(u);
        if (idx < 0 && shown_.buf < 0 && !ready_.empty()) idx = 0;   // nothing up yet: the nearest
        const bool shownFits = shown_.buf >= 0 && shown_.t <= u + kVideoTimeEps;
        if (idx >= 0 && (!shownFits || ready_[(std::size_t)idx].t > shown_.t)) {
            if (shown_.buf >= 0) { releaseLocked(shown_.buf); freed = true; }
            shown_ = ready_[(std::size_t)idx];
            ready_.erase(ready_.begin() + idx);
        }
        if (shown_.buf < 0) return false;
        // Frames the playhead has passed can never be shown again.
        for (std::size_t i = 0; i < ready_.size();) {
            const bool passed = dir_ >= 0 ? ready_[i].t < shown_.t : ready_[i].t > shown_.t;
            if (passed) { releaseLocked(ready_[i].buf); ready_.erase(ready_.begin() + (std::ptrdiff_t)i); freed = true; }
            else ++i;
        }
        out.rgba   = pool_[(std::size_t)shown_.buf].get();
        out.t      = shown_.t;
        out.serial = shown_.serial;
    }
    if (freed) cv_.notify_all();
    return true;
}

bool VideoStream::frameReadyFor(double u) const {
    std::lock_guard<std::mutex> lk(m_);
    return readyLocked(u);
}

bool VideoStream::waitForFrame(double u, double timeoutSeconds) {
    std::unique_lock<std::mutex> lk(m_);
    return cv_.wait_for(lk, std::chrono::duration<double>(timeoutSeconds),
                        [&] { return stop_.load() || readyLocked(u); }) && readyLocked(u);
}

void VideoStream::readAudio(double u0, double u1, float* out, int n) const {
    std::lock_guard<std::mutex> lk(m_);
    audio_.sample(u0, u1, out, n);
}

// --- under m_ -------------------------------------------------------------------------------

int VideoStream::readyFrameLocked(double u) const {
    return videoSelectFrame((int)ready_.size(), u, [this](int i) { return ready_[(std::size_t)i].t; });
}

bool VideoStream::readyLocked(double u) const {
    if (state_ == State::Failed) return true;                 // nothing more will come
    if (state_ != State::Ready || !runValid_) return false;
    if (dir_ != planDir_ || (dir_ < 0 && req_.offline && !planOffline_)) return false;   // step() flushes it all first
    if (dir_ < 0 && !runOffline_) return false;               // live stretches skipped frames
    if (u < runLo_ - kVideoTimeEps || u >= runHi_ - kVideoTimeEps) return false;
    if (!holdsLocked(u)) return false;
    if (dir_ < 0 || !info_.hasAudio) return true;             // reverse: the stretch brought its audio
    return u < audioLapStart_ - kVideoTimeEps || audioSettledU_ >= u - kVideoTimeEps;
}

int VideoStream::acquireLocked() {
    if (free_.empty()) return -1;
    const int b = free_.back();
    free_.pop_back();
    return b;
}

void VideoStream::releaseLocked(int buf) {
    free_.push_back(buf);
    ++freedSerial_;
}

void VideoStream::insertReadyLocked(const Slot& s) {
    auto at = std::upper_bound(ready_.begin(), ready_.end(), s.t,
                               [](double t, const Slot& o) { return t < o.t; });
    ready_.insert(at, s);
}

void VideoStream::flushReadyLocked() {
    for (const Slot& s : ready_) releaseLocked(s.buf);
    ready_.clear();
}

// Release queued frames that can never be shown for this request: in forward play those older than
// the frame for the target, in reverse those after it (the playhead has passed them), and with loop
// off those before the playhead's lap. Frames past its end stay: the loop-off playhead stops just
// short of them, and they are the next ones to show if loop comes back on. Offline, the request is
// the node's guess at the next frame's playhead, which an automated rate can move: while the guess's frame
// is held (so nothing has to make room for it), the frames between the one on screen and the guess stay
// too -- one may be the frame the real playhead needs, and a frame given up is a seek to decode it again.
// Only while the frame on screen is on the way to the guess, though: after a jump or a flip it lies
// beyond it, and so does nothing to keep.
void VideoStream::recycleLocked(const VideoRequest& r, int dir) {
    double frameT = (shown_.buf >= 0 && shown_.t <= r.u + kVideoTimeEps) ? shown_.t : -kInf;
    const int best = readyFrameLocked(r.u);
    if (best >= 0) frameT = std::max(frameT, ready_[(std::size_t)best].t);
    double lo = frameT;                                                   // forward: frames before lo go
    double hi = std::isfinite(frameT) ? frameT : r.u + kVideoTimeEps;     // reverse: frames after hi go
    if (r.offline && shown_.buf >= 0 && holdsLocked(r.u)) {
        if (shown_.t <= lo) lo = shown_.t;
        if (shown_.t >= hi) hi = shown_.t;
    }
    for (std::size_t i = 0; i < ready_.size();) {
        const Slot& s = ready_[i];
        bool drop = dir >= 0 ? s.t < lo : s.t > hi;
        if (!r.loop && s.t < r.lapLo - kVideoTimeEps) drop = true;
        if (drop) { releaseLocked(s.buf); ready_.erase(ready_.begin() + (std::ptrdiff_t)i); }
        else ++i;
    }
}

// The exclusive end of the consecutive run of decided frames (forward): the next frame's time when
// it has been peeked, at the end of the file the lap's end (forever when the duration is unknown),
// else just past the last frame taken. Reads worker state: called by the worker only.
void VideoStream::setRunHiLocked() {
    if (next_.valid())  runHi_ = lapOffset_ + next_.t;
    else if (eof_)      runHi_ = winfo_.duration > 0.0 ? lapOffset_ + winfo_.duration : kInf;
    else                runHi_ = lastT_ + 2.0 * kVideoTimeEps;
}

// The frame held for u, exactly: a held slot with t <= u < until (the next frame decoded after it).
bool VideoStream::holdsLocked(double u) const {
    if (shown_.buf >= 0 && shown_.t <= u + kVideoTimeEps && u < shown_.until - kVideoTimeEps) return true;
    const int i = readyFrameLocked(u);
    return i >= 0 && u < ready_[(std::size_t)i].until - kVideoTimeEps;
}

// --- worker ---------------------------------------------------------------------------------

// The time of the next frame the decoder gives (the lap's end at its end): the exclusive end of the last frame taken.
double VideoStream::nextFrameTime() const {
    if (next_.valid()) return lapOffset_ + next_.t;
    if (eof_)          return winfo_.duration > 0.0 ? lapOffset_ + winfo_.duration : kInf;
    return lastT_ + 2.0 * kVideoTimeEps;
}

void VideoStream::setFailed(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(m_);
        state_ = State::Failed;
        error_ = msg;
    }
    cv_.notify_all();
}

void VideoStream::run() {
    try {
        std::string err;
        dec_.setMaxQueuedBytes(readAheadBytes_);
        if (!dec_.open(path_, err, &stop_)) { setFailed(err); return; }
        Info inf;
        inf.width    = dec_.width();
        inf.height   = dec_.height();
        inf.duration = dec_.duration();
        inf.frameDur = dec_.frameDuration();
        inf.hasAudio = dec_.hasAudio();
        const int n = videoPoolFrames(poolBytes_, inf.width, inf.height);
        std::vector<std::unique_ptr<std::uint8_t[]>> pool;
        try {
            pool.reserve((std::size_t)n);
            for (int i = 0; i < n; ++i)
                pool.emplace_back(new std::uint8_t[(std::size_t)inf.width * inf.height * 4]);
        } catch (const std::bad_alloc&) {
            setFailed("not enough memory for " + std::to_string(inf.width) + "x" +
                      std::to_string(inf.height) + " frames");
            return;
        }
        winfo_ = inf;
        lastT_ = -inf.frameDur;                                // the first frame is expected at 0
        {
            std::lock_guard<std::mutex> lk(m_);
            info_ = inf;
            pool_ = std::move(pool);
            for (int i = n - 1; i >= 0; --i) free_.push_back(i);
            state_ = State::Ready;
        }
        cv_.notify_all();
        while (!stop_) step();
    } catch (const std::exception& e) {
        setFailed(std::string("video worker: ") + e.what());
    } catch (...) {
        setFailed("video worker: unknown error");
    }
}

void VideoStream::step() {
    VideoRequest r;
    int dir = 1;
    bool changed = false, toOffline = false, lostTarget = false;
    VideoPlanInput in;
    {
        std::lock_guard<std::mutex> lk(m_);
        r = req_;
        dir = dir_;
        seenReq_ = reqSerial_;
        seenFreed_ = freedSerial_;
        changed = dir != planDir_;
        // An offline render starting in reverse cannot use live stretches: they kept every stride-th frame.
        toOffline = dir < 0 && r.offline && !planOffline_;
        if (changed || toOffline) { flushReadyLocked(); runValid_ = false; }
        planDir_ = dir;
        planOffline_ = r.offline;
        recycleLocked(r, dir);
        if (!r.loop && runValid_ && runLo_ < r.lapLo) {    // what was decided before the lap is gone
            if (runHi_ <= r.lapLo) runValid_ = false;
            else                   runLo_ = r.lapLo;
        }
        if (dir >= 0) audio_.retain(r.u - kVideoAudioKeep, kInf, r.u);
        else          audio_.retain(-kInf, r.u + kVideoAudioKeep, r.u);
        in.lowest = kInf;
        if (shown_.buf >= 0) in.lowest = shown_.t;
        if (!ready_.empty()) in.lowest = std::min(in.lowest, ready_.front().t);
        in.freeBuffers = (int)free_.size();
        in.poolSize    = (int)pool_.size();
        // Offline forward, readiness can wait for ever on what only a seek back to u mends: u's frame was
        // released while frames past it are held (the planner counts what is held), or it is held but a
        // catch-up to a guess far ahead restarted the run past it. Reverse needs no such help: while the
        // guess's frame is held nothing between it and the frame on screen is released (recycleLocked), and
        // when it is not, the guess lies below what the stretches cover and gets a fresh stretch -- which
        // leaves the real playhead above it, stranded, to get another.
        if (r.offline && dir >= 0) {
            if (!holdsLocked(r.u)) {
                lostTarget = shown_.buf >= 0 && shown_.t > r.u + kVideoTimeEps;
                for (const Slot& s : ready_) lostTarget = lostTarget || s.t > r.u + kVideoTimeEps;
            } else {
                lostTarget = runValid_ && r.u < runLo_ - kVideoTimeEps;
            }
        }
    }
    if (lostTarget) in.lowest = kInf;                             // seek back to u
    if (changed || toOffline) coverValid_ = false;
    if (!r.loop && coverValid_ && coverLo_ < r.lapLo) {     // likewise what reverse had covered
        if (coverHi_ <= r.lapLo) coverValid_ = false;
        else                     coverLo_ = r.lapLo;
    }

    in.target     = r.u;
    in.dir        = dir;
    in.dirChanged = changed;
    in.loop       = r.loop;
    in.duration   = winfo_.duration;
    in.frameDur   = winfo_.frameDur;
    in.lapLo      = r.lapLo;
    in.lapHi      = r.lapHi;
    in.head       = next_.valid() ? lapOffset_ + next_.t : lastT_ + winfo_.frameDur;
    in.eof        = eof_ && !next_.valid();
    double key = 0.0;
    in.keyKnown   = dec_.nextKeyframeAfter(in.head - lapOffset_, key);
    in.nextKey    = lapOffset_ + key;
    in.lapEnd     = winfo_.duration > 0.0 ? lapOffset_ + winfo_.duration : kInf;
    if (!std::isfinite(in.lowest) && !lostTarget) in.lowest = in.head;
    in.seekPinned = seekPinned_;
    in.pinnedFrom = pinnedFrom_;
    in.noSeekBelow = noSeekBelow_;
    in.coverValid = coverValid_;
    in.coverLo    = coverLo_;
    in.coverHi    = coverHi_;
    in.lead       = r.offline ? 0.0 : std::min(2.0, std::fabs((double)r.rate) * lastStretchSeconds_);

    const VideoStep s = videoNextStep(in);
    // Keep the audio ahead of the playhead -- but not before a seek, which throws the read-ahead away.
    if (dir >= 0 && s.kind != VideoStepKind::Seek) pumpAudio(r.u, r.offline);
    switch (s.kind) {
        case VideoStepKind::Wait:    waitForWork(); break;
        case VideoStepKind::Fill:    fill(); break;
        case VideoStepKind::CatchUp: catchUp(s.to); break;
        case VideoStepKind::Seek:    seekTo(s.to); break;
        case VideoStepKind::Wrap:    wrap(); break;
        case VideoStepKind::Reverse: reverseStretch(s.to, s.fresh, r.offline); break;
    }
}

void VideoStream::waitForWork() {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, std::chrono::milliseconds(100), [this] {
        return stop_.load() || reqSerial_ != seenReq_ || freedSerial_ != seenFreed_;
    });
}

bool VideoStream::directionChanged() const {
    std::lock_guard<std::mutex> lk(m_);
    return dir_ != planDir_;
}

// Decode the next frame into next_ (without converting it) unless one is already waiting. When the
// duration is known, a frame at or past it ends the lap.
bool VideoStream::peekNext() {
    if (next_.valid()) return true;
    if (eof_) return false;
    bool ok = dec_.decodeNext(next_);
    if (ok && winfo_.duration > 0.0 && next_.t >= winfo_.duration - kVideoTimeEps) {
        next_.reset();
        ok = false;
    }
    if (!ok) eof_ = true;
    takeAudio();
    return ok;
}

// Move the decoder's pending audio into the store -- a new chunk after a seek or wrap, and wherever the
// source's audio jumps (a gap, or an overlap) -- clipped below audioClipHi_, and publish how far the
// audio is settled.
void VideoStream::takeAudio() {
    double start = 0.0;
    bool continues = false;
    while (dec_.takeAudio(audioTmp_, start, continues)) {
        std::lock_guard<std::mutex> lk(m_);
        if (!audioChunkOpen_ || !continues) { audio_.beginChunk(lapOffset_ + start); audioChunkOpen_ = true; }
        audio_.append(audioTmp_.data(), audioTmp_.size(), audioClipHi_);
    }
    const double settled = dec_.audioSettledUpTo();
    {
        std::lock_guard<std::mutex> lk(m_);
        audioSettledU_ = lapOffset_ + settled;
        audioLapStart_ = lapOffset_;
    }
    cv_.notify_all();
}

// Read the audio ahead of u. An offline render cannot go on without it: the decoder may give it up
// only then (VideoDecoder::pumpAudio).
void VideoStream::pumpAudio(double u, bool offline) {
    if (!winfo_.hasAudio) return;
    dec_.pumpAudio(u + kVideoAudioLead - lapOffset_, offline);
    takeAudio();
}

// Seek the decoder. One that cannot even get back to the start of its file can neither loop nor play in
// reverse, and every step would seek again: fail instead of spinning.
void VideoStream::seekDecoder(double t) {
    if (!dec_.seek(t) && !stop_) throw std::runtime_error("cannot seek in this file");
}

// Seek in the decoder's lap so the next frame is at or before unwrapped time `end` (+ `slack`: callers
// pass the rule their decode loop admits frames by). A seek can land late: where the index holds
// decode times (FLV, fragmented MP4, B-frames without an edit list) a seek just below a keyframe lands
// ON it; a demuxer that searches by timestamp (MPEG-TS) can overshoot by a keyframe interval; some
// (FLV, MPEG-TS) find no frame at all when asked for the last ones. So retry further back -- 1 s, 2 s,
// 4 s... -- down to the start of the file. False when even that lands after `end` or finds nothing.
bool VideoStream::seekLanding(double end, double slack) {
    const double src = end - lapOffset_;
    for (double back = 0.0;; back = back > 0.0 ? 2.0 * back : 1.0) {
        const double at = std::max(0.0, src - back);
        seekDecoder(at);
        next_.reset();
        eof_ = false;
        audioChunkOpen_ = false;
        if (peekNext() && lapOffset_ + next_.t <= end + slack) return true;
        if (at <= 0.0) return false;
    }
}

void VideoStream::seekTo(double target) {
    const double before = next_.valid() ? lapOffset_ + next_.t : lastT_ + winfo_.frameDur;   // the head
    seeks_.fetch_add(1);
    {
        // Frames after the target are stale; the newest one at or before it stays up until the
        // seek delivers a better one (recycleLocked drops the older ones).
        std::lock_guard<std::mutex> lk(m_);
        for (std::size_t i = 0; i < ready_.size();) {
            if (ready_[i].t > target + kVideoTimeEps) { releaseLocked(ready_[i].buf); ready_.erase(ready_.begin() + (std::ptrdiff_t)i); }
            else ++i;
        }
        runValid_ = false;
        audioSettledU_ = -kInf;
    }
    lapOffset_ = winfo_.duration > 0.0 ? videoLapStart(target, winfo_.duration) : 0.0;
    const bool landed = seekLanding(target, kVideoTimeEps);
    seekPinned_ = !landed;
    pinnedFrom_ = target;
    noSeekBelow_ = videoNoSeekBelow(target, before, next_.valid() ? lapOffset_ + next_.t : kInf);
    lastT_ = target;
    catchUp(target);
}

// Decode forward without converting until the next frame is past `target`, then convert only the
// frame for `target`. Live, the target chases a playhead that keeps moving meanwhile -- but only for
// kVideoCatchUpSlice: a decoder slower than the playhead (4K HEVC at 2x) would otherwise chase it
// forever and never show a frame. After the slice it shows the best frame reached and re-plans,
// which may seek ahead instead.
void VideoStream::catchUp(double target) {
    DecodedFrame held;
    double heldT = 0.0;
    const auto began = std::chrono::steady_clock::now();
    bool offline = false;
    { std::lock_guard<std::mutex> lk(m_); offline = req_.offline; }
    while (!stop_ && peekNext()) {
        if (!offline && held.valid() &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count() > kVideoCatchUpSlice)
            break;
        const double t = lapOffset_ + next_.t;
        if (t > target + kVideoTimeEps) break;
        held = std::move(next_);
        heldT = t;
        lastT_ = t;
        pumpAudio(target, offline);
        if (directionChanged()) return;
        std::lock_guard<std::mutex> lk(m_);
        if (!req_.offline && req_.u > target) target = req_.u;
    }
    if (!held.valid() && next_.valid()) {          // the target precedes the first frame: show that one
        held = std::move(next_);
        heldT = lapOffset_ + held.t;
        lastT_ = heldT;
        peekNext();
    }
    if (held.valid()) {
        publish(held, heldT, true);
    } else {
        { std::lock_guard<std::mutex> lk(m_); setRunHiLocked(); }
        cv_.notify_all();
    }
}

void VideoStream::fill() {
    if (!peekNext()) {                             // end of the lap: extend the run to its end
        { std::lock_guard<std::mutex> lk(m_); setRunHiLocked(); }
        cv_.notify_all();
        return;
    }
    DecodedFrame f = std::move(next_);
    const double t = lapOffset_ + f.t;
    lastT_ = t;
    peekNext();                                    // one frame of lookahead bounds the run exactly
    publish(f, t, false);
}

void VideoStream::wrap() {
    lapOffset_ += winfo_.duration;
    noSeekBelow_ = -kInf;                          // what a seek taught about keyframes was in the old lap
    seekDecoder(0.0);
    next_.reset();
    eof_ = false;
    audioChunkOpen_ = false;
    lastT_ = lapOffset_ - winfo_.frameDur;
    std::lock_guard<std::mutex> lk(m_);
    audioSettledU_ = -kInf;
    audioLapStart_ = lapOffset_;                   // the previous lap's audio is complete
}

// Convert `f` into a free buffer and queue it. False if no buffer is free or conversion fails.
bool VideoStream::publishSlot(DecodedFrame& f, double t, double until) {
    int b = -1;
    { std::lock_guard<std::mutex> lk(m_); b = acquireLocked(); }
    if (b < 0) return false;
    if (!dec_.convert(f, pool_[(std::size_t)b].get(), winfo_.width * 4)) {   // pool_ is fixed once Ready
        std::lock_guard<std::mutex> lk(m_);
        releaseLocked(b);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        insertReadyLocked(Slot{t, nextSerial_++, b, until});
    }
    cv_.notify_all();
    return true;
}

// Forward: queue the frame and extend the run of consecutive decided frames.
bool VideoStream::publish(DecodedFrame& f, double t, bool runStart) {
    if (!publishSlot(f, t, nextFrameTime())) return false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (runStart || !runValid_) { runValid_ = true; runLo_ = t; }
        setRunHiLocked();
    }
    cv_.notify_all();
    return true;
}

// Decode a stretch forward from its keyframe and queue the frames it keeps: live every stride-th, counted
// back from its top frame; offline a ring of the newest pool/2, every one of them. A `fresh` stretch
// ends AT `to` -- the playhead, inclusive -- and starts a new run; otherwise the stretch lies strictly
// BELOW `to`, the start of the stretch above, whose frame is already queued. The kept frames are queued
// together when the stretch is complete: decoding runs forward, so queueing them one by one would show
// the stretch's EARLIEST frame first and then play it forwards.
void VideoStream::reverseStretch(double to, bool fresh, bool offline) {
    const double D = winfo_.duration, fd = winfo_.frameDur;
    const auto began = std::chrono::steady_clock::now();
    reverseStretches_.fetch_add(1);
    if (fresh) {
        std::lock_guard<std::mutex> lk(m_);
        flushReadyLocked();
        runValid_ = false;
        coverValid_ = false;
    }
    const double end = fresh ? to : to - kVideoTimeEps;          // the latest time this stretch may hold
    lapOffset_ = D > 0.0 ? videoLapStart(end, D) : 0.0;           // just below a lap start: the lap before
    audioClipHi_ = coverValid_ ? coverLo_ : kInf;                 // never duplicate the stretch above's audio
    // Land where the loop below admits frames: a prefetch landing ON the stretch above's keyframe
    // would decode nothing, cover nothing, and be planned again forever.
    const bool landed = seekLanding(end, fresh ? kVideoTimeEps : 0.0);
    int cap = 1;
    { std::lock_guard<std::mutex> lk(m_); cap = videoStretchBudget((int)pool_.size()); }

    double lo = lapOffset_;                        // how far down this stretch covers
    std::vector<Slot> ring;                        // the kept frames, oldest first (offline: the newest `cap`)
    bool ringPending = false;                      // ring.back().until awaits the next frame's time
    auto keep = [&](DecodedFrame& f, double t, bool evict) {
        int b = -1;
        if (!evict || (int)ring.size() < cap) { std::lock_guard<std::mutex> lk(m_); b = acquireLocked(); }
        if (b < 0 && evict && !ring.empty()) { b = ring.front().buf; ring.erase(ring.begin()); }
        if (b < 0) return;                         // live: no room for this one -- skip it
        if (dec_.convert(f, pool_[(std::size_t)b].get(), winfo_.width * 4)) {
            ring.push_back(Slot{t, 0, b, kInf});
            ringPending = true;
        } else {
            std::lock_guard<std::mutex> lk(m_);
            releaseLocked(b);
        }
    };
    if (!next_.valid()) {
        // Nothing decodable at all: treat the lap as covered.
    } else if (!landed) {
        // `end` precedes the file's first frame: that frame is the one to show, and nothing is earlier.
        DecodedFrame f = std::move(next_);
        const double t = lapOffset_ + f.t;
        lastT_ = t;
        peekNext();
        keep(f, t, false);
        if (ringPending) { ring.back().until = nextFrameTime(); ringPending = false; }
    } else {
        // Live strides count back from half a frame below the stretch above, on the keyframe's grid, so a
        // frame a container rounded (by up to half a frame) still counts from the right place.
        const double key = lapOffset_ + next_.t;
        const VideoStretch plan = videoPlanStretch(key, fresh ? to : to - 0.5 * fd, fd, cap, offline);
        cap = plan.keep;
        DecodedFrame spare;                        // live: the newest frame not kept, in case none is
        double spareT = 0.0, spareUntil = kInf;
        bool sparePending = false;
        while (!stop_ && peekNext()) {
            const double t = lapOffset_ + next_.t;
            if (ringPending && !ring.empty()) { ring.back().until = t; ringPending = false; }
            if (sparePending) { spareUntil = t; sparePending = false; }
            if (fresh ? t > end + kVideoTimeEps : t > end) break;
            DecodedFrame f = std::move(next_);
            lastT_ = t;
            if (plan.contiguous) {
                keep(f, t, true);
            } else if (videoStretchKeeps(plan, t, fd)) {
                keep(f, t, false);
                spare.reset();
            } else {
                spare = std::move(f);
                spareT = t;
                sparePending = true;
            }
            if (directionChanged()) {              // abandon the stretch; the next step re-plans
                std::lock_guard<std::mutex> lk(m_);
                for (const Slot& s : ring) releaseLocked(s.buf);
                audioClipHi_ = kInf;
                return;
            }
        }
        if (ringPending && !ring.empty()) { ring.back().until = nextFrameTime(); ringPending = false; }
        if (sparePending) spareUntil = nextFrameTime();
        if (!plan.contiguous && ring.empty() && spare.valid()) { keep(spare, spareT, false); if (!ring.empty()) ring.back().until = spareUntil; ringPending = false; }   // never empty
        // Covered down to the keyframe -- or the lap start, when the keyframe is the lap's first frame --
        // unless the offline ring evicted the stretch's lower frames: then only down to its oldest.
        const bool evicted = plan.contiguous && !ring.empty() && ring.front().t > key + kVideoTimeEps;
        const bool atLapStart = key - lapOffset_ < fd * 0.5;
        lo = evicted ? ring.front().t : (atLapStart ? lapOffset_ : key);
    }
    dec_.pumpAudio(end - lapOffset_ + fd);         // settle the stretch's audio up to its end
    takeAudio();
    audioClipHi_ = kInf;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (const Slot& s : ring) insertReadyLocked(Slot{s.t, nextSerial_++, s.buf, s.until});
        const bool firstOfRun = !coverValid_;
        coverLo_ = lo;
        if (firstOfRun) coverHi_ = end;
        coverValid_ = true;
        // Offline, stretches are consecutive: the run grows downwards from the first stretch's end.
        if (firstOfRun || !runValid_) {
            runHi_ = next_.valid() ? lapOffset_ + next_.t : (D > 0.0 ? lapOffset_ + D : kInf);
        }
        runLo_ = lo;
        runValid_ = true;
        runOffline_ = offline;
    }
    lastStretchSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    cv_.notify_all();
}

} // namespace oss
```

- [ ] **Step 7: Add `src/gfx/VideoStream.cpp` after `src/gfx/VideoDecoder.cpp` in BOTH `APP_SOURCES` and the `gl_smoke` source list (the line appears exactly twice)**. In `CMakeLists.txt`, replace:

```cmake
  src/gfx/VideoDecoder.cpp
```

with:

```cmake
  src/gfx/VideoDecoder.cpp
  src/gfx/VideoStream.cpp
```

- [ ] **Step 8: Build and run the scenarios (the last prints only if the others passed: `gl_smoke` stops at a failure)**

```bash
cmake --build build --target gl_smoke -j8 && ./build/gl_smoke 2>&1 | grep -E 'VideoStream|reverse stays|loop off follows|readiness is exact|FAIL'
```

Expected output includes: `offline readiness is exact through rate changes`

- [ ] **Step 9: Commit**

```bash
git add src/gfx/VideoStream.h src/gfx/VideoStream.cpp CMakeLists.txt tests/gl_smoke.cpp
git commit -F - <<'EOF'
feat(gfx): add VideoStream, a background decoder for one video file

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

### Task 8: The Video Player on the worker (the fix)

**Files:**
- Modify (full rewrite): `src/modules/VideoPlayerNode.h`, `src/modules/VideoPlayerNode.cpp`
- Modify: `tests/gl_smoke.cpp`

The node keeps its ports and semantics but no longer decodes. Each frame: advance the unwrapped playhead (`videoAdvance`; held at the start until the first frame is on screen), post it to the stream, upload the newest ready frame at or before it into a staging texture and flip it into the published texture with one `glBlitFramebuffer` (when the file changes, the last picture stays up until the new file's first frame replaces it), and read the matching audio. Offline, it waits for the exact frame (up to 10 s, for a frame its guess did not predict — but never on a render's first frame, a pre-roll frame the renderer never captures, which can need a whole reverse stretch) and prefetches the next one, reporting it through `loading()` so the renderer's gate does the waiting; a stream that fails after it opened keeps `loading()` true offline, so the render fails naming the node instead of going on without the picture. An old stream (a file change, the node's destructor) goes to `VideoStream::retire()`, never waited for. The existing scenario must now drive the node the way the renderer does (offline + gated on `loading()`), plus a live check that polls; four new scenarios pin the fix. At 160x90 decoding is so fast that time bounds alone prove little, so the hitch checks that `evaluate()` returned WITHOUT the new frame, and the shutdown fills a 1080p pool (about 100 ms to free in place) before timing the file change.

- [ ] **Step 1: Add the test helpers, just above `// --- Scenario 10: Video Player decodes a file to texture + audio ---`: gated evaluation, and reading the indexed clip's frame number back from a texture (a read-back names the frame on screen — and a wrong vertical flip reads a different number)**:

```cpp
// Evaluate the way the OfflineRenderer does: wait while any node reports loading() (the Video
// Player decodes on a worker thread), then run one frame. False on timeout.
static bool evaluateGated(Graph& g, float dt, double timeoutSeconds = 10.0) {
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& n : g.nodes())
        while (n->loading()) {
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > timeoutSeconds)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    g.evaluate(dt);
    return true;
}

// The index painted by writeIndexedClip, read from bottom-up RGBA pixels (a texture or a decoded
// VideoFrame).
static int readFrameIndex(const unsigned char* px, int w, int h) {
    int idx = 0;
    for (int band = 0; band < 9; ++band) {
        const int fromTop = (2 * band + 1) * h / 18;       // the band's centre row, from the top
        const int y = h - 1 - fromTop;
        idx = idx * 2 + (px[((std::size_t)y * w + w / 2) * 4] > 127 ? 1 : 0);
    }
    return idx;
}
static int readFrameIndex(TexRef t) {
    std::vector<unsigned char> px((std::size_t)t.w * t.h * 4);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return readFrameIndex(px.data(), t.w, t.h);
}

```

- [ ] **Step 2: Replace the whole of Scenario 10 (from its `// --- Scenario 10:` comment up to, not including, `// --- Scenario 11:`) so it runs gated in offline mode and adds a live check**. In `tests/gl_smoke.cpp`, replace:

```cpp
// --- Scenario 10: Video Player decodes a file to texture + audio ---
// Decodes tests/assets/test.mp4 (a 128x96 colour pattern with a 330 Hz tone),
// first through the bare VideoDecoder, then through the VideoPlayerNode wired
// to an Output -- checking the picture becomes a non-black texture, the node
// emits non-silent audio, the playhead advances forward, and a negative rate
// walks it backwards (reverse playback).
static bool scenario_video_player_decode() {
    {
        // (a) VideoDecoder produces video frames and resampled audio directly.
        VideoDecoder dec;
        std::string err;
        if (!dec.open("tests/assets/test.mp4", err)) {
            return failed(("video open failed: " + err).c_str());
        }
        if (dec.width() != 128 || dec.height() != 96) { return failed("video dimensions wrong"); }
        if (!dec.hasAudio()) { return failed("test video should have an audio track"); }

        VideoFrame vf;
        std::vector<float> audio; double aStart = 0.0; bool aValid = false;
        int frames = 0;
        while (frames < 8 && dec.decodeFrame(vf, audio, aStart, aValid)) ++frames;
        if (frames == 0) { return failed("decoded no video frames"); }
        bool audioNonZero = false;
        for (float s : audio) if (s > 0.01f || s < -0.01f) { audioNonZero = true; break; }
        if (audio.empty() || !audioNonZero) { return failed("decoded no (non-silent) audio"); }
        std::fprintf(stderr, "gl_smoke OK: VideoDecoder decoded %d frames + %zu audio samples\n",
                     frames, audio.size());

        // (b) VideoPlayerNode -> Output: forward play, non-black texture + audio.
        Graph g;
        auto vid = std::make_unique<VideoPlayerNode>();
        vid->inputDefault(0) = std::string("tests/assets/test.mp4");   // file
        vid->inputDefault(3) = false;                                   // loop off (deterministic)
        auto out = std::make_unique<OutputNode>();
        vid->initGL(); out->initGL();
        int vId = g.addNode(std::move(vid));
        int oId = g.addNode(std::move(out));
        if (!g.connect(vId, 0, oId, 0)) { return failed("connect Video->Output"); }
        auto* outNode = dynamic_cast<OutputNode*>(g.findNode(oId));
        auto* vidNode = dynamic_cast<VideoPlayerNode*>(g.findNode(vId));

        // Traverse most of the clip so the sliding window has to extend forward
        // across several keyframes, accumulating "did we ever see picture/audio".
        bool sawColour = false, sawNodeAudio = false;
        for (int f = 0; f < 12; ++f) {
            g.evaluate(1.0f / 10.0f);   // ~10 fps clip, 12 frames ~= 1.2s
            AudioRef na = vidNode->audioOut();
            for (std::size_t i = 0; i < na.count; ++i)
                if (na.samples[i] > 0.01f || na.samples[i] < -0.01f) { sawNodeAudio = true; break; }
            TexRef t = outNode->current();
            if (t.id) {
                std::vector<unsigned char> px((size_t)t.w * t.h * 4);
                glBindTexture(GL_TEXTURE_2D, t.id);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                for (size_t i = 0; i < px.size(); i += 4)
                    if (px[i] > 30 || px[i+1] > 30 || px[i+2] > 30) { sawColour = true; break; }
            }
        }
        if (!sawColour)    { return failed("video produced no visible texture"); }
        if (!sawNodeAudio) { return failed("video node emitted no audio"); }

        double fwd = vidNode->playhead();
        if (!(fwd > 0.5)) { return failed("playhead did not advance through the clip on forward play"); }

        // (c) reverse: a negative rate walks the playhead backwards, forcing the
        // window to re-seek to an earlier keyframe and rebuild.
        vidNode->inputDefault(1) = -1.0f;   // rate
        double before = vidNode->playhead();
        for (int f = 0; f < 5; ++f) g.evaluate(1.0f / 10.0f);
        double after = vidNode->playhead();
        if (!(after < before)) { return failed("playhead did not move backwards on reverse play"); }
        std::fprintf(stderr, "gl_smoke OK: VideoPlayer rendered + audio, advanced to %.2fs, reversed %.2f->%.2f\n",
                     fwd, before, after);
    }
    return true;
}
```

with:

```cpp
// --- Scenario 10: Video Player decodes a file to texture + audio ---
// Decodes tests/assets/test.mp4 (a 128x96 colour pattern with a 330 Hz tone),
// first through the bare VideoDecoder, then through the VideoPlayerNode wired
// to an Output -- checking the picture becomes a non-black texture, the node
// emits non-silent audio, the playhead advances forward, and a negative rate
// walks it backwards (reverse playback).
static bool scenario_video_player_decode() {
    {
        // (a) VideoDecoder produces video frames and resampled audio directly.
        VideoDecoder dec;
        std::string err;
        if (!dec.open("tests/assets/test.mp4", err)) {
            return failed(("video open failed: " + err).c_str());
        }
        if (dec.width() != 128 || dec.height() != 96) { return failed("video dimensions wrong"); }
        if (!dec.hasAudio()) { return failed("test video should have an audio track"); }

        VideoFrame vf;
        std::vector<float> audio; double aStart = 0.0; bool aValid = false;
        int frames = 0;
        while (frames < 8 && dec.decodeFrame(vf, audio, aStart, aValid)) ++frames;
        if (frames == 0) { return failed("decoded no video frames"); }
        bool audioNonZero = false;
        for (float s : audio) if (s > 0.01f || s < -0.01f) { audioNonZero = true; break; }
        if (audio.empty() || !audioNonZero) { return failed("decoded no (non-silent) audio"); }
        std::fprintf(stderr, "gl_smoke OK: VideoDecoder decoded %d frames + %zu audio samples\n",
                     frames, audio.size());

        // (b) VideoPlayerNode -> Output: forward play, non-black texture + audio.
        Graph g;
        auto vid = std::make_unique<VideoPlayerNode>();
        vid->inputDefault(0) = std::string("tests/assets/test.mp4");   // file
        vid->inputDefault(3) = false;                                   // loop off (deterministic)
        auto out = std::make_unique<OutputNode>();
        vid->initGL(); out->initGL();
        int vId = g.addNode(std::move(vid));
        int oId = g.addNode(std::move(out));
        if (!g.connect(vId, 0, oId, 0)) { return failed("connect Video->Output"); }
        auto* outNode = dynamic_cast<OutputNode*>(g.findNode(oId));
        auto* vidNode = dynamic_cast<VideoPlayerNode*>(g.findNode(vId));

        // Decoding is asynchronous, so drive it like an offline render: exact frames, gated on
        // loading(). Traverse most of the clip across several keyframes, accumulating "did we ever
        // see picture/audio".
        g.setOffline(true);
        bool sawColour = false, sawNodeAudio = false;
        for (int f = 0; f < 12; ++f) {
            if (!evaluateGated(g, 1.0f / 10.0f)) { return failed("video: loading() never cleared"); }   // ~10 fps clip, 12 frames ~= 1.2s
            AudioRef na = vidNode->audioOut();
            for (std::size_t i = 0; i < na.count; ++i)
                if (na.samples[i] > 0.01f || na.samples[i] < -0.01f) { sawNodeAudio = true; break; }
            TexRef t = outNode->current();
            if (t.id) {
                std::vector<unsigned char> px((size_t)t.w * t.h * 4);
                glBindTexture(GL_TEXTURE_2D, t.id);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                for (size_t i = 0; i < px.size(); i += 4)
                    if (px[i] > 30 || px[i+1] > 30 || px[i+2] > 30) { sawColour = true; break; }
            }
        }
        if (!sawColour)    { return failed("video produced no visible texture"); }
        if (!sawNodeAudio) { return failed("video node emitted no audio"); }

        double fwd = vidNode->playhead();
        if (!(fwd > 0.5)) { return failed("playhead did not advance through the clip on forward play"); }

        // (c) reverse: a negative rate walks the playhead backwards (the worker decodes reverse stretches).
        vidNode->inputDefault(1) = -1.0f;   // rate
        double before = vidNode->playhead();
        for (int f = 0; f < 5; ++f)
            if (!evaluateGated(g, 1.0f / 10.0f)) { return failed("video reverse: loading() never cleared"); }
        double after = vidNode->playhead();
        if (!(after < before)) { return failed("playhead did not move backwards on reverse play"); }
        std::fprintf(stderr, "gl_smoke OK: VideoPlayer rendered + audio, advanced to %.2fs, reversed %.2f->%.2f\n",
                     fwd, before, after);
    }
    {
        // (d) live: the picture arrives from the worker with no evaluate() waiting for it, and the playhead
        // holds at the start until the first frame is up (it would otherwise skip the first frames). Then a
        // switch to another file (another size) keeps the last picture up until the new file's first frame
        // replaces it, instead of flashing black while that file opens.
        Graph g;
        auto vid = std::make_unique<VideoPlayerNode>();
        vid->inputDefault(0) = std::string("tests/assets/test.mp4");
        auto out = std::make_unique<OutputNode>();
        vid->initGL(); out->initGL();
        int vId = g.addNode(std::move(vid));
        int oId = g.addNode(std::move(out));
        if (!g.connect(vId, 0, oId, 0)) { return failed("live video: connect"); }
        auto* outNode = dynamic_cast<OutputNode*>(g.findNode(oId));
        auto* vp = dynamic_cast<VideoPlayerNode*>(g.findNode(vId));
        const auto t0 = std::chrono::steady_clock::now();
        bool sawColour = false;
        double worstMs = 0.0;
        while (!sawColour && secondsSince(t0) < 5.0) {
            const auto f0 = std::chrono::steady_clock::now();
            g.evaluate(1.0f / 60.0f);
            worstMs = std::max(worstMs, secondsSince(f0) * 1000.0);
            if (!vp->hasFrame() && vp->playhead() != 0.0) { return failed("live video: the playhead ran before the first frame was up"); }
            TexRef t = outNode->current();
            if (t.id) { int r, gg, b, a; readCentre(t, r, gg, b, a); sawColour = r > 30 || gg > 30 || b > 30; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!sawColour) { return failed("live video: no picture within 5 s"); }
        if (worstMs > 50.0) { return failed("live video: an evaluate() waited for the worker"); }
        const std::string next = "build/_video_switch.mp4";         // 160x90; test.mp4 is 128x96
        if (!writeIndexedClip(next, 25, 25)) { return failed("live video: write the second clip"); }
        vp->inputDefault(0) = next;
        const auto s0 = std::chrono::steady_clock::now();
        do {
            g.evaluate(1.0f / 60.0f);
            if (!vp->hasFrame() && outNode->current().id == 0) { return failed("live video: the output went black while the next file opened"); }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (!vp->hasFrame() && secondsSince(s0) < 5.0);
        const TexRef shown = outNode->current();
        if (!vp->hasFrame() || shown.w != kIdxW || shown.h != kIdxH || readFrameIndex(shown) != 0) {
            return failed("live video: the next file's first frame did not replace the last picture");
        }
        std::fprintf(stderr, "gl_smoke OK: live VideoPlayer showed a picture after %.0f ms\n", secondsSince(t0) * 1000.0);
    }
    return true;
}

```

- [ ] **Step 3: Add the four new scenarios, just above `// --- Scenario 11: Text 2D / Text 3D -> geometry -> renderers ---`**:

```cpp
// --- Scenario: a hitch between keyframes no longer locks the Video Player up ---
// The bug: decoding ran on the UI thread, a slow frame pushed the playhead out of the cached window,
// and the rebuild -- seek back to the keyframe, decode up to 48 frames -- made the next frame slower
// still. With keyframes more than 48 frames apart the rebuilt window even ended BEFORE the playhead,
// so every frame rebuilt forever. Now a 3 s jump into the middle of a 250-frame keyframe interval
// must neither block evaluate() -- it returns without the new frame, which the worker has yet to
// decode (at 160x90 decoding is so fast that a time bound alone could not tell) -- nor stop the
// picture catching up. A 1-pixel scissor box is left on throughout: the upload must ignore it.
static bool scenario_video_player_hitch_recovery() {
    {
        const std::string clip = "build/_video_longgop.mp4";
        if (!writeIndexedClip(clip, 300, 250)) { return failed("video hitch: write clip"); }
        Graph g;
        auto vid = std::make_unique<VideoPlayerNode>();
        vid->inputDefault(0) = clip;
        vid->inputDefault(3) = false;                                // loop off
        auto out = std::make_unique<OutputNode>();
        vid->initGL(); out->initGL();
        int vId = g.addNode(std::move(vid));
        int oId = g.addNode(std::move(out));
        if (!g.connect(vId, 0, oId, 0)) { return failed("video hitch: connect"); }
        auto* vp = dynamic_cast<VideoPlayerNode*>(g.findNode(vId));
        auto* outNode = dynamic_cast<OutputNode*>(g.findNode(oId));
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 1, 1);

        auto t0 = std::chrono::steady_clock::now();
        while (!vp->hasFrame() && secondsSince(t0) < 5.0) {   // live: open + first frame
            g.evaluate(0.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!vp->hasFrame()) { return failed("video hitch: no first frame within 5 s"); }

        t0 = std::chrono::steady_clock::now();
        g.evaluate(3.0f);                                            // the hitch: 75 frames past keyframe 0
        const double hitchMs = secondsSince(t0) * 1000.0;
        if (hitchMs > 50.0) {
            std::fprintf(stderr, "video hitch: evaluate() took %.0f ms\n", hitchMs);
            return failed("video hitch: evaluate() blocked after the hitch -- decoding is on the UI thread");
        }
        if (!(vp->shownFrameTime() < vp->playhead() - 1.0 / kIdxFps)) {
            return failed("video hitch: evaluate() returned with the caught-up frame -- it waited for the worker");
        }
        double worstMs = 0.0;
        bool caughtUp = false;
        t0 = std::chrono::steady_clock::now();
        while (!caughtUp && secondsSince(t0) < 1.0) {
            const auto f0 = std::chrono::steady_clock::now();
            g.evaluate(1.0f / 60.0f);
            worstMs = std::max(worstMs, secondsSince(f0) * 1000.0);
            caughtUp = std::fabs(vp->playhead() - vp->shownFrameTime()) <= 1.0 / kIdxFps + 1e-6;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        if (!caughtUp) { return failed("video hitch: the picture did not catch up with the playhead within 1 s"); }
        if (worstMs > 50.0) { return failed("video hitch: an evaluate() blocked while catching up"); }
        const int expect = (int)std::lround(vp->shownFrameTime() * kIdxFps);
        const int got = readFrameIndex(outNode->current());
        if (got != expect) {
            std::fprintf(stderr, "video hitch: texture shows frame %d, expected %d\n", got, expect);
            return failed("video hitch: the texture does not hold the frame the node reports");
        }
        glDisable(GL_SCISSOR_TEST);
        std::fprintf(stderr, "gl_smoke OK: a 3 s hitch mid-keyframe-interval: evaluate %.1f ms, caught up to frame %d in %.0f ms (worst frame %.1f ms)\n",
                     hitchMs, got, secondsSince(t0) * 1000.0, worstMs);
    }
    return true;
}

// Render `frames` frames of a VideoPlayer -> Output (+ Audio Out) graph through the real
// OfflineRenderer at the indexed clip's size and rate, one frame per step(), then decode the file: the
// band index of every frame, and the RMS of every frame's audio block. `rateAt(e)`, if given, sets `vp`'s
// rate before the e-th frame is evaluated (e from 0, the one pre-roll frame first): an automated rate.
// `worstStepMs`, if given, gets the longest step() -- the longest the UI waited.
static bool renderVideoPlayer(Graph& g, int frames, const std::string& outPath,
                              std::vector<int>& idx, std::vector<double>& rms, VideoPlayerNode* vp = nullptr,
                              float (*rateAt)(long long) = nullptr, double* worstStepMs = nullptr) {
    g.transport().bpm = 120.0;                                       // 2 s per bar
    RenderSettings s;
    s.startBar = 0.0; s.endBar = frames / (double)kIdxFps / 2.0; s.prerollBars = 0.0;
    s.fps = kIdxFps; s.width = kIdxW; s.height = kIdxH; s.outPath = outPath;
    std::remove(s.outPath.c_str());
    OfflineRenderer r; std::string err;
    if (!r.start(g, s, err)) { std::fprintf(stderr, "render: %s\n", err.c_str()); return false; }
    long long evaluated = 0;
    for (int guard = 0;; ++guard) {
        if (rateAt && vp) vp->inputDefault(1) = rateAt(evaluated);
        const long long before = r.progress().prerollDone + r.progress().framesDone;
        const auto s0 = std::chrono::steady_clock::now();
        const bool more = r.step(0.0);                               // at most one frame
        if (worstStepMs) *worstStepMs = std::max(*worstStepMs, secondsSince(s0) * 1000.0);
        evaluated += r.progress().prerollDone + r.progress().framesDone - before;
        if (!more) break;
        if (r.progress().waitingForLoad) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (guard > 1000000) return false;
    }
    if (r.progress().phase != OfflineRenderer::Phase::Done) {
        std::fprintf(stderr, "render: %s\n", r.progress().status.c_str());
        return false;
    }
    VideoDecoder dec; std::string derr;
    if (!dec.open(s.outPath, derr)) return false;
    VideoFrame vf; std::vector<float> au; double aS = 0; bool aV = false;
    idx.clear();
    std::vector<float> all;
    while (dec.decodeFrame(vf, au, aS, aV)) idx.push_back(readFrameIndex(vf.rgba, vf.width, vf.height));
    all.swap(au);
    rms.clear();
    const std::size_t block = 48000 / kIdxFps;
    for (std::size_t b = 0; b + block <= all.size(); b += block) {
        double e = 0.0;
        for (std::size_t i = b; i < b + block; ++i) e += (double)all[i] * all[i];
        rms.push_back(std::sqrt(e / block));
    }
    return true;
}

// A graph with one Video Player on the indexed clip feeding an Output and an Audio Out.
static VideoPlayerNode* buildVideoRenderGraph(Graph& g, const std::string& clip, bool loop, float rate) {
    auto vid = std::make_unique<VideoPlayerNode>();
    vid->inputDefault(0) = clip;
    vid->inputDefault(1) = rate;
    vid->inputDefault(3) = loop;
    auto out = std::make_unique<OutputNode>();
    auto ao  = std::make_unique<AudioOutputNode>();
    vid->initGL(); out->initGL();
    int vId = g.addNode(std::move(vid));
    int oId = g.addNode(std::move(out));
    int aId = g.addNode(std::move(ao));
    if (!g.connect(vId, 0, oId, 0) || !g.connect(vId, 1, aId, 0) || !g.connect(vId, 1, aId, 1)) return nullptr;
    return dynamic_cast<VideoPlayerNode*>(g.findNode(vId));
}

// Place the (live) playhead at `seconds`: open the file, then one evaluate() with that dt.
static bool parkPlayhead(Graph& g, VideoPlayerNode* vp, float seconds) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!vp->hasFrame() && secondsSince(t0) < 5.0) {
        g.evaluate(0.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!vp->hasFrame()) return false;
    g.evaluate(seconds);
    return true;
}

// --- Scenario: offline renders of the Video Player are frame-exact, forward and reverse ---
// Parked mid-way between keyframes 250 frames apart, a render forward must show every frame in
// order, and a render in reverse every frame backwards -- through the real OfflineRenderer, so
// loading() and the renderer's gate are part of what is tested. So must a render whose rate is
// automated (+1, +2, then -1): each change leaves the node's guess at the next frame wrong, and
// evaluate() waits for the exact frame itself. And a render's first frame -- a pre-roll frame, never
// captured -- is not waited for inside evaluate(): here it needs a reverse stretch through 87 frames
// of 1080p (seconds at 4K), so a render could not even start without freezing the UI.
static bool scenario_video_player_offline_exact() {
    const std::string clip = "build/_video_longgop.mp4";            // written by the hitch scenario
    for (float rate : {1.0f, -1.0f}) {
        Graph g;
        VideoPlayerNode* vp = buildVideoRenderGraph(g, clip, false, 1.0f);
        if (!vp) { return failed("video offline: build graph"); }
        if (!parkPlayhead(g, vp, rate > 0 ? 5.0f : 7.0f)) { return failed("video offline: open"); }
        vp->inputDefault(1) = rate;                                  // park playing forward, then set the rate
        const int start = (int)std::lround(vp->playhead() * kIdxFps);   // 125 or 175
        std::vector<int> idx; std::vector<double> rms;
        if (!renderVideoPlayer(g, 20, "build/_video_offline_exact.mp4", idx, rms)) { return failed("video offline: render"); }
        if (idx.size() != 20) { return failed("video offline: expected 20 frames"); }
        // One burned pre-roll frame, then frame k shows start + (k + 2) * direction.
        for (int k = 0; k < 20; ++k) {
            const int expect = start + (k + 2) * (rate > 0 ? 1 : -1);
            if (idx[(std::size_t)k] != expect) {
                std::fprintf(stderr, "video offline (rate %.0f): frame %d shows %d, expected %d\n", rate, k, idx[(std::size_t)k], expect);
                return failed("video offline: a rendered frame is not the exact frame for the playhead");
            }
        }
        std::fprintf(stderr, "gl_smoke OK: offline render at rate %+.0f from frame %d is frame-exact (%d..%d)\n",
                     rate, start, idx.front(), idx.back());
    }
    {
        Graph g;
        VideoPlayerNode* vp = buildVideoRenderGraph(g, clip, false, 1.0f);
        if (!vp || !parkPlayhead(g, vp, 5.0f)) { return failed("video offline: open"); }
        auto rateAt = [](long long e) { return e < 10 ? 1.0f : (e < 20 ? 2.0f : -1.0f); };
        double u = std::round(vp->playhead() * kIdxFps);             // in frames: 125
        std::vector<int> idx; std::vector<double> rms;
        if (!renderVideoPlayer(g, 30, "build/_video_offline_rates.mp4", idx, rms, vp, rateAt)) {
            return failed("video offline: render with an automated rate");
        }
        if (idx.size() != 30) { return failed("video offline: expected 30 frames"); }
        for (long long e = 0; e <= 30; ++e) {                        // evaluate e moves u; frame e - 1 shows it
            u += rateAt(e);
            if (e >= 1 && idx[(std::size_t)(e - 1)] != (int)u) {
                std::fprintf(stderr, "video offline (automated rate): frame %lld shows %d, expected %d\n", e - 1, idx[(std::size_t)(e - 1)], (int)u);
                return failed("video offline: an automated rate renders a wrong frame");
            }
        }
    }
    {
        const std::string big = "build/_video_1080.mp4";              // 1080p, one keyframe; reused by shutdown
        if (!writeIndexedClip(big, 100, 100, 1920, 1080)) { return failed("video offline: write the 1080p clip"); }
        Graph g;
        VideoPlayerNode* vp = buildVideoRenderGraph(g, big, false, 1.0f);
        if (!vp || !parkPlayhead(g, vp, 3.5f)) { return failed("video offline: open the 1080p clip"); }
        vp->inputDefault(1) = -1.0f;
        const int start = (int)std::floor(vp->playhead() * kIdxFps + 1e-6);   // 3.5 s: frame 87
        std::vector<int> idx; std::vector<double> rms;
        double worstMs = 0.0;
        if (!renderVideoPlayer(g, 10, "build/_video_offline_first.mp4", idx, rms, nullptr, nullptr, &worstMs) ||
            idx.size() != 10) {
            return failed("video offline: render 10 frames of the 1080p clip in reverse");
        }
        for (int k = 0; k < 10; ++k)
            if (idx[(std::size_t)k] != start - (k + 2)) {
                std::fprintf(stderr, "video offline (1080p): frame %d shows %d, expected %d\n", k, idx[(std::size_t)k], start - (k + 2));
                return failed("video offline: the 1080p reverse render is not frame-exact");
            }
        if (worstMs > 100.0) {
            std::fprintf(stderr, "video offline: longest step() %.0f ms\n", worstMs);
            return failed("video offline: the render's first frame was waited for inside evaluate()");
        }
        std::fprintf(stderr, "gl_smoke OK: an automated rate (+1, +2, -1) renders every frame exactly; a render's "
                     "first frame is not waited for (longest step %.0f ms)\n", worstMs);
    }
    return true;
}

// --- Scenario: a loop is seamless -- no stale frame and no silent audio block at the wrap ---
static bool scenario_video_player_loop_seam() {
    {
        const std::string clip = "build/_video_longgop.mp4";
        Graph g;
        VideoPlayerNode* vp = buildVideoRenderGraph(g, clip, true, 1.0f);
        if (!vp) { return failed("video loop: build graph"); }
        if (!parkPlayhead(g, vp, 11.6f)) { return failed("video loop: open"); }   // 10 frames before the end
        std::vector<int> idx; std::vector<double> rms;
        if (!renderVideoPlayer(g, 20, "build/_video_loop_seam.mp4", idx, rms)) { return failed("video loop: render"); }
        for (int k = 0; k < (int)idx.size(); ++k) {
            const int expect = (290 + k + 2) % 300;
            if (idx[(std::size_t)k] != expect) {
                std::fprintf(stderr, "video loop: frame %d shows %d, expected %d\n", k, idx[(std::size_t)k], expect);
                return failed("video loop: the frames across the loop are not consecutive");
            }
        }
        for (std::size_t b = 1; b + 1 < rms.size(); ++b)        // skip the encoder's first/last blocks
            if (rms[b] < 0.05) {
                std::fprintf(stderr, "video loop: audio block %zu RMS %.3f\n", b, rms[b]);
                return failed("video loop: a silent audio block -- the loop is not seamless");
            }
        std::fprintf(stderr, "gl_smoke OK: looping across the end is seamless (frames %d..%d, no silent audio block)\n",
                     idx.front(), idx.back());
    }
    return true;
}

// --- Scenario: changing the file or deleting the node mid-catch-up returns at once ---
// The old stream goes to VideoStream::retire(), which tears it down on another thread. Torn down in place
// -- here with a full 1080p pool, 530 MB -- it takes about 100 ms (0.1-0.5 s at 4K), and the UI would
// wait it out.
static bool scenario_video_player_shutdown() {
    {
        const std::string clip = "build/_video_1080.mp4";           // written by the offline scenario
        for (int mode = 0; mode < 2; ++mode) {
            Graph g;
            auto vid = std::make_unique<VideoPlayerNode>();
            vid->inputDefault(0) = clip;
            vid->initGL();
            int vId = g.addNode(std::move(vid));
            auto* vp = dynamic_cast<VideoPlayerNode*>(g.findNode(vId));
            if (!parkPlayhead(g, vp, 0.0f)) { return failed("video shutdown: open"); }
            const auto p0 = std::chrono::steady_clock::now();
            while (secondsSince(p0) < 0.6) {                        // play: the worker fills the pool
                g.evaluate(1.0f / 60.0f);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            g.evaluate(3.0f);                                        // a jump past the pool: a catch-up
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto t0 = std::chrono::steady_clock::now();
            if (mode == 0) { vp->inputDefault(0) = std::string("tests/assets/test.mp4"); g.evaluate(0.0f); }
            else           { g.removeNode(vId); }
            const double ms = secondsSince(t0) * 1000.0;
            if (ms > 25.0) {
                std::fprintf(stderr, "video shutdown: %s took %.0f ms\n", mode == 0 ? "changing the file" : "deleting the node", ms);
                return failed("video shutdown: the caller waited for the old stream's teardown");
            }
            std::fprintf(stderr, "gl_smoke OK: %s mid-catch-up returned in %.1f ms\n",
                         mode == 0 ? "changing the file" : "deleting the node", ms);
        }
    }
    return true;
}
```

- [ ] **Step 4: Register them in `kScenarios`, just below `scenario_video_player_decode,`**. In `tests/gl_smoke.cpp`, replace:

```cpp
    scenario_video_player_decode,
```

with:

```cpp
    scenario_video_player_decode,
    scenario_video_player_hitch_recovery,
    scenario_video_player_offline_exact,
    scenario_video_player_loop_seam,
    scenario_video_player_shutdown,
```

- [ ] **Step 5: Build to verify it fails**

```bash
cmake --build build --target gl_smoke -j8
```

Expected: the build FAILS, with an error mentioning `no member named 'hasFrame'`.

- [ ] **Step 6: Replace `src/modules/VideoPlayerNode.h`** — `src/modules/VideoPlayerNode.h` (complete file)

```cpp
#pragma once
#include <glad/gl.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/VideoPlan.h"
#include "gfx/VideoStream.h"
#include "audio/AudioBlock.h"

namespace oss {

// Plays a video file (named by its string input), streaming the picture as a
// texture (output 0) and the soundtrack as 48 kHz mono audio (output 1). The
// `rate` input is a signed playback speed: 1 = normal, 0.5 = half, 2 = double,
// and NEGATIVE values play in reverse (the audio is swept backwards and won't
// sound musical -- that's expected). `play` pauses; `loop` wraps at the ends.
//
// Decoding runs on a background worker (gfx/VideoStream), so the UI never waits on FFmpeg.
// Each frame the node advances an UNWRAPPED playhead (it keeps counting past the end rather
// than jumping back, so the worker can decode the next lap early -- see core/VideoPlan.h),
// posts it to the stream, and uploads the newest ready frame at or before it. The worker
// converts rows top-down, so the upload lands in a staging texture and one flipped
// glBlitFramebuffer puts it bottom-up into the published texture. When the file changes, the
// last picture stays up until the new file's first frame replaces it.
//
// Offline renders stay frame-exact: evaluate() waits for the exact frame, and loading()
// reports the NEXT frame not ready yet, so the renderer's gate does the waiting between frames.
// A stream that fails mid-render keeps loading() true, so the render fails rather than going on
// without the picture. Old streams are handed to VideoStream::retire(), never waited for.
class VideoPlayerNode : public Node {
public:
    VideoPlayerNode();
    ~VideoPlayerNode() override;
    void initGL() override;
    void evaluate(EvalContext& ctx) override;
    std::string statusLine() const override { return status_; }
    bool loading() const override;

    // Test/inspection accessors.
    double   playhead() const { return videoPosition(ph_, loop_, duration_); }   // position in the clip
    bool     hasFrame() const { return shownSerial_ != 0; }   // a frame of the current file is on screen
    double   shownFrameTime() const { return shownT_; }       // its unwrapped time (valid when hasFrame())
    AudioRef audioOut() const { return AudioRef{outBuf_.data(), (std::size_t)lastAudioN_, outRate_}; }

    // Offline: how long evaluate() waits for a frame the prefetch did not predict (an automated rate, a
    // direction flip). A reverse stretch through a long keyframe interval at 4K takes seconds, and the UI
    // waits with it; a frame that takes longer latches a stall, and the render fails naming the node.
    static constexpr double kOfflineFrameWaitSeconds = 10.0;

private:
    void openPath(const std::string& path);
    VideoRequest makeRequest(const VideoPlayhead& p, bool play, float rate, bool loop) const;
    void ensureTextures(int w, int h);
    void freeGL();
    void upload(const VideoStream::FrameView& f);
    void publishHeld(EvalContext& ctx);
    void updateStatus(bool play, float rate);

    std::unique_ptr<VideoStream> stream_;
    std::string   path_;
    std::string   status_;
    bool          needInfo_ = false;     // the stream is new: read its info once it is Ready
    int           vidW_ = 0, vidH_ = 0;  // its frame size
    bool          opened_ = false;       // it has been Ready: a failure now is mid-play
    bool          failLogged_ = false;
    double        duration_ = 0.0;
    double        frameDur_ = 1.0 / 30.0;
    VideoPlayhead ph_;
    bool          loop_ = true;

    GLuint        stageTex_ = 0;         // receives the worker's top-down rows
    GLuint        tex_      = 0;         // published, bottom-up
    GLuint        readFbo_  = 0, drawFbo_ = 0;
    int           texW_ = 0, texH_ = 0;
    std::uint64_t shownSerial_ = 0;      // 0: nothing uploaded from this stream yet
    double        shownT_ = -1.0;
    bool          picture_ = false;      // tex_ holds a picture: this file's, or the last one's until then

    bool          offline_  = false;     // the last evaluate() was part of an offline render
    bool          stalled_  = false;     // offline: a frame missed its wait (latched until live again)
    double        pendingU_ = 0.0;       // offline: the next frame's playhead, prefetched

    int                outRate_   = VideoDecoder::kOutRate;
    std::vector<float> outBuf_;          // owns the samples audioOut/AudioRef points at
    int                lastAudioN_ = 0;
};

} // namespace oss
```

- [ ] **Step 7: Replace `src/modules/VideoPlayerNode.cpp`** — `src/modules/VideoPlayerNode.cpp` (complete file)

```cpp
#include "modules/VideoPlayerNode.h"
#include "core/Value.h"
#include "gfx/GLStateGuard.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace oss {

VideoPlayerNode::VideoPlayerNode() : Node("Video Player"), outBuf_(kAudioMaxBlock, 0.0f) {
    addAssetInput("file", AssetType::Video);   // .mp4/.mov/... path
    addInput("rate", PortType::Float,  1.0f, -2.0f, 2.0f); // signed: negative = reverse
    addInput("play", PortType::Bool,   true);
    addInput("loop", PortType::Bool,   true);
    addOutput("video", PortType::Texture);
    addOutput("audio", PortType::Audio);
}

VideoPlayerNode::~VideoPlayerNode() {
    VideoStream::retire(std::move(stream_));   // torn down on another thread: at 4K that takes 0.1-0.5 s
    freeGL();
}

void VideoPlayerNode::initGL() { /* textures are allocated once the stream knows the size */ }

void VideoPlayerNode::evaluate(EvalContext& ctx) {
    const std::string& path = ctx.in<std::string>(0);
    const float rate = ctx.in<float>(1);
    const bool  play = ctx.in<bool>(2);
    const bool  loop = ctx.in<bool>(3);
    const bool  wasOffline = offline_;
    offline_ = ctx.offline;
    if (!offline_) stalled_ = false;
    loop_ = loop;

    if (path != path_) { path_ = path; openPath(path); }
    if (!stream_) { picture_ = false; publishHeld(ctx); return; }

    const VideoStream::State st = stream_->state();
    if (st == VideoStream::State::Failed) {
        status_ = (opened_ ? "failed: " : "load failed: ") + stream_->error();
        if (!failLogged_) {
            std::fprintf(stderr, "[Video] %s (%s)\n", status_.c_str(), path_.c_str());
            failLogged_ = true;
        }
        picture_ = false;
        publishHeld(ctx);
        return;
    }
    if (st == VideoStream::State::Opening) {
        status_ = "opening...";
        publishHeld(ctx);                              // the last file's picture, until this one's first frame
        return;
    }
    if (needInfo_) {
        opened_ = true;
        const VideoStream::Info inf = stream_->info();
        duration_ = inf.duration;
        frameDur_ = inf.frameDur;
        vidW_ = inf.width;
        vidH_ = inf.height;
        needInfo_ = false;
        std::fprintf(stderr, "[Video] loaded %s (%dx%d, %.1fs, %s)\n", path_.c_str(),
                     inf.width, inf.height, inf.duration, inf.hasAudio ? "audio" : "no audio");
    }

    // Hold the playhead at the start until the first frame is on screen: the worker's first frames
    // take a moment (open, decoder warm-up), and running the clock meanwhile would skip them.
    const bool advancing = play && shownSerial_ != 0;
    // Offline, dt is 1.0f / fps: snap it to the exact frame step so a long render stays on the grid.
    const double dt = offline_ ? videoFrameStep(ctx.dt) : (double)ctx.dt;
    const double prevU = ph_.u;
    ph_ = videoAdvance(ph_, advancing, rate, loop, dt, duration_);
    stream_->request(makeRequest(ph_, play, rate, loop));

    // Offline renders are exact: wait for the frame for u (normally already there -- see below). Not on a
    // render's first frame: it can need a whole reverse stretch decoded, and it is never captured -- the
    // renderer always runs a pre-roll frame first (Node::loading()) -- so the gate waits for the next one.
    if (offline_ && wasOffline && !stream_->frameReadyFor(ph_.u) &&
        !stream_->waitForFrame(ph_.u, kOfflineFrameWaitSeconds))
        stalled_ = true;                               // publish what we have; loading() fails the render

    VideoStream::FrameView fv;
    if (stream_->frameAt(ph_.u, fv) && fv.serial != shownSerial_) {
        ensureTextures(vidW_, vidH_);                  // a new size: made here, so no evaluate() publishes it empty
        upload(fv);
    }
    ctx.out<TexRef>(0, TexRef{ picture_ ? tex_ : 0u, texW_, texH_ });

    // Audio for the slice of source time just played. The playhead is unwrapped, so a loop wrap
    // is a continuous slice too; a pause is silent.
    const int n = audioBlockFrames(outRate_, ctx.dt);
    if (!play || ph_.u == prevU) std::fill(outBuf_.begin(), outBuf_.begin() + n, 0.0f);
    else                         stream_->readAudio(prevU, ph_.u, outBuf_.data(), n);
    lastAudioN_ = n;
    ctx.out<AudioRef>(1, AudioRef{outBuf_.data(), (std::size_t)n, outRate_});

    // Offline, ask for the NEXT frame now: loading() reports it not ready, so the renderer's gate
    // waits between frames instead of this evaluate() blocking the UI.
    if (offline_) {
        const VideoPlayhead next = videoAdvance(ph_, play, rate, loop, dt, duration_);
        pendingU_ = next.u;
        stream_->request(makeRequest(next, play, rate, loop));
    }
    updateStatus(play, rate);
}

bool VideoPlayerNode::loading() const {
    if (!stream_) return false;
    const VideoStream::State st = stream_->state();
    if (st == VideoStream::State::Opening) return true;
    if (!offline_) return false;
    // A stream that fails once open has no frames left to give: hold the render, so it fails naming this
    // node rather than finishing with the picture gone. (One that never opened renders without video.)
    if (st == VideoStream::State::Failed) return opened_;
    return stalled_ || !stream_->frameReadyFor(pendingU_);
}

void VideoPlayerNode::openPath(const std::string& path) {
    VideoStream::retire(std::move(stream_));            // never waits for the old worker
    ph_ = VideoPlayhead{};
    duration_ = 0.0;
    frameDur_ = 1.0 / 30.0;
    shownSerial_ = 0;
    shownT_ = -1.0;
    stalled_ = false;
    pendingU_ = 0.0;
    failLogged_ = false;
    opened_ = false;
    needInfo_ = !path.empty();
    status_.clear();
    if (path.empty()) return;
    stream_ = std::make_unique<VideoStream>(path);
    status_ = "opening...";
}

VideoRequest VideoPlayerNode::makeRequest(const VideoPlayhead& p, bool play, float rate, bool loop) const {
    VideoRequest r;
    r.u       = p.u;
    r.rate    = play ? rate : 0.0f;
    r.offline = offline_;
    r.loop    = loop && duration_ > 0.0;
    if (!r.loop) {
        r.lapLo = duration_ > 0.0 ? p.lapLo : 0.0;
        r.lapHi = duration_ > 0.0 ? p.lapLo + duration_ : std::numeric_limits<double>::infinity();
    }
    return r;
}

void VideoPlayerNode::ensureTextures(int w, int h) {
    if (tex_ && texW_ == w && texH_ == h) return;
    freeGL();
    GLStateGuard guard;                                 // restores bindings the setup disturbs
    auto makeTex = [&](GLuint& t) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    makeTex(stageTex_);
    makeTex(tex_);
    glGenFramebuffers(1, &readFbo_);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo_);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stageTex_, 0);
    glGenFramebuffers(1, &drawFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo_);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    texW_ = w;
    texH_ = h;
    shownSerial_ = 0;
    shownT_ = -1.0;
}

void VideoPlayerNode::freeGL() {
    if (readFbo_)  glDeleteFramebuffers(1, &readFbo_);
    if (drawFbo_)  glDeleteFramebuffers(1, &drawFbo_);
    if (stageTex_) glDeleteTextures(1, &stageTex_);
    if (tex_)      glDeleteTextures(1, &tex_);
    readFbo_ = drawFbo_ = stageTex_ = tex_ = 0;
    texW_ = texH_ = 0;
    picture_ = false;
}

// Upload the worker's top-down rows to the staging texture, then flip them into the published
// texture with one blit (source rows 0..H to destination rows H..0).
void VideoPlayerNode::upload(const VideoStream::FrameView& f) {
    GLStateGuard guard;
    glDisable(GL_SCISSOR_TEST);                         // the blit is clipped by the scissor box
    glBindTexture(GL_TEXTURE_2D, stageTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texW_, texH_, GL_RGBA, GL_UNSIGNED_BYTE, f.rgba);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo_);
    glBlitFramebuffer(0, 0, texW_, texH_, 0, texH_, texW_, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    shownSerial_ = f.serial;
    shownT_ = f.t;
    picture_ = true;
}

// No frame of this file yet: publish the picture held over from the last file (if any), and silence.
void VideoPlayerNode::publishHeld(EvalContext& ctx) {
    ctx.out<TexRef>(0, TexRef{ picture_ ? tex_ : 0u, texW_, texH_ });
    ctx.out<AudioRef>(1, AudioRef{});
}

void VideoPlayerNode::updateStatus(bool play, float rate) {
    const bool buffering = play && rate > 0.0f && shownSerial_ != 0 &&
                           ph_.u - shownT_ > std::max(0.2, 2.0 * frameDur_);
    const char* tail = !play ? " (paused)" : (buffering ? " (buffering)" : "");
    char buf[128];
    if (duration_ > 0.0)
        std::snprintf(buf, sizeof(buf), "%.1f / %.1f s  x%.2f%s", playhead(), duration_, rate, tail);
    else
        std::snprintf(buf, sizeof(buf), "%.1f s  x%.2f%s", playhead(), rate, tail);
    status_ = buf;
}

} // namespace oss
```

- [ ] **Step 8: Build and run the video scenarios**

```bash
cmake --build build --target gl_smoke -j8 && ./build/gl_smoke 2>&1 | grep -E 'gl_smoke (OK|FAIL).*(VideoPlayer|hitch|offline render at|loop|mid-catch)|FAIL'
```

Expected output includes: `a 3 s hitch mid-keyframe-interval`

- [ ] **Step 9: Build the app and run every suite**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure
```

Expected output includes: `100% tests passed`

- [ ] **Step 10: Commit**

```bash
git add src/modules/VideoPlayerNode.h src/modules/VideoPlayerNode.cpp tests/gl_smoke.cpp
git commit -F - <<'EOF'
fix(video): decode the Video Player on a worker so the UI never waits

Loading a large video froze the UI: decoding ran inside evaluate(), a slow frame
pushed the playhead out of the cached window, and the rebuild (seek back, decode
up to 48 frames) made the next frame slower still; with keyframes more than 48
frames apart the rebuilt window ended before the playhead, so it never recovered.
The node now posts an unwrapped playhead to a VideoStream worker and uploads the
newest ready frame; offline renders wait for the exact frame and gate the next.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

### Task 9: Document the design in CLAUDE.md

**Files:**
- Modify: `CLAUDE.md` (two bullets under *Hard rules*, and three references to them)

- [ ] **Step 1: Replace the *Real-time threads bridge through queues* bullet**. In `CLAUDE.md`, replace:

```markdown
- **Real-time threads bridge through queues, not the graph.** Audio (libsoundio) and
  mesh loading (`std::async`) run off the graph thread; they hand data back via a
  lock-free SPSC ring buffer (`src/audio/SpscRingBuffer.h`) or `AsyncLoader`
  (`src/core/AsyncLoader.h`). GL uploads always happen on the main thread.
```

with:

```markdown
- **Real-time threads bridge through queues, not the graph.** Audio (libsoundio),
  mesh loading (`std::async`) and video decoding (a worker per Video Player, `gfx/VideoStream`)
  run off the graph thread; they hand data back via a lock-free SPSC ring buffer
  (`src/audio/SpscRingBuffer.h`), `AsyncLoader` (`src/core/AsyncLoader.h`), or the stream's
  mutex-guarded frame queue and audio store. GL uploads always happen on the main thread.
```

- [ ] **Step 2: Replace the *`VideoDecoder` … is a GL-free FFmpeg wrapper* bullet (it describes the synchronous design this plan removes)**. In `CLAUDE.md`, replace:

```markdown
- **`VideoDecoder` (`src/gfx/VideoDecoder.{h,cpp}`) is a GL-free FFmpeg wrapper** —
  it produces CPU RGBA frames (bottom-up) + 48 kHz mono float audio; FFmpeg headers
  are confined to its `.cpp`. The `VideoPlayerNode` decodes synchronously on the
  graph thread and keeps a sliding keyframe-window frame cache to play forward,
  reverse, and at variable `rate` off a forward-only decoder.
```

with:

```markdown
- **`VideoDecoder` (`src/gfx/VideoDecoder.{h,cpp}`) is a GL-free FFmpeg wrapper** —
  FFmpeg headers are confined to its `.cpp`. Decoding uses FFmpeg's frame threads
  (`thread_count = 0`). `decodeNext()` hands back a `DecodedFrame` (a counted reference,
  not yet converted) and `convert()` turns it into TOP-DOWN RGBA with threaded swscale
  (`sws_scale_frame`, which only writes into refcounted frames, so the caller's buffer is
  wrapped in an `AVBufferRef` whose free callback does nothing). Video packets are queued
  (≤ 64 MB) rather than decoded at once, so `pumpAudio()` keeps 48 kHz mono audio ahead of the
  video; `audioSettledUpTo()` says how far no more audio can arrive. The legacy `decodeFrame()`
  (bottom-up RGBA + audio appended; its converter follows the decoded frames' own format, not the
  stream's) is built on those and kept for `gl_smoke`. Every time in and out counts from the first video
  frame (decoded at `open()`), so a container that starts its clock late (MPEG-TS) or shows a
  B-frame delay (FLV, fragmented MP4) still plays from 0; `open()` also seeks to the start
  first, so indexes read only on a seek (Matroska cues) are there, and MPEG-TS/-PS indexes are
  ignored: their demuxers list every packet a seek probes as a keyframe. A frame with no
  timestamp (the B-frame tail of an AVI or MPEG-PS, a raw H.264 stream) follows the one before
  it, and is skipped when nothing precedes it since a seek into the file. A raw stream has no
  times to seek by, so `seek()` sends it to its first byte (the fallback for any failed seek;
  `seek()` returns false only when not even the start is reachable). Audio comes out in runs
  that follow its timestamps (`takeAudio(out, startT, continues)`: a hole or an overlap starts a
  new run), the resampler is rebuilt when the audio format changes and reset by a seek, and once
  an offline caller (`pumpAudio(t, true)`) is stuck at the 64 MB cap (nothing taken off the queue
  since it last stopped there) the audio counts as settled up to the last packet read. `open()`
  takes an abort flag that FFmpeg's I/O polls; set during the probe, it fails the open
  ("stopped"). A decoder is used by one thread only: its stream's worker.
- **The Video Player decodes on a worker.** `VideoPlayerNode` (`src/modules/VideoPlayerNode.{h,cpp}`)
  drives a `VideoStream` (`src/gfx/VideoStream.{h,cpp}`, one per node, GL-free) whose worker thread
  opens the file, keeps a fixed pool of RGBA frames (512 MB budget, allocated at open:
  `core/VideoPlan.h` `videoPoolFrames`, 4 to 64 frames) decoded ahead of the requested playhead,
  and reads audio into a `core/TimedAudio.h` store. **The worker holds the stream's mutex only
  for bookkeeping, never while decoding or converting**, so `request()`, `frameAt()` and
  `readAudio()` return at once; the frame `frameAt()` returns stays checked out until the next
  `frameAt()`. Its decisions — fill, catch up (decode without converting; live, in 100 ms
  slices), seek (ahead only for jumps over 1 s, since a seek restarts the frame-thread pipeline;
  always for a new direction, or a target behind everything held unless the last seek for it was
  pinned before the file's first frame), wrap, and reverse
  keyframe-to-frame stretches published whole — are the pure, unit-tested `videoNextStep()`. A
  seek lands where its decode loop admits frames, backing off 1 s, 2 s, 4 s… to the file's start
  when it lands late (decode-time indexes, MPEG-TS), and a seek ahead that landed behind the
  decoder is not repeated nearby (`videoNoSeekBelow`). An offline render starting in reverse
  discards the live stretches, which keep every stride-th frame, and decodes offline ones. A new audio chunk begins wherever
  the decoder's audio jumps, and a file it cannot seek in at all fails the stream ("cannot seek in
  this file") rather than spinning.
  The node's playhead is UNWRAPPED (lap × D + position, `videoAdvance`), so the worker decodes the
  next lap early and loops are seamless; the playhead is held at 0 until the first frame is on
  screen. The node uploads the newest frame at or before the playhead into a staging texture and
  flips it into the published texture with one `glBlitFramebuffer` (scissor test off: a blit is
  clipped by it). When the file changes, the last picture stays up until the new file's first
  frame replaces it.
  **Offline renders stay exact.** `loading()` reports the node's GUESS at the next frame (this
  frame's rate) not ready, so the renderer's gate does the waiting between frames with the UI
  running; offline `dt` is snapped to the exact frame step (`videoFrameStep`). Readiness names the
  frame for the playhead exactly (each queued frame knows when the next one decoded starts) and,
  playing forward, needs its audio settled too. A frame the guess did not predict (an automated
  rate, a direction flip) is waited for inside `evaluate()` for up to `kOfflineFrameWaitSeconds`
  (10 s), blocking the UI (seconds, for a fresh reverse stretch at 4K) — except the render's first
  frame, which is always an uncaptured pre-roll frame. A file still opening holds the gate like any
  loader; a frame that misses that wait latches a stall, and a stall or a stream that fails once
  open keeps `loading()` true for good, so the render fails after `kRenderLoadTimeoutSeconds`,
  naming the node.
  Old streams (a file change, the node's destructor) go to `VideoStream::retire()`: tearing a
  stream down takes 0.1–0.5 s at 4K, so a reaper thread does it (joined at exit), and a retired
  stream holds its memory until the reaper destroys it. Tests: `tests/test_video_plan.cpp`,
  `tests/test_timed_audio.cpp`, and the `gl_smoke` scenarios `scenario_video_decoder_*`,
  `scenario_video_stream_*` and `scenario_video_player_*` (untested: the stall latch, and the rule
  for a stream that fails once open).
```

- [ ] **Step 3: Add the Video Player to the *Offline render* bullet's list of `loading()` nodes**. In `CLAUDE.md`, replace:

```markdown
unpolled future can't deadlock the gate; Image Sequencer via `futurePending` on its prefetch),
```

with:

```markdown
unpolled future can't deadlock the gate; Image Sequencer via `futurePending` on its prefetch;
  the Video Player while its file opens, while its next frame is not ready, after a stall, or
  after its stream failed once open),
```

- [ ] **Step 4: In the *Image Streamer* bullet, say which `VideoDecoder` path `ImageLoader` mirrors (the main one is now top-down)**. In `CLAUDE.md`, replace:

```markdown
GL-free `gfx/ImageLoader` (an `stb_image` wrapper mirroring `VideoDecoder`, rows flipped
  bottom-up to match) and publishes it as a `TexRef`; it loads once on path change and
  republishes each frame. `KaleidoscopeNode`
```

with:

```markdown
GL-free `gfx/ImageLoader` (an `stb_image` wrapper mirroring `VideoDecoder::decodeFrame()`,
  rows flipped bottom-up to match) and publishes it as a `TexRef`; it loads once on path
  change and republishes each frame. `KaleidoscopeNode`
```

- [ ] **Step 5: Open the *`VideoEncoder`* bullet with whose mirror it is (a new bullet now stands between them), and mention the keyframe interval**. In `CLAUDE.md`, replace:

```markdown
- **`VideoEncoder` (`src/gfx/VideoEncoder.{h,cpp}`) is its mirror** — a GL-free
  FFmpeg muxer writing RGBA frames + interleaved float audio (mono or stereo) to
  an H.264/AAC mp4. The `RecorderNode` is a pass-through tap (video/audio in → same
  out) that reads back the input texture and feeds the encoder while `record` is on;
```

with:

```markdown
- **`VideoEncoder` (`src/gfx/VideoEncoder.{h,cpp}`) is `VideoDecoder`'s mirror** — a GL-free
  FFmpeg muxer writing RGBA frames + interleaved float audio (mono or stereo) to
  an H.264/AAC mp4 (`open()`'s optional `keyframeInterval`, for test clips, places keyframes
  exactly every N frames, writing with libx264 or the MPEG-4 fallback only). The `RecorderNode`
  is a pass-through tap (video/audio in → same out) that reads back the input texture and feeds
  the encoder while `record` is on;
```

- [ ] **Step 6: Check the new text is in**

```bash
grep -c 'decodes on a worker' CLAUDE.md
```

Expected output includes: `1`

- [ ] **Step 7: Commit**

```bash
git add CLAUDE.md
git commit -F - <<'EOF'
docs: describe the background Video Player in CLAUDE.md

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
EOF
```

### Task 10: Acceptance — measure it on the real clips

**Files:** none committed. The harness and clips live in a scratch directory outside the repository.

These are the spec's acceptance criteria; the prototype's measurements on the development machine (i7-7820HQ) are given so a regression is obvious.

- [ ] **Step 1: Generate the clips** (about 300 MB; in a scratch directory, e.g. `mkdir -p ~/vidtest && cd ~/vidtest`)

```bash
gen() { ffmpeg -y -f lavfi -i "testsrc2=size=$2:rate=30" -f lavfi -i "sine=frequency=330:sample_rate=48000" \
  -t 12 -vf "noise=alls=10:allf=t" -c:v libx264 -preset veryfast $3 -b:v $4 -maxrate $4 -bufsize $4 \
  -pix_fmt yuv420p -c:a aac -ac 2 -movflags +faststart "$1.mp4"; }
gen v4k_gop250 3840x2160 "" 40M;       gen v4k_gop30 3840x2160 "-g 30" 40M
gen v1080_gop250 1920x1080 "" 12M;     gen v1080_gop30 1920x1080 "-g 30" 12M
ffmpeg -y -f lavfi -i "testsrc2=size=3840x2160:rate=30" -t 6 -vf "noise=alls=10:allf=t,format=nv12" \
  -c:v hevc_videotoolbox -b:v 40M -tag:v hvc1 v4k_hevc8.mp4
ffmpeg -y -f lavfi -i "testsrc2=size=3840x2160:rate=30" -t 6 -vf "noise=alls=10:allf=t,format=p010le" \
  -c:v hevc_videotoolbox -profile:v main10 -b:v 40M -tag:v hvc1 v4k_hevc10.mp4
```

- [ ] **Step 2: Save the harness** as `~/vidtest/vp_accept.cpp` (macOS: it reads resident memory through Mach):

```cpp
// Acceptance harness for the reworked VideoPlayerNode: drives it the way src/main.cpp does
// (dt = wall time of the previous frame, ~16.7 ms vsync floor) and reports UI-thread cost,
// how far the picture lags the playhead, and memory.
// usage: vp_accept <video> [wallSec=8] [rate=1] [stallAtSec=-1] [stallMs=0]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <mach/mach.h>
#include "modules/VideoPlayerNode.h"
#include "gfx/VideoDecoder.h"

using namespace oss;
using clk = std::chrono::steady_clock;
static double msSince(clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); }
static double rssMB() {
    mach_task_basic_info info; mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n);
    return info.resident_size / 1048576.0;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string path = argv[1];
    const double wallSec = argc > 2 ? std::atof(argv[2]) : 8.0;
    const float  rate    = argc > 3 ? (float)std::atof(argv[3]) : 1.0f;
    const double stallAt = argc > 4 ? std::atof(argv[4]) : -1.0;
    const double stallMs = argc > 5 ? std::atof(argv[5]) : 0.0;

    double fd = 1.0 / 30.0, dur = 0.0;
    { VideoDecoder d; std::string e; if (d.open(path, e)) { fd = d.frameDuration(); dur = d.duration(); } }

    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "accept", nullptr, nullptr);
    glfwMakeContextCurrent(win);
    gladLoadGL((GLADloadfunc)glfwGetProcAddress);

    Transport transport;
    {
        VideoPlayerNode vp;
        vp.inputDefault(0) = path;
        vp.inputDefault(1) = rate;
        vp.initGL();
        auto start = clk::now();
        float dt = 1.0f / 60.0f;
        int frames = 0, steady = 0, onTime = 0, changes = 0; double lastShown = -1e9;
        double worst = 0.0, total = 0.0, maxLagFrames = 0.0, firstShownMs = -1.0, rssMax = 0.0;
        double worstAfterStall = 0.0, recoverMs = -1.0, stallEnd = -1.0, worstAt = 0.0, worstLate = 0.0;
        std::vector<double> evals;
        bool stalled = false;
        while (msSince(start) < wallSec * 1000.0) {
            auto f0 = clk::now();
            std::vector<Value> ins; for (auto& p : vp.inputs()) ins.push_back(p.defaultValue);
            std::vector<Value> outs(vp.outputs().size());
            EvalContext ctx{ins, outs, dt, &transport, nullptr, false};
            vp.evaluate(ctx);
            glFinish();                                             // include the upload + blit
            const double evalMs = msSince(f0);
            if (!stalled && stallAt >= 0 && msSince(start) >= stallAt * 1000.0) {
                std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(stallMs));
                stalled = true; stallEnd = msSince(start);
            }
            const double el = msSince(f0);
            if (el < 16.667) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(16.667 - el));
            dt = (float)(msSince(f0) / 1000.0);
            ++frames;
            if (vp.hasFrame()) {
                if (vp.shownFrameTime() != lastShown) { ++changes; lastShown = vp.shownFrameTime(); }
                if (firstShownMs < 0) firstShownMs = msSince(start);
                evals.push_back(evalMs);
                if (evalMs > worst) { worst = evalMs; worstAt = msSince(start) - firstShownMs; }
                if (msSince(start) - firstShownMs > 500.0) worstLate = std::max(worstLate, evalMs);
                total += evalMs;
                double d = vp.playhead() - vp.shownFrameTime();       // modular: the node reports wrapped
                if (dur > 0) { d = std::fmod(d, dur); if (d > dur / 2) d -= dur; if (d < -dur / 2) d += dur; }
                const double lag = std::fabs(d) / fd;
                if (std::getenv("LAGLOG") && lag > 3.0)
                    std::printf("  lag %.1f fr at wall %.2f s, playhead %.2f, shown %.2f\n", lag, msSince(start) / 1000.0, vp.playhead(), vp.shownFrameTime());
                if (msSince(start) - firstShownMs > 1000.0) {        // steady state: 1 s after the first frame
                    ++steady; if (lag <= 1.0 + 1e-6) ++onTime;
                    maxLagFrames = std::max(maxLagFrames, lag);
                }
                if (stallEnd >= 0 && msSince(start) > stallEnd) {
                    worstAfterStall = std::max(worstAfterStall, evalMs);
                    if (recoverMs < 0 && lag <= 1.0 + 1e-6) recoverMs = msSince(start) - stallEnd;
                }
            }
            rssMax = std::max(rssMax, rssMB());
        }
        std::sort(evals.begin(), evals.end());
        const double p99 = evals.empty() ? 0 : evals[(size_t)(evals.size() * 0.99)];
        std::printf("%-28s rate %+.1f: ui %.0f fps | first frame %.0f ms | eval mean %.1f p99 %.1f worst %.1f ms | on time %.1f%% (max lag %.1f fr) | rss %.0f MB",
                    path.substr(path.rfind('/') + 1).c_str(), rate, frames / wallSec, firstShownMs,
                    evals.empty() ? 0 : total / evals.size(), p99, worst,
                    steady ? 100.0 * onTime / steady : 0.0, maxLagFrames, rssMax);
        std::printf(" | picture changes %.1f/s", changes / wallSec);
        std::printf(" | worst at +%.0f ms, worst after 0.5 s %.1f ms", worstAt, worstLate);
        if (stallAt >= 0) std::printf(" | after %.0f ms stall: back in step in %.0f ms, worst eval %.1f ms", stallMs, recoverMs, worstAfterStall);
        std::printf("\n");
    }
    glfwDestroyWindow(win); glfwTerminate();
    return 0;
}
```

- [ ] **Step 3: Build it twice** — the app's Debug flags and `-O2` (`R` is the repository root):

```bash
R=/path/to/opengl-shader-streamer; B=$R/build
INC="-std=gnu++17 -DGLFW_INCLUDE_NONE -I$R/src -I$B/gladsources/glad_gl41/include -I$B/_deps/glfw-src/include -I$B/_deps/glm-src $(pkg-config --cflags libavformat)"
SRC="vp_accept.cpp $R/src/modules/VideoPlayerNode.cpp $R/src/gfx/VideoStream.cpp $R/src/gfx/VideoDecoder.cpp $R/src/core/Node.cpp"
LIB="$B/libglad_gl41.a $B/_deps/glfw-build/src/libglfw3.a $(pkg-config --libs libavformat libavcodec libswscale libswresample libavutil) -framework OpenGL -framework Cocoa -framework IOKit"
c++ -g $INC $SRC $LIB -o vp_accept_dbg && c++ -O2 -g $INC $SRC $LIB -o vp_accept_rel
```

- [ ] **Step 4: Forward 1× on every clip, both builds**

```bash
for b in rel dbg; do for v in v1080_gop30 v1080_gop250 v4k_gop30 v4k_gop250 v4k_hevc8 v4k_hevc10; do ./vp_accept_$b $v.mp4 8 1; done; done
```

Expected on every line: `on time 100.0%`, `ui 57`–`58 fps`, `worst after 0.5 s` ≤ 25 ms and `eval mean` < 10 ms at 4K (prototype: 15–22 ms and 7–9 ms; 1080p mean 2.2 ms), `rss` ≤ 1200 MB at 4K (0.88 GB H.264, 1.15 GB 10-bit HEVC).

- [ ] **Step 5: One 600 ms hitch at t = 3 s**

```bash
./vp_accept_rel v1080_gop250.mp4 10 1 3 600; ./vp_accept_rel v4k_gop250.mp4 10 1 3 600
```

Expected: `back in step in 0 ms` (well within the 0.5 s criterion; before this change the 1080p clip locked at ~1 fps).

- [ ] **Step 6: 2× and reverse**

```bash
for v in v4k_gop250 v4k_hevc8 v4k_hevc10; do ./vp_accept_rel $v.mp4 8 2; done
for v in v1080_gop30 v1080_gop250 v4k_gop30 v4k_gop250; do ./vp_accept_rel $v.mp4 8 -1; done
```

Expected: 2× H.264 4K `on time 100.0%`; 2× HEVC 4K `picture changes` ≥ 5/s (prototype 7–11 — the decoder cannot reach 60 fps, so it skips frames rather than freezing). Reverse: 1080p with 1 s keyframes `on time 100.0%`; the others step (max lag ≈ the stride: 8 frames at 1080p/8 s keyframes, 4 at 4K/1 s, ~32 at 4K/8 s).

- [ ] **Step 7: A long keyframe interval in MPEG-TS** — its index lists seek probes as keyframes, so a seek ahead can land far behind the decoder. Save the harness as `~/vidtest/vp_tsjump.cpp`:

```cpp
// Acceptance: a long keyframe interval in MPEG-TS. Reverse briefly (its seeks fill the demuxer's index
// with probe positions flagged as keyframes), forward again, then the playhead jumps 4 s ahead of the
// decoder: the picture must be back within 0.2 s of the playhead quickly, without seeking over and over.
// Drives gfx/VideoStream directly (no GL): ./vp_tsjump long1080.ts
#include "gfx/VideoStream.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
using namespace oss;
using clk = std::chrono::steady_clock;
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static double since(clk::time_point t0) { return std::chrono::duration<double>(clk::now() - t0).count(); }
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: vp_tsjump FILE\n"); return 2; }
    const std::string path = argv[1];
    VideoStream s(path);
    for (int i = 0; i < 500 && s.state() == VideoStream::State::Opening; ++i) sleepMs(10);
    if (s.state() != VideoStream::State::Ready) { std::fprintf(stderr, "open failed: %s\n", s.error().c_str()); return 1; }
    VideoStream::FrameView fv;
    VideoRequest r; r.loop = true; r.offline = false;
    r.u = 4.0; r.rate = -1.0f; s.request(r);                   // reverse at 4 s: stretches seek
    for (int i = 0; i < 100; ++i) { r.u -= 0.016; s.request(r); s.frameAt(r.u, fv); sleepMs(16); }
    r.rate = 1.0f; s.request(r);                               // forward again: a seek (direction change)
    for (int i = 0; i < 100; ++i) { r.u += 0.016; s.request(r); s.frameAt(r.u, fv); sleepMs(16); }
    const std::uint64_t seeks0 = s.seeks();
    const double jumpFrom = r.u;
    const auto t0 = clk::now();
    double caught = -1.0;
    while (since(t0) < 4.0) {                                  // the playhead jumps 4 s ahead, plays on
        r.u = jumpFrom + 4.0 + since(t0); s.request(r); s.frameAt(r.u, fv);
        if (caught < 0 && r.u - fv.t < 0.2) caught = since(t0);
        sleepMs(16);
    }
    std::printf("%s: jumped from %.2f; picture back within 0.2 s after %s; seeks during the 4 s: %llu\n", path.c_str(),
                jumpFrom, caught < 0 ? "NEVER" : (std::to_string(caught) + " s").c_str(),
                (unsigned long long)(s.seeks() - seeks0));
    return caught < 0 ? 1 : 0;
}
```

```bash
ffmpeg -y -f lavfi -i "testsrc2=size=1920x1080:rate=25" -t 30 -vf "noise=alls=10:allf=t" -c:v libx264 \
  -preset veryfast -g 250 -keyint_min 250 -sc_threshold 0 -b:v 8M -pix_fmt yuv420p -f mpegts long1080.ts
c++ -O2 -std=gnu++17 -I$R/src $(pkg-config --cflags libavformat) vp_tsjump.cpp $R/src/gfx/VideoStream.cpp \
  $R/src/gfx/VideoDecoder.cpp $(pkg-config --libs libavformat libavcodec libswscale libswresample libavutil) -o vp_tsjump
for i in 1 2 3 4 5; do ./vp_tsjump long1080.ts; done
```

Expected on every line: the picture back within 0.2 s after < 1 s, with at most 1 seek (prototype, 10 runs: 0.04–0.31 s, no seeks; a reviewer's 33 runs: median 0.055 s, worst 0.44 s; trusting the MPEG-TS index instead took 1.6–2.4 s with a seek landing 8 s back).

- [ ] **Step 8: ThreadSanitizer** — a separate build directory:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread -O1" \
  -DCMAKE_C_FLAGS="-fsanitize=thread -O1" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan --target gl_smoke -j8
TSAN_OPTIONS="halt_on_error=0" ./build-tsan/gl_smoke 2>&1 | grep -c "WARNING: ThreadSanitizer"
```

Expected: `0`, and `gl_smoke` exits 0. (FFmpeg itself is not instrumented; every hand-off between the worker and the graph thread is.)

- [ ] **Step 9: Try it in the app** — `./build/shader_streamer`, add a Video Player, pick a 4K file: the UI stays responsive while it opens and plays; the status line shows `opening...` then the position.

### Task 11: Finish the branch

- [ ] **Step 1: Full test run**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure
```

Expected: `100% tests passed out of 3`.

- [ ] **Step 2:** Use superpowers:finishing-a-development-branch to decide how to integrate `fix/video-player-ui-stall` (PR to `develop`; the three CI workflows build Linux/macOS/Windows).
