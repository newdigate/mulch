#include <doctest/doctest.h>
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

// The frames a live stretch keeps, as indices counted back from `end` (0 = end).
static std::vector<long> keptBack(const VideoStretch& s, double key, double fd) {
    std::vector<long> k;
    for (double t = key; t <= s.end + 1e-9; t += fd)
        if (videoStretchKeeps(s, t, fd)) k.push_back(std::lround((s.end - t) / fd));
    return k;
}

TEST_CASE("videoPlanStretch: every frame when the stretch fits the budget") {
    const double fd = 1.0 / 30.0;
    VideoStretch s = videoPlanStretch(0.0, 29 * fd, fd, 32, false);   // 30 frames, budget 32
    CHECK(s.stride == 1);
    CHECK(keptBack(s, 0.0, fd).size() == 30);
}

TEST_CASE("videoPlanStretch: every n-th frame otherwise, always keeping the end, within budget") {
    const double fd = 1.0 / 30.0;
    VideoStretch s = videoPlanStretch(0.0, 29 * fd, fd, 8, false);    // 30 frames, budget 8
    CHECK(s.stride == 4);
    std::vector<long> k = keptBack(s, 0.0, fd);
    CHECK(k.size() == 8);
    CHECK(k.front() == 28);                                           // ascending time: earliest first...
    CHECK(k.back() == 0);                                             // ...and the end frame is kept

    VideoStretch l = videoPlanStretch(0.0, 249 * fd, fd, 8, false);   // a 250-frame keyframe interval
    CHECK(l.stride == 32);
    std::vector<long> kl = keptBack(l, 0.0, fd);
    CHECK(kl.size() <= 8);
    CHECK(kl.back() == 0);
    CHECK(249 - kl.front() < l.stride);                               // no gap wider than the stride
}

TEST_CASE("videoStretchKeeps: an end between frames counts from the frame containing it") {
    const double fd = 1.0 / 30.0;
    const double end = 29 * fd - 1e-6;                 // just below a keyframe: the stretch above's start
    VideoStretch s = videoPlanStretch(0.0, end, fd, 8, false);
    CHECK(s.stride == 4);                              // 29 frames in [0, end]
    CHECK(videoStretchKeeps(s, 28 * fd, fd));          // the frame just below the boundary is kept
    CHECK_FALSE(videoStretchKeeps(s, 27 * fd, fd));
    CHECK(videoStretchKeeps(s, 24 * fd, fd));
}

TEST_CASE("videoPlanStretch: offline keeps consecutive frames (a ring of the newest)") {
    const double fd = 1.0 / 30.0;
    VideoStretch s = videoPlanStretch(0.0, 249 * fd, fd, 8, true);
    CHECK(s.contiguous);
    CHECK(s.stride == 1);
    CHECK(s.keep == 8);
    CHECK(keptBack(s, 0.0, fd).size() == 250);                        // all converted; the ring keeps 8
}

