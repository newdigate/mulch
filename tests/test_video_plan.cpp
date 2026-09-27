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
            for (long i = keyAbove - gop; tOf(i) <= tOf(keyAbove) - kVideoTimeEps; ++i)   // what it admits
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
