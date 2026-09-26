# Video Player Background Decoding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop large videos from freezing the UI by moving the Video Player's decoding onto a per-node background worker, while keeping offline renders frame-exact.

**Architecture:** Pure, unit-tested playback decisions (`core/VideoPlan.h`) and a time-tagged audio store (`core/TimedAudio.h`) drive a worker thread (`gfx/VideoStream`) that owns a reworked `VideoDecoder` (threaded decode, split decode/convert, audio read-ahead) and a fixed pool of RGBA frames. The node (`modules/VideoPlayerNode`) only advances an unwrapped playhead, posts it, and uploads the newest ready frame through a flipped blit.

**Tech Stack:** C++17, FFmpeg ≥ 5.1 (libavformat/libavcodec/libswscale/libswresample), OpenGL 4.1 core, doctest (`core_tests`), the headless GL harness (`gl_smoke`).

**Spec:** [`docs/superpowers/specs/2026-09-26-video-player-background-decode-design.md`](../specs/2026-09-26-video-player-background-decode-design.md) — read it first; it explains *why* each rule exists.

---

## Before you start

- Work on the branch `fix/video-player-ui-stall` (it already holds the spec). Never commit to `main`.
- Run every command from the repository root; `gl_smoke` resolves `shaders/` and `tests/assets/` relative to it.
- The build directory is `build/` (configured by `cmake -S . -B build`). Every code block below is complete — copy it exactly.
- This plan was generated from a prototype that passed all of `core_tests`, `gl_smoke` and `render_cli`, and a ThreadSanitizer build of `gl_smoke` with zero reports; each task's "verify it fails" and "verify it passes" outputs were checked by replaying the plan on a fresh copy of the repository.

## File structure

| File | Responsibility |
|---|---|
| `src/core/VideoPlan.h` (new) | Pure playback decisions: pool size, laps and the unwrapped playhead, frame selection, reverse stretches, the worker's next step |
| `src/core/TimedAudio.h` (new) | Audio chunks tagged with unwrapped time; sampling identical to the old `emitAudio` |
| `src/gfx/VideoDecoder.{h,cpp}` (rewrite) | FFmpeg: threaded decode, `decodeNext`/`convert`, packet queue + audio read-ahead, keyframe lookup; legacy `decodeFrame` unchanged |
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

`videoNextStep()` is the worker's whole decision table. Forward: seek when the target is behind everything held (unless the last seek was *pinned* because the target precedes the file's first frame); when the target is more than 2 frames ahead of the decoder, seek only if a keyframe lies between (the next lap's start counts) **and** the gap is over 1 s (a seek restarts FFmpeg's frame-threading pipeline), else catch up; wrap or wait at the lap's end; fill a free buffer; else wait. With loop off, a decoder left a lap behind the playhead (it fell behind while looping) seeks into the playhead's lap — `lapEnd` is the decoder's lap end whether looping or not. A seek ahead that landed at or behind the decoder (no index, or MPEG-TS's, which lists its seek probes as keyframes) is not repeated within the lap until the target has moved on by the gap it revealed (`videoNoSeekBelow`). Reverse: a fresh stretch when the run is new, the target fell below what is covered, or the playhead is *stranded* above it — stopped (paused or offline: no lead), or more than `kVideoRestartLeads` leads above — aimed `lead` below a moving playhead; prefetch the stretch below once a stretch's budget (`videoStretchBudget`, half the pool) is free; wait at the start of the clip with loop off. First, one line of Task 2's anchor test changes so it admits frames exactly as the worker's prefetch does (strictly below the stretch above) — it passes before and after.

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
    CHECK(videoNoSeekBelow(6.5, 2.56, 2.56) == 6.5 + 6.5 - 2.56);        // exactly at the head counts
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
    CHECK(videoNextStep(in).fresh);
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
    double nextKey     = 0.0;      // ...and this is the first keyframe in it after `head` (+inf: none -- an
                                   // index built as the file is read, FLV's or NUT's, may not have got there)
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
//  counts) and the jump is longer than kVideoSeekMinJump -- a seek restarts the decoder's
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
        const double key    = std::min(in.keyKnown ? in.nextKey : std::numeric_limits<double>::infinity(), in.lapEnd);
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

- [ ] **Step 5: Build and run the tests (38 test cases)**

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

The worker's audio, as chunks tagged with unwrapped time. `sample()` reproduces the node's old `emitAudio` mapping exactly; uncovered time is silence; where chunks overlap at a loop seam the later-starting chunk wins; `append(..., clipHi)` lets a reverse stretch stop at the start of the stretch above it.

- [ ] **Step 1: Write the failing tests** — `tests/test_timed_audio.cpp` (complete file)

```cpp
#include <doctest/doctest.h>
#include <vector>
#include "core/TimedAudio.h"

using namespace oss;

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

TEST_CASE("TimedAudio: where chunks overlap, the later-starting chunk wins (a loop seam)") {
    TimedAudio a(1000);
    std::vector<float> ones(2000, 1.0f), twos(2000, 2.0f);
    a.beginChunk(0.0); a.append(ones.data(), ones.size());       // [0, 2)
    a.beginChunk(1.5); a.append(twos.data(), twos.size());       // [1.5, 3.5)
    float v = 0.0f;
    a.sample(1.0, 1.0, &v, 1); CHECK(v == doctest::Approx(1.0f));
    a.sample(1.6, 1.6, &v, 1); CHECK(v == doctest::Approx(2.0f));
    a.sample(3.0, 3.0, &v, 1); CHECK(v == doctest::Approx(2.0f));
}

TEST_CASE("TimedAudio: append keeps only samples before clipHi") {
    TimedAudio a(1000);
    std::vector<float> s(100, 1.0f);
    a.beginChunk(0.0);
    a.append(s.data(), s.size(), 0.010);                          // 10 samples fit before 10 ms
    CHECK(a.size() == 10);
    a.append(s.data(), s.size(), 0.010);                          // already at the clip: nothing more
    CHECK(a.size() == 10);
    TimedAudio none;
    none.append(s.data(), s.size());                              // no chunk begun: no-op
    CHECK(none.size() == 0);
}

TEST_CASE("TimedAudio: retain drops chunks outside the window and trims fronts a second at a time") {
    TimedAudio a(1000);
    std::vector<float> s(5000, 1.0f);
    a.beginChunk(0.0);  a.append(s.data(), s.size());            // [0, 5)
    a.beginChunk(10.0); a.append(s.data(), s.size());            // [10, 15): current
    a.retain(6.0, 100.0);                                         // the first chunk ends before 6
    CHECK(a.size() == 5000);
    a.retain(10.5, 100.0);                                        // under a second into the chunk: kept
    CHECK(a.size() == 5000);
    a.retain(12.5, 100.0);                                        // 2.5 s in: trimmed
    CHECK(a.size() == 2500);
    float v = 0.0f;
    a.sample(13.0, 13.0, &v, 1); CHECK(v == doctest::Approx(1.0f));
}

TEST_CASE("TimedAudio: holds at most kMaxSeconds, dropping the oldest chunks first") {
    TimedAudio a(100);
    std::vector<float> s(2000, 1.0f);                             // 20 s per chunk at 100 Hz
    a.beginChunk(0.0);  a.append(s.data(), s.size());
    a.beginChunk(20.0); a.append(s.data(), s.size());             // 40 s held: the first chunk goes
    CHECK(a.size() == 2000);
    float v = 0.0f;
    a.sample(10.0, 10.0, &v, 1); CHECK(v == 0.0f);
    a.sample(30.0, 30.0, &v, 1); CHECK(v == doctest::Approx(1.0f));
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
#include <cstddef>
#include <limits>
#include <vector>

namespace oss {

// Decoded audio tagged with UNWRAPPED time (see core/VideoPlan.h), for the Video Player's worker.
// Audio is kept as contiguous chunks, each a start time plus mono samples at `rate`. A seek, a loop
// wrap or a reverse stretch begins a new chunk. sample() maps output samples onto source time exactly
// as the node's emitAudio always did (linear interpolation; a reversed span reads backwards). Times
// no chunk covers are silence, and where chunks overlap (a loop seam) the later-starting chunk wins.
// GL-free. Not thread-safe: VideoStream guards it with its mutex.
class TimedAudio {
public:
    static constexpr double kMaxSeconds = 30.0;   // safety cap on everything held

    explicit TimedAudio(int rate = 48000) : rate_(rate) {}

    int  rate() const { return rate_; }
    bool empty() const { return chunks_.empty(); }
    void clear() { chunks_.clear(); }

    // Start a new chunk at unwrapped time `startU`; append() adds to it.
    void beginChunk(double startU) { chunks_.push_back(Chunk{startU, {}}); }

    // Append to the current chunk, keeping only samples that start before `clipHi` (a reverse stretch
    // passes the start of the stretch above it, so the two never overlap). No-op with no chunk.
    void append(const float* s, std::size_t n,
                double clipHi = std::numeric_limits<double>::infinity()) {
        if (chunks_.empty() || n == 0) return;
        Chunk& c = chunks_.back();
        const double room = (clipHi - end(c)) * rate_;
        if (room <= 0.0) return;
        const std::size_t take = room < (double)n ? (std::size_t)room : n;
        c.s.insert(c.s.end(), s, s + take);
        capTotal();
    }

    // Drop audio lying entirely outside [lo, hi]. A chunk's front is trimmed only a whole second at a
    // time, so calling this every frame does not memmove the buffer every frame.
    void retain(double lo, double hi) {
        std::vector<Chunk> kept;
        kept.reserve(chunks_.size());
        for (std::size_t i = 0; i < chunks_.size(); ++i) {
            Chunk& c = chunks_[i];
            const bool current = i + 1 == chunks_.size();
            if (!current && (end(c) < lo || c.start > hi)) continue;
            if (lo > c.start + 1.0) {
                const std::size_t drop = std::min(c.s.size(), (std::size_t)((lo - c.start) * rate_));
                c.s.erase(c.s.begin(), c.s.begin() + (std::ptrdiff_t)drop);
                c.start += (double)drop / rate_;
            }
            kept.push_back(std::move(c));
        }
        chunks_.swap(kept);
    }

    // n output samples spanning source time [u0, u1] (u1 < u0 reads backwards).
    void sample(double u0, double u1, float* out, int n) const {
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

    float at(double t) const {
        const Chunk* best = nullptr;
        for (const Chunk& c : chunks_) {
            const double idx = (t - c.start) * rate_;
            if (idx >= 0.0 && idx < (double)c.s.size() - 1.0 && (!best || c.start > best->start)) best = &c;
        }
        if (!best) return 0.0f;
        const double idx = (t - best->start) * rate_;
        const std::size_t i = (std::size_t)idx;
        const float fr = (float)(idx - (double)i);
        return best->s[i] * (1.0f - fr) + best->s[i + 1] * fr;
    }

    // Drop the oldest chunks (never the current one) while more than kMaxSeconds is held.
    void capTotal() {
        const std::size_t cap = (std::size_t)(kMaxSeconds * rate_);
        while (chunks_.size() > 1 && size() > cap) chunks_.erase(chunks_.begin());
    }

    std::vector<Chunk> chunks_;
    int rate_;
};

} // namespace oss
```

- [ ] **Step 5: Build and run the tests (7 test cases)**

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

The worker must decode a frame, look at its time, and only then decide to convert it; convert with threads; and keep audio ahead of the video however few frames it buffers. So: `decodeNext()` returns a `DecodedFrame` (a counted reference, nothing copied); `convert()` writes TOP-DOWN RGBA with threaded swscale (the portable FFmpeg ≥ 5 path rejects a negative-stride destination, and FFmpeg 5–7 allocate a new buffer for a destination frame with none — so the caller's buffer is wrapped in a no-op-free `AVBufferRef`); video packets are queued (≤ 64 MB) so `pumpAudio()` can read audio ahead; `thread_count = 0`; an interrupt callback lets a stop abort blocking I/O. The legacy `decodeFrame()` keeps its exact behaviour (bottom-up, single-threaded conversion) — the 15 `gl_smoke` uses rely on it. Every time in and out counts from the first video frame, which `open()` decodes (and the first `decodeNext()` hands out): a container that starts its clock late (MPEG-TS) or shows a B-frame delay with no edit list (FLV, fragmented MP4) would otherwise put the first frame after the playhead's 0 — an offline render could never have its first frame. `seek(t <= 0)` goes to the very start of the file, since a timestamp search (MPEG-TS) can overshoot the first frame's time by a keyframe interval; audio from before the first frame is dropped. `open()` seeks to the start before decoding that first frame, because some demuxers read their keyframe index only when first asked to seek (Matroska and WebM cues) — without it the worker could not seek ahead in the first lap.

- [ ] **Step 1: Add `#include <cstring>` to `tests/gl_smoke.cpp`, after `#include <cstdlib>`**. In `tests/gl_smoke.cpp`, replace:

```cpp
#include <cstdlib>
```

with:

```cpp
#include <cstdlib>
#include <cstring>
```

- [ ] **Step 2: Add the failing scenario, just above `// --- Scenario 10: Video Player decodes a file to texture + audio ---`**:

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
        std::vector<float> s; double st = 0.0;
        if (!c.takeAudio(s, st) || s.size() < 48000 || c.audioSettledUpTo() < 1.0) {
            return failed("split decode: pumpAudio did not read a second of audio ahead of one video frame");
        }

        // A file whose video starts late -- an FLV with B-frames puts its first frame a frame in --
        // counts from that frame: it is at 0, and the keyframe index, the duration, seek() and the
        // audio count from it too.
        const std::string late = "build/_late_start.flv";
        {
            VideoEncoder enc;
            if (!enc.open(late, 64, 48, 25, 48000, 1, err)) { return failed(("late start: encode: " + err).c_str()); }
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
        }
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
        d.seek(0.0);
        DecodedFrame g;
        if (!d.decodeNext(g) || g.t != 0.0) { return failed("late start: seek(0) must return to the first frame"); }
        d.pumpAudio(1.0);
        std::vector<float> la; double laStart = -1.0;
        if (!d.takeAudio(la, laStart) || laStart < 0.0 || laStart > 0.05) {
            return failed("late start: the audio must count from the first frame");
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
        std::fprintf(stderr, "gl_smoke OK: decodeNext+convert == decodeFrame flipped; keyframe lookup; %.2f s of audio read ahead; "
                     "a late-starting file counts from its first frame; Matroska cues are read at open\n", s.size() / 48000.0);
    }
    return true;
}

```

- [ ] **Step 3: Register it in `kScenarios`, just above `scenario_video_player_decode,`**. In `tests/gl_smoke.cpp`, replace:

```cpp
    scenario_video_player_decode,
```

with:

```cpp
    scenario_video_decoder_split_decode,
    scenario_video_player_decode,
```

- [ ] **Step 4: Build to verify it fails**

```bash
cmake --build build --target gl_smoke -j8
```

Expected: the build FAILS, with an error mentioning `no member named 'decodeNext'`.

- [ ] **Step 5: Replace `src/gfx/VideoDecoder.h`** — `src/gfx/VideoDecoder.h` (complete file)

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
// playhead starts at 0. Audio from before the first frame is dropped.
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // Open `path`. Returns false and fills `err` on failure (bad path, no video
    // stream, unsupported codec). A file with no audio stream still opens. `abort` (optional)
    // is polled by FFmpeg during blocking I/O: once it reads true, opening or reading gives up
    // promptly (VideoStream uses it to stop its worker). Decodes the first frame, to learn when
    // it is; the first decodeNext() hands it out.
    bool open(const std::string& path, std::string& err, const std::atomic<bool>* abort = nullptr);

    bool   isOpen()   const { return fmt_ != nullptr; }
    int    width()    const { return width_; }
    int    height()   const { return height_; }
    double duration() const { return duration_; }      // seconds (0 if unknown)
    bool   hasAudio() const { return astream_ >= 0; }
    int    audioRate() const { return kOutRate; }      // we always resample to this
    int    audioChannels() const { return audioChannels_; }  // source channels (before our mono downmix)
    static constexpr int kOutRate = 48000;             // 48 kHz mono float out

    // Nominal seconds per frame, from the stream's average (else real) frame rate; 1/30 if unknown.
    double frameDuration() const;

    // The first keyframe after time `t`, from the container's index. `key` is +inf when the
    // index holds none after `t`. False when the stream has no index (yet).
    bool nextKeyframeAfter(double t, double& key) const;

    // Seek so the next decodeNext() resumes at the keyframe at or before time
    // `t` (seconds; at or before 0, the start of the file). Flushes the decoders, the queued
    // packets and the pending audio, so nothing stale leaks across.
    void seek(double t);

    // Decode the next video frame in source order WITHOUT converting it. Audio met on the way is
    // decoded into the pending buffer (see takeAudio). False at the end of the stream.
    bool decodeNext(DecodedFrame& out);

    // Convert a decoded frame to RGBA8 -- rows TOP-DOWN, `dstStride` bytes apart -- with threaded
    // swscale. False on failure (e.g. the frame's size changed mid-stream).
    bool convert(const DecodedFrame& f, std::uint8_t* dst, int dstStride);

    // Read ahead -- queueing video packets instead of decoding them -- until the audio is settled up
    // to time `t` (see audioSettledUpTo), the input ends, or kMaxQueuedBytes of video packets
    // are waiting.
    void pumpAudio(double t);

    // The time up to which no more audio will arrive: the end of the decoded audio, or -- once
    // the demuxer has read kAudioSettleSlack past a point without meeting audio for it -- that
    // point. +inf at the end of the input or with no audio stream.
    double audioSettledUpTo() const;

    // Move out the audio decoded since the last call (48 kHz mono float); `startT` is the time of
    // its first sample. False when there is none.
    bool takeAudio(std::vector<float>& out, double& startT);

    // Decode the next video frame in source order into `out`, converted to bottom-up RGBA. Any
    // audio decoded on the way (audio packets interleaved before this video frame) is appended
    // to `audio` as 48 kHz mono float. The first time audio is appended into a freshly-cleared
    // `audio`, `audioStartT` is set to that audio's time and `audioStartValid` to true
    // (left untouched on later calls so a multi-call fill keeps one contiguous timeline).
    // Returns false at end of stream.
    bool decodeFrame(VideoFrame& out, std::vector<float>& audio,
                     double& audioStartT, bool& audioStartValid);

    static constexpr std::size_t kMaxQueuedBytes   = std::size_t(64) << 20;  // read-ahead cap
    static constexpr double      kAudioSettleSlack = 2.0;                    // seconds

private:
    void close();
    void resetStreamState();
    void clearQueue();
    bool readPacket();
    void drainAudio();

    AVFormatContext* fmt_     = nullptr;
    AVCodecContext*  vctx_    = nullptr;   // video decoder
    AVCodecContext*  actx_    = nullptr;   // audio decoder (null if no audio)
    SwsContext*      sws_     = nullptr;   // decodeFrame(): -> RGBA, single-threaded, bottom-up
    SwsContext*      swsThr_  = nullptr;   // convert(): -> RGBA, threaded, top-down
    int              swsThrFmt_ = -1;      // the pixel format swsThr_ was built for
    SwrContext*      swr_     = nullptr;   // -> 48 kHz mono float
    AVFrame*         frame_   = nullptr;   // reused decode target (video or audio)
    AVFrame*         dstFrame_ = nullptr;  // convert()'s destination wrapper
    AVPacket*        pkt_     = nullptr;

    std::deque<AVPacket*> vq_;             // video packets read ahead, not yet decoded
    std::size_t           queuedBytes_ = 0;

    int    vstream_ = -1;
    int    astream_ = -1;
    int    width_   = 0;
    int    height_  = 0;
    double duration_   = 0.0;
    int    audioChannels_ = 0;  // source audio channel count (0 if no audio)
    double vTimeBase_  = 0.0;   // seconds per video stream tick
    double startT_     = 0.0;   // the container's time of the first video frame: subtracted from
                                // every time handed out, added to every time taken in
    DecodedFrame first_;        // decoded by open() to find startT_; the first decodeNext() returns it
    double aTimeBase_  = 0.0;   // seconds per audio stream tick
    bool   demuxEof_   = false; // read the last packet (and flushed the audio decoder)
    bool   vflushed_   = false; // sent the video decoder its end of stream
    double demuxedT_   = -std::numeric_limits<double>::infinity();   // latest packet time read (container time)
    double audioEndT_  = -std::numeric_limits<double>::infinity();   // end of the decoded audio (container time)

    std::vector<float>        pendingAudio_;          // decoded since the last takeAudio()
    double                    pendingAudioStart_ = 0.0;   // container time
    std::vector<std::uint8_t> rgba_;                  // decodeFrame()'s RGBA target
    std::vector<float>        aScratch_;              // reused swr output scratch
    std::vector<float>        legacyAudio_;           // decodeFrame()'s takeAudio scratch
};

} // namespace oss
```

- [ ] **Step 6: Replace `src/gfx/VideoDecoder.cpp`** — `src/gfx/VideoDecoder.cpp` (complete file)

```cpp
#include "gfx/VideoDecoder.h"
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace oss {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

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
    if (vctx_)     avcodec_free_context(&vctx_);
    if (actx_)     avcodec_free_context(&actx_);
    if (pkt_)      av_packet_free(&pkt_);
    if (frame_)    av_frame_free(&frame_);
    if (dstFrame_) av_frame_free(&dstFrame_);
    if (fmt_)      avformat_close_input(&fmt_);
    vstream_ = astream_ = -1;
    width_ = height_ = 0;
    audioChannels_ = 0;
    duration_ = vTimeBase_ = aTimeBase_ = startT_ = 0.0;
    resetStreamState();
}

void VideoDecoder::resetStreamState() {
    demuxEof_ = vflushed_ = false;
    demuxedT_ = audioEndT_ = -kInf;
    pendingAudio_.clear();
    pendingAudioStart_ = 0.0;
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

    vstream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vstream_ < 0) { err = "no video stream"; close(); return false; }
    astream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);  // may be < 0

    // --- Video decoder ---
    AVStream* vs = fmt_->streams[vstream_];
    const AVCodec* vcodec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!vcodec) { err = "unsupported video codec"; close(); return false; }
    vctx_ = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(vctx_, vs->codecpar);
    vctx_->thread_count = 0;   // automatic: a decode thread per core (libavcodec defaults to one)
    if (avcodec_open2(vctx_, vcodec, nullptr) < 0) {
        err = "could not open video decoder"; close(); return false;
    }
    width_     = vctx_->width;
    height_    = vctx_->height;
    vTimeBase_ = av_q2d(vs->time_base);
    if (width_ <= 0 || height_ <= 0) { err = "video has no dimensions"; close(); return false; }

    sws_ = sws_getContext(width_, height_, vctx_->pix_fmt,
                          width_, height_, AV_PIX_FMT_RGBA,
                          SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) { err = "could not init colour converter"; close(); return false; }
    rgba_.assign((std::size_t)width_ * height_ * 4, 0);

    // --- Audio decoder (optional; swr is set up lazily on the first frame so it
    //     matches the decoder's real output format) ---
    if (astream_ >= 0) {
        AVStream* as = fmt_->streams[astream_];
        const AVCodec* acodec = avcodec_find_decoder(as->codecpar->codec_id);
        if (acodec) {
            actx_ = avcodec_alloc_context3(acodec);
            avcodec_parameters_to_context(actx_, as->codecpar);
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
    if (decodeNext(f)) {
        startT_ = f.t;
        f.t = 0.0;
        first_ = std::move(f);
        const double clockStart = fmt_->start_time != AV_NOPTS_VALUE ? (double)fmt_->start_time / AV_TIME_BASE : 0.0;
        if (duration_ > 0.0) duration_ = std::max(0.0, duration_ - (startT_ - clockStart));
    }
    return true;
}

double VideoDecoder::frameDuration() const {
    if (!fmt_ || vstream_ < 0) return 1.0 / 30.0;
    const AVStream* vs = fmt_->streams[vstream_];
    AVRational r = vs->avg_frame_rate;
    if (r.num <= 0 || r.den <= 0) r = vs->r_frame_rate;
    if (r.num <= 0 || r.den <= 0) return 1.0 / 30.0;
    return (double)r.den / (double)r.num;
}

bool VideoDecoder::nextKeyframeAfter(double t, double& key) const {
    if (!fmt_ || vstream_ < 0 || vTimeBase_ <= 0.0) return false;
    AVStream* vs = fmt_->streams[vstream_];
    if (avformat_index_get_entries_count(vs) <= 0) return false;
    const int64_t ts = (int64_t)std::floor((t + startT_) / vTimeBase_) + 1;   // strictly after t
    const AVIndexEntry* e = avformat_index_get_entry_from_timestamp(vs, ts, 0);   // keyframe, >= ts
    key = e ? e->timestamp * vTimeBase_ - startT_ : kInf;
    return true;
}

void VideoDecoder::seek(double t) {
    if (!fmt_) return;
    first_.reset();
    // At or before 0, the very start of the file rather than the first frame's time: a demuxer that
    // searches by timestamp (MPEG-TS) can overshoot that by a keyframe interval.
    const double src = t > 0.0 ? t + startT_ : 0.0;
    int64_t ts = (int64_t)(src / (vTimeBase_ > 0 ? vTimeBase_ : 1.0));
    // BACKWARD lands on the keyframe at or before ts -- exactly the GOP start the
    // caller decodes forward from. If that fails (e.g. a stream it cannot seek), restart
    // from the beginning: the caller then decodes forward to its target -- slow, but right.
    if (av_seek_frame(fmt_, vstream_, ts, AVSEEK_FLAG_BACKWARD) < 0)
        av_seek_frame(fmt_, vstream_, 0, AVSEEK_FLAG_BACKWARD);
    if (vctx_) avcodec_flush_buffers(vctx_);
    if (actx_) avcodec_flush_buffers(actx_);
    clearQueue();
    resetStreamState();
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
    if (ts != AV_NOPTS_VALUE) demuxedT_ = std::max(demuxedT_, ts * av_q2d(st->time_base));
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
// append it to the pending buffer, noting the source time of the buffer's first sample.
void VideoDecoder::drainAudio() {
    if (!actx_) return;
    while (avcodec_receive_frame(actx_, frame_) == 0) {
        if (!swr_) {
            AVChannelLayout outLayout; av_channel_layout_default(&outLayout, 1);  // mono
            AVChannelLayout inLayout;
            if (frame_->ch_layout.nb_channels > 0) av_channel_layout_copy(&inLayout, &frame_->ch_layout);
            else                                   av_channel_layout_default(&inLayout, 1);
            int rc = swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_FLT, kOutRate,
                                         &inLayout, (AVSampleFormat)frame_->format,
                                         frame_->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&outLayout);
            av_channel_layout_uninit(&inLayout);
            if (rc < 0 || !swr_ || swr_init(swr_) < 0) {
                if (swr_) swr_free(&swr_);
                av_frame_unref(frame_);
                continue;   // can't resample this frame; skip it
            }
        }
        const double start = frame_->pts != AV_NOPTS_VALUE ? frame_->pts * aTimeBase_
                           : (std::isfinite(audioEndT_) ? audioEndT_ : 0.0);
        int outCount = swr_get_out_samples(swr_, frame_->nb_samples);
        if (outCount > 0) {
            if ((int)aScratch_.size() < outCount) aScratch_.resize(outCount);
            uint8_t* outptr = (uint8_t*)aScratch_.data();
            int got = swr_convert(swr_, &outptr, outCount,
                                  (const uint8_t**)frame_->extended_data, frame_->nb_samples);
            if (got > 0) {
                if (pendingAudio_.empty()) pendingAudioStart_ = start;
                pendingAudio_.insert(pendingAudio_.end(), aScratch_.data(), aScratch_.data() + got);
                audioEndT_ = start + (double)got / kOutRate;
            }
        }
        av_frame_unref(frame_);
    }
}

void VideoDecoder::pumpAudio(double t) {
    if (!fmt_ || !actx_) return;
    while (audioSettledUpTo() < t && queuedBytes_ < kMaxQueuedBytes && readPacket()) {}
}

double VideoDecoder::audioSettledUpTo() const {
    if (!actx_ || demuxEof_) return kInf;
    return std::max(audioEndT_, demuxedT_ - kAudioSettleSlack) - startT_;
}

bool VideoDecoder::takeAudio(std::vector<float>& out, double& startT) {
    out.clear();
    if (pendingAudio_.empty()) return false;
    out.swap(pendingAudio_);
    startT = pendingAudioStart_ - startT_;
    if (startT < 0.0) {                            // from before the first frame: the clip starts later
        const std::size_t skip = std::min(out.size(), (std::size_t)std::llround(-startT * kOutRate));
        out.erase(out.begin(), out.begin() + (std::ptrdiff_t)skip);
        startT = std::max(0.0, startT + (double)skip / kOutRate);
    }
    return !out.empty();
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
            out.t = ((ts != AV_NOPTS_VALUE) ? ts * vTimeBase_ : 0.0) - startT_;
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
        avcodec_send_packet(vctx_, p);
        av_packet_free(&p);
    }
}

bool VideoDecoder::convert(const DecodedFrame& f, std::uint8_t* dst, int dstStride) {
    const AVFrame* src = f.frame_;
    if (!src || !dst || src->width != width_ || src->height != height_) return false;
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
    if (takeAudio(legacyAudio_, start)) {
        if (!audioStartValid) { audioStartT = start; audioStartValid = true; }
        audio.insert(audio.end(), legacyAudio_.begin(), legacyAudio_.end());
    }
    if (!ok) return false;

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

- [ ] **Step 7: Build and run the scenario**

```bash
cmake --build build --target gl_smoke -j8 && ./build/gl_smoke 2>&1 | grep -E 'decodeNext|FAIL'
```

Expected output includes: `decodeNext+convert == decodeFrame flipped`

- [ ] **Step 8: Run everything: the old Video Player still builds on the legacy API and every existing scenario (encoder round-trips, offline renders) still passes**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure
```

Expected output includes: `100% tests passed`

- [ ] **Step 9: Commit**

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

The regression clips need keyframes far apart (x264's default of 250 frames is what exposed the lock-up). A trailing defaulted parameter keeps every existing caller unchanged; x264's scene-cut keyframes are switched off only when an interval is asked for.

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
            return failed("keyint: expected keyframes exactly every 2 s (50 frames at 25 fps)");
        }
        std::fprintf(stderr, "gl_smoke OK: an explicit keyframe interval places keyframes exactly (%.2f s, %.2f s)\n", k1, k2);
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
    // a keyframe exactly every that many frames (scene-cut keyframes off -- tests use it
    // to write clips with widely spaced keyframes); 0 keeps one per second. Returns
    // false on failure.
    bool open(const std::string& path, int width, int height, int fps,
              int audioRate, int audioChannels, std::string& err, int keyframeInterval = 0);
```

- [ ] **Step 5: In `src/gfx/VideoEncoder.cpp`, the definition's signature**. In `src/gfx/VideoEncoder.cpp`, replace:

```cpp
bool VideoEncoder::open(const std::string& path, int width, int height, int fps,
                        int audioRate, int audioChannels, std::string& err) {
```

with:

```cpp
bool VideoEncoder::open(const std::string& path, int width, int height, int fps,
                        int audioRate, int audioChannels, std::string& err, int keyframeInterval) {
```

- [ ] **Step 6: In `src/gfx/VideoEncoder.cpp`, the GOP size**. In `src/gfx/VideoEncoder.cpp`, replace:

```cpp
    vctx_->gop_size  = fps;
```

with:

```cpp
    vctx_->gop_size  = keyframeInterval > 0 ? keyframeInterval : fps;
```

- [ ] **Step 7: In `src/gfx/VideoEncoder.cpp`, after the x264 `crf` option**. In `src/gfx/VideoEncoder.cpp`, replace:

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
```

- [ ] **Step 8: Build and run the scenario**

```bash
cmake --build build --target gl_smoke -j8 && ./build/gl_smoke 2>&1 | grep -E 'keyframe interval|FAIL'
```

Expected output includes: `an explicit keyframe interval places keyframes exactly`

- [ ] **Step 9: Commit**

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

One worker thread per file. It opens the file, sizes a pool of RGBA buffers from the 512 MB budget, then loops: snapshot the request, recycle frames that can never be shown, read audio ahead, ask `videoNextStep()` what to do, and do it — never holding the mutex while decoding or converting. The graph thread only calls `request()`, `frameAt()` (the frame it returns is *checked out* until the next call), `readAudio()`, and offline `frameReadyFor()` / `waitForFrame()`. Details that the prototype showed matter: live catch-up is sliced to 100 ms so a decoder slower than the playhead still moves the picture; a seek keeps the newest frame at or before its target; a seek lands where its decode loop admits frames, retrying further back (1 s, 2 s, 4 s… to the file's start) when it lands late — a decode-time index lands a seek just below a keyframe ON it, a timestamp search (MPEG-TS) overshoots a keyframe interval, and some demuxers find nothing near the end — and is *pinned* only if even the start lands late; reverse stretches are published whole (they decode forwards); offline, a stretch whose ring evicted its lower frames covers only down to its oldest; a frame at or past the duration ends the lap. With loop off, frames past the playhead's lap are kept (they are the next ones if loop comes back on) and what reverse covered below the lap is dropped with its frames; runs end at the lap's end whether looping or not. An offline render that starts in reverse restarts the run — live stretches kept every stride-th frame — and until an offline stretch lands, reverse readiness says no. The new scenarios write the awkward files themselves — a long first keyframe interval, an FLV with B-frames, a one-keyframe clip for loop toggles — and count reverse stretches (`reverseStretches()`; `seeks()` is its forward twin) to prove live reverse cannot spin.

- [ ] **Step 1: Add `#include "gfx/VideoStream.h"` to `tests/gl_smoke.cpp`, after `#include "gfx/VideoDecoder.h"`**. In `tests/gl_smoke.cpp`, replace:

```cpp
#include "gfx/VideoDecoder.h"
```

with:

```cpp
#include "gfx/VideoDecoder.h"
#include "gfx/VideoStream.h"
```

- [ ] **Step 2: Add a timing helper, the indexed test clip (each frame paints its own number), and the three failing scenarios, just above `// --- Scenario 10: Video Player decodes a file to texture + audio ---`**:

```cpp
// Seconds since t0.
static double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// A clip whose every frame paints its own index as 9 horizontal black/white bands (the top band is
// bit 8), 160x90 at 25 fps with a 440 Hz tone, and keyframes exactly `gop` frames apart. Reading the
// bands back says which frame is on screen -- and a wrong vertical flip reads a different number.
static const int kIdxW = 160, kIdxH = 90, kIdxFps = 25;
static bool writeIndexedClip(const std::string& path, int frames, int gop, int w = kIdxW, int h = kIdxH) {
    VideoEncoder enc; std::string err;
    if (!enc.open(path, w, h, kIdxFps, 48000, 1, err, gop)) {
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
        const std::string gop = "build/_rev_long_gop.mp4", flv = "build/_rev_bframes.flv";
        if (!writeIndexedClip(gop, 60, 60) || !writeIndexedClip(flv, 100, 25)) {
            return failed("reverse files: could not write the clips");
        }
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
        double u = 1.2;
        VideoStream::FrameView fv;
        const auto t0 = std::chrono::steady_clock::now();
        auto last = t0;
        while (secondsSince(t0) < 2.5) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            const auto now = std::chrono::steady_clock::now();
            u -= std::chrono::duration<double>(now - last).count();
            last = now;
            VideoRequest r;
            r.u = u; r.rate = -1.0f; r.loop = true;
            s.request(r);
            s.frameAt(u, fv);
        }
        const unsigned long long live = (unsigned long long)s.reverseStretches();
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
        std::fprintf(stderr, "gl_smoke OK: reverse stays exact through a long first keyframe interval and across an FLV's "
                     "seam, and offline straight after live; live it does not spin (%llu stretches in 2.5 s); paused it settles\n", live);
    }
    return true;
}

// --- Scenario: VideoStream -- toggling loop ---
// Loop is a live control, and the worker must follow it wherever the decoder happens to be:
//  (a) loop off with the playhead a lap ahead of the decoder (a hitch, or a decoder slower than the
//      playhead) pins the playhead's lap: the worker must move there -- it used to wait at the end of the
//      decoder's lap for ever, and offline report the earlier lap's frames ready;
//  (b) loop off and back on near the end of the lap: the next lap's frames, decoded ahead, stay -- they
//      were recycled, and the worker went on filling from past them, leaving a hole;
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
            drive(s, r, 0.3);                                     // decoded ahead through the wrap
            r.u = 3.5; r.loop = false; r.lapLo = 0.0; r.lapHi = D;
            drive(s, r, 0.2);
            r.u = D + 0.02; r.loop = true; r.lapLo = -std::numeric_limits<double>::infinity();
            r.lapHi = std::numeric_limits<double>::infinity();
            if (!exactFrom(s, r, 10, D, "loop off and on at the lap's end")) {
                return failed("loop toggles: loop off and back on must keep the next lap's frames");
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

    explicit VideoStream(std::string path, std::size_t poolBytes = kVideoPoolBytes);
    ~VideoStream();                        // stops and joins the worker
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

    // Offline: the frame for u is decided (no undecoded frame can fall between it and u) and held,
    // and -- playing forward -- no more audio can arrive for times up to u. True once Failed.
    bool frameReadyFor(double u) const;
    bool waitForFrame(double u, double timeoutSeconds);

    // n output samples spanning source time [u0, u1] (see TimedAudio::sample).
    void readAudio(double u0, double u1, float* out, int n) const;

    // Diagnostics: seeks made and reverse stretches decoded so far. Both grow with the ground the
    // playhead covers; a step that achieved nothing and was planned again would spin the worker, and
    // show here.
    std::uint64_t seeks() const            { return seeks_.load(); }
    std::uint64_t reverseStretches() const { return reverseStretches_.load(); }

private:
    struct Slot { double t = 0.0; std::uint64_t serial = 0; int buf = -1; };

    // Worker thread.
    void run();
    void step();
    void waitForWork();
    bool peekNext();
    void takeAudio();
    void pumpAudio(double u);
    bool seekLanding(double end, double slack);
    void seekTo(double target);
    void catchUp(double target);
    void fill();
    void wrap();
    void reverseStretch(double to, bool fresh, bool offline);
    bool publish(DecodedFrame& f, double t, bool runStart);
    bool publishSlot(DecodedFrame& f, double t);
    bool directionChanged() const;
    void setFailed(const std::string& msg);

    // Under m_.
    int  readyFrameLocked(double u) const;   // index in ready_ of the frame for u, or -1
    bool readyLocked(double u) const;
    int  acquireLocked();
    void releaseLocked(int buf);
    void insertReadyLocked(const Slot& s);
    void flushReadyLocked();
    void recycleLocked(const VideoRequest& r, int dir);
    void setRunHiLocked();

    const std::string path_;
    const std::size_t poolBytes_;
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
    int           planDir_ = 1;            // the direction the worker last planned for...
    bool          planOffline_ = false;    // ...and whether for an offline render
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
#include <exception>
#include <new>

namespace oss {

namespace { constexpr double kInf = std::numeric_limits<double>::infinity(); }

VideoStream::VideoStream(std::string path, std::size_t poolBytes)
    : path_(std::move(path)), poolBytes_(poolBytes), audio_(VideoDecoder::kOutRate) {
    thread_ = std::thread([this] { run(); });
}

VideoStream::~VideoStream() {
    { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();   // bounded: the worker checks stop_ between frames,
}                                               // and FFmpeg's I/O polls it while blocked

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
    if (dir_ < 0 && !runOffline_) return false;               // live stretches skipped frames
    if (u < runLo_ - kVideoTimeEps || u >= runHi_ - kVideoTimeEps) return false;
    const bool held = (shown_.buf >= 0 && shown_.t <= u + kVideoTimeEps) || readyFrameLocked(u) >= 0;
    if (!held) return false;
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
// short of them, and they are the next ones to show if loop comes back on.
void VideoStream::recycleLocked(const VideoRequest& r, int dir) {
    double frameT = (shown_.buf >= 0 && shown_.t <= r.u + kVideoTimeEps) ? shown_.t : -kInf;
    const int best = readyFrameLocked(r.u);
    if (best >= 0) frameT = std::max(frameT, ready_[(std::size_t)best].t);
    for (std::size_t i = 0; i < ready_.size();) {
        const Slot& s = ready_[i];
        bool drop = dir >= 0 ? (std::isfinite(frameT) && s.t < frameT)
                             : (std::isfinite(frameT) ? s.t > frameT : s.t > r.u + kVideoTimeEps);
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

// --- worker ---------------------------------------------------------------------------------

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
    bool changed = false, toOffline = false;
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
        recycleLocked(r, dir);
        if (!r.loop && runValid_ && runLo_ < r.lapLo) {    // what was decided before the lap is gone
            if (runHi_ <= r.lapLo) runValid_ = false;
            else                   runLo_ = r.lapLo;
        }
        if (dir >= 0) audio_.retain(r.u - kVideoAudioKeep, kInf);
        else          audio_.retain(-kInf, r.u + kVideoAudioKeep);
        in.lowest = kInf;
        if (shown_.buf >= 0) in.lowest = shown_.t;
        if (!ready_.empty()) in.lowest = std::min(in.lowest, ready_.front().t);
        in.freeBuffers = (int)free_.size();
        in.poolSize    = (int)pool_.size();
    }
    if (changed) planDir_ = dir;
    if (changed || toOffline) coverValid_ = false;
    planOffline_ = r.offline;
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
    if (!std::isfinite(in.lowest)) in.lowest = in.head;
    in.seekPinned = seekPinned_;
    in.pinnedFrom = pinnedFrom_;
    in.noSeekBelow = noSeekBelow_;
    in.coverValid = coverValid_;
    in.coverLo    = coverLo_;
    in.coverHi    = coverHi_;
    in.lead       = r.offline ? 0.0 : std::min(2.0, std::fabs((double)r.rate) * lastStretchSeconds_);

    if (dir >= 0) pumpAudio(r.u);                               // keep the audio ahead of the playhead

    const VideoStep s = videoNextStep(in);
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

// Move the decoder's pending audio into the store (a new chunk after a seek or wrap), clipped below
// audioClipHi_, and publish how far the audio is settled.
void VideoStream::takeAudio() {
    double start = 0.0;
    const bool got = dec_.takeAudio(audioTmp_, start);
    const double settled = dec_.audioSettledUpTo();
    {
        std::lock_guard<std::mutex> lk(m_);
        if (got) {
            if (!audioChunkOpen_) { audio_.beginChunk(lapOffset_ + start); audioChunkOpen_ = true; }
            audio_.append(audioTmp_.data(), audioTmp_.size(), audioClipHi_);
        }
        audioSettledU_ = lapOffset_ + settled;
        audioLapStart_ = lapOffset_;
    }
    cv_.notify_all();
}

void VideoStream::pumpAudio(double u) {
    if (!winfo_.hasAudio) return;
    dec_.pumpAudio(u + kVideoAudioLead - lapOffset_);
    takeAudio();
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
        dec_.seek(at);
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
        pumpAudio(target);
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
    dec_.seek(0.0);
    next_.reset();
    eof_ = false;
    audioChunkOpen_ = false;
    lastT_ = lapOffset_ - winfo_.frameDur;
    std::lock_guard<std::mutex> lk(m_);
    audioSettledU_ = -kInf;
    audioLapStart_ = lapOffset_;                   // the previous lap's audio is complete
}

// Convert `f` into a free buffer and queue it. False if no buffer is free or conversion fails.
bool VideoStream::publishSlot(DecodedFrame& f, double t) {
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
        insertReadyLocked(Slot{t, nextSerial_++, b});
    }
    cv_.notify_all();
    return true;
}

// Forward: queue the frame and extend the run of consecutive decided frames.
bool VideoStream::publish(DecodedFrame& f, double t, bool runStart) {
    if (!publishSlot(f, t)) return false;
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
    auto keep = [&](DecodedFrame& f, double t, bool evict) {
        int b = -1;
        if (!evict || (int)ring.size() < cap) { std::lock_guard<std::mutex> lk(m_); b = acquireLocked(); }
        if (b < 0 && evict && !ring.empty()) { b = ring.front().buf; ring.erase(ring.begin()); }
        if (b < 0) return;                         // live: no room for this one -- skip it
        if (dec_.convert(f, pool_[(std::size_t)b].get(), winfo_.width * 4)) {
            ring.push_back(Slot{t, 0, b});
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
    } else {
        // Live strides count back from half a frame below the stretch above, on the keyframe's grid, so a
        // frame a container rounded (by up to half a frame) still counts from the right place.
        const double key = lapOffset_ + next_.t;
        const VideoStretch plan = videoPlanStretch(key, fresh ? to : to - 0.5 * fd, fd, cap, offline);
        cap = plan.keep;
        DecodedFrame spare;                        // live: the newest frame not kept, in case none is
        double spareT = 0.0;
        while (!stop_ && peekNext()) {
            const double t = lapOffset_ + next_.t;
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
            }
            if (directionChanged()) {              // abandon the stretch; the next step re-plans
                std::lock_guard<std::mutex> lk(m_);
                for (const Slot& s : ring) releaseLocked(s.buf);
                audioClipHi_ = kInf;
                return;
            }
        }
        if (!plan.contiguous && ring.empty() && spare.valid()) keep(spare, spareT, false);   // never empty
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
        for (const Slot& s : ring) insertReadyLocked(Slot{s.t, nextSerial_++, s.buf});
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
cmake --build build --target gl_smoke -j8 && ./build/gl_smoke 2>&1 | grep -E 'VideoStream|reverse stays|loop off follows|FAIL'
```

Expected output includes: `loop off follows a playhead a lap ahead`

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

The node keeps its ports and semantics but no longer decodes. Each frame: advance the unwrapped playhead (`videoAdvance`; held at the start until the first frame is on screen), post it to the stream, upload the newest ready frame at or before it into a staging texture and flip it into the published texture with one `glBlitFramebuffer`, and read the matching audio. Offline, it waits for the exact frame and prefetches the next one, reporting it through `loading()` so the renderer's gate does the waiting. The existing scenario must now drive the node the way the renderer does (offline + gated on `loading()`), plus a live check that polls; four new scenarios pin the fix.

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

        // (c) reverse: a negative rate walks the playhead backwards, forcing the
        // window to re-seek to an earlier keyframe and rebuild.
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
        // (d) live: the picture arrives from the worker without any evaluate() waiting for it.
        Graph g;
        auto vid = std::make_unique<VideoPlayerNode>();
        vid->inputDefault(0) = std::string("tests/assets/test.mp4");
        auto out = std::make_unique<OutputNode>();
        vid->initGL(); out->initGL();
        int vId = g.addNode(std::move(vid));
        int oId = g.addNode(std::move(out));
        if (!g.connect(vId, 0, oId, 0)) { return failed("live video: connect"); }
        auto* outNode = dynamic_cast<OutputNode*>(g.findNode(oId));
        const auto t0 = std::chrono::steady_clock::now();
        bool sawColour = false;
        while (!sawColour && secondsSince(t0) < 5.0) {
            g.evaluate(1.0f / 60.0f);
            TexRef t = outNode->current();
            if (t.id) { int r, gg, b, a; readCentre(t, r, gg, b, a); sawColour = r > 30 || gg > 30 || b > 30; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!sawColour) { return failed("live video: no picture within 5 s"); }
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
// must neither block evaluate() nor stop the picture catching up.
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
        std::fprintf(stderr, "gl_smoke OK: a 3 s hitch mid-keyframe-interval: evaluate %.1f ms, caught up to frame %d in %.0f ms (worst frame %.1f ms)\n",
                     hitchMs, got, secondsSince(t0) * 1000.0, worstMs);
    }
    return true;
}

// Render `frames` frames of a VideoPlayer -> Output (+ Audio Out) graph through the real
// OfflineRenderer at the indexed clip's size and rate, then decode the file: the band index of every
// frame, and the RMS of every frame's audio block.
static bool renderVideoPlayer(Graph& g, int frames, const std::string& outPath,
                              std::vector<int>& idx, std::vector<double>& rms) {
    g.transport().bpm = 120.0;                                       // 2 s per bar
    RenderSettings s;
    s.startBar = 0.0; s.endBar = frames / (double)kIdxFps / 2.0; s.prerollBars = 0.0;
    s.fps = kIdxFps; s.width = kIdxW; s.height = kIdxH; s.outPath = outPath;
    std::remove(s.outPath.c_str());
    OfflineRenderer r; std::string err;
    if (!r.start(g, s, err)) { std::fprintf(stderr, "render: %s\n", err.c_str()); return false; }
    int guard = 0;
    while (r.step(0.05)) {
        if (r.progress().waitingForLoad) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (++guard > 1000000) return false;
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
// loading() and the renderer's gate are part of what is tested.
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

// --- Scenario: changing the file or deleting the node mid-seek returns promptly ---
static bool scenario_video_player_shutdown() {
    {
        const std::string clip = "build/_video_shutdown.mp4";      // 640x360, one keyframe: a long catch-up
        if (!writeIndexedClip(clip, 300, 300, 640, 360)) { return failed("video shutdown: write clip"); }
        for (int mode = 0; mode < 2; ++mode) {
            Graph g;
            auto vid = std::make_unique<VideoPlayerNode>();
            vid->inputDefault(0) = clip;
            vid->initGL();
            int vId = g.addNode(std::move(vid));
            auto* vp = dynamic_cast<VideoPlayerNode*>(g.findNode(vId));
            if (!parkPlayhead(g, vp, 11.5f)) { return failed("video shutdown: open"); }   // starts a long catch-up
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto t0 = std::chrono::steady_clock::now();
            if (mode == 0) { vp->inputDefault(0) = std::string("tests/assets/test.mp4"); g.evaluate(0.0f); }
            else           { g.removeNode(vId); }
            const double ms = secondsSince(t0) * 1000.0;
            if (ms > 200.0) {
                std::fprintf(stderr, "video shutdown: %s took %.0f ms\n", mode == 0 ? "changing the file" : "deleting the node", ms);
                return failed("video shutdown: the worker did not stop promptly");
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
// glBlitFramebuffer puts it bottom-up into the published texture.
//
// Offline renders stay frame-exact: evaluate() waits for the exact frame, and loading()
// reports the NEXT frame not ready yet, so the renderer's gate does the waiting between frames.
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

    // Offline: how long evaluate() waits for a frame the prefetch did not predict.
    static constexpr double kOfflineFrameWaitSeconds = 2.0;

private:
    void openPath(const std::string& path);
    VideoRequest makeRequest(const VideoPlayhead& p, bool play, float rate, bool loop) const;
    void ensureTextures(int w, int h);
    void freeGL();
    void upload(const VideoStream::FrameView& f);
    void publishEmpty(EvalContext& ctx);
    void updateStatus(bool play, float rate);

    std::unique_ptr<VideoStream> stream_;
    std::string   path_;
    std::string   status_;
    bool          needInfo_ = false;     // the stream is new: read its info once it is Ready
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
    stream_.reset();   // stop and join the worker first
    freeGL();
}

void VideoPlayerNode::initGL() { /* textures are allocated once the stream knows the size */ }

void VideoPlayerNode::evaluate(EvalContext& ctx) {
    const std::string& path = ctx.in<std::string>(0);
    const float rate = ctx.in<float>(1);
    const bool  play = ctx.in<bool>(2);
    const bool  loop = ctx.in<bool>(3);
    offline_ = ctx.offline;
    if (!offline_) stalled_ = false;
    loop_ = loop;

    if (path != path_) { path_ = path; openPath(path); }
    if (!stream_) { publishEmpty(ctx); return; }

    const VideoStream::State st = stream_->state();
    if (st == VideoStream::State::Failed) {
        status_ = "load failed: " + stream_->error();
        if (!failLogged_) {
            std::fprintf(stderr, "[Video] load failed: %s (%s)\n", stream_->error().c_str(), path_.c_str());
            failLogged_ = true;
        }
        publishEmpty(ctx);
        return;
    }
    if (st == VideoStream::State::Opening) {
        status_ = "opening...";
        publishEmpty(ctx);
        return;
    }
    if (needInfo_) {
        const VideoStream::Info inf = stream_->info();
        duration_ = inf.duration;
        frameDur_ = inf.frameDur;
        ensureTextures(inf.width, inf.height);
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

    // Offline renders are exact: wait for the frame for u (normally already there -- see below).
    if (offline_ && !stream_->frameReadyFor(ph_.u) &&
        !stream_->waitForFrame(ph_.u, kOfflineFrameWaitSeconds))
        stalled_ = true;                               // publish what we have; loading() fails the render

    VideoStream::FrameView fv;
    if (stream_->frameAt(ph_.u, fv) && fv.serial != shownSerial_) upload(fv);
    ctx.out<TexRef>(0, TexRef{ shownSerial_ ? tex_ : 0u, texW_, texH_ });

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
    if (!offline_ || st != VideoStream::State::Ready) return false;
    return stalled_ || !stream_->frameReadyFor(pendingU_);
}

void VideoPlayerNode::openPath(const std::string& path) {
    stream_.reset();                                    // joins the old worker (bounded)
    ph_ = VideoPlayhead{};
    duration_ = 0.0;
    frameDur_ = 1.0 / 30.0;
    shownSerial_ = 0;
    shownT_ = -1.0;
    stalled_ = false;
    failLogged_ = false;
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
}

void VideoPlayerNode::publishEmpty(EvalContext& ctx) {
    ctx.out<TexRef>(0, TexRef{});
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
- Modify: `CLAUDE.md` (two bullets under *Hard rules*)

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
  mutex-guarded frame pool. GL uploads always happen on the main thread.
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
  wrapped in a no-op-free `AVBufferRef`). Video packets are queued (≤ 64 MB) rather than
  decoded at once, so `pumpAudio()` keeps 48 kHz mono audio ahead of the video;
  `audioSettledUpTo()` says how far no more audio can arrive. The legacy `decodeFrame()`
  (bottom-up RGBA + audio appended) is built on those and unchanged for `gl_smoke`. Every time
  in and out counts from the first video frame (decoded at `open()`), so a container that starts
  its clock late (MPEG-TS) or shows a B-frame delay (FLV, fragmented MP4) still plays from 0;
  `open()` also seeks to the start first, so indexes read only on a seek (Matroska cues) are there.
- **The Video Player decodes on a worker** (`src/gfx/VideoStream.{h,cpp}`, one per node,
  GL-free): it opens the file, keeps a fixed pool of RGBA frames (512 MB budget:
  `core/VideoPlan.h` `videoPoolFrames`) decoded ahead of the requested playhead, and reads
  audio into a `core/TimedAudio.h` store. Its decisions — fill, catch up (decode without
  converting, 100 ms slices), seek (only for jumps over 1 s: a seek restarts the frame-thread
  pipeline), wrap, and reverse keyframe-to-frame stretches published whole — are the pure,
  unit-tested `videoNextStep()`. A seek lands where its decode loop admits frames, backing off
  1 s, 2 s, 4 s… to the file's start when it lands late (decode-time indexes, MPEG-TS), and a
  seek ahead that landed behind the decoder is not repeated nearby (`videoNoSeekBelow`). An
  offline render starting in reverse restarts the run: live stretches keep every stride-th frame. The node's playhead is UNWRAPPED (lap × D + position,
  `videoAdvance`), so the worker decodes the next lap early and loops are seamless; it is held
  at the start until the first frame is on screen. The node uploads the newest frame at or
  before the playhead into a staging texture and flips it into the published texture with
  one `glBlitFramebuffer`. Offline renders stay exact: `evaluate()` waits for the frame for
  the playhead and `loading()` reports the NEXT frame not ready, so the renderer's gate does
  the waiting between frames.
```

- [ ] **Step 3: Check the new text is in**

```bash
grep -c 'decodes on a worker' CLAUDE.md
```

Expected output includes: `1`

- [ ] **Step 4: Commit**

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

Expected on every line: the picture back within 0.2 s after < 0.3 s, with at most 1 seek (prototype: 0.05–0.08 s, 0 seeks; without the seek guard 3 runs in 5 never caught up).

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

- [ ] **Step 2:** Use superpowers:finishing-a-development-branch to decide how to integrate `fix/video-player-ui-stall` (PR to `main`; the three CI workflows build Linux/macOS/Windows).
