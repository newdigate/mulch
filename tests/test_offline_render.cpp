#include <doctest/doctest.h>
#include <limits>
#include "core/OfflineRender.h"

using namespace oss;

// 120 bpm, 4/4 -> 2 s per bar.
static const double kSpb120 = 2.0;

TEST_CASE("renderFrameCount: exact for the listed frame rates") {
    RenderSettings s; s.startBar = 0.0; s.endBar = 8.0; s.fps = 60;
    CHECK(renderFrameCount(s, kSpb120) == 960);                 // 16 s * 60
    s.fps = 30; s.endBar = 1.0;
    CHECK(renderFrameCount(s, 2.4) == 72);                      // 1 bar @ 100 bpm = 2.4 s
    s.fps = 60; s.startBar = 0.0; s.endBar = 0.5;
    CHECK(renderFrameCount(s, kSpb120) == 60);                  // half a bar
    s.startBar = 2.0; s.endBar = 10.0; s.fps = 25;
    CHECK(renderFrameCount(s, kSpb120) == 400);                 // start offset does not matter
}

TEST_CASE("renderFrameCount: float noise does not add a frame, a tiny range still yields one") {
    RenderSettings s; s.startBar = 0.0; s.endBar = 0.1; s.fps = 60;
    CHECK(renderFrameCount(s, kSpb120) == 12);                  // 0.2 s * 60 = 12.000000000000002
    s.endBar = 0.0001;
    CHECK(renderFrameCount(s, kSpb120) == 1);                   // non-empty -> at least one frame
    s.endBar = 0.0;
    CHECK(renderFrameCount(s, kSpb120) == 0);                   // empty
    s.endBar = -1.0;
    CHECK(renderFrameCount(s, kSpb120) == 0);                   // inverted
}

TEST_CASE("renderFrameCount: a partial trailing frame is still rendered") {
    RenderSettings s; s.startBar = 0.0; s.endBar = 1.0; s.fps = 60;
    const double spb145 = 240.0 / 145.0;              // 1 bar @ 145 bpm = 1.655172 s
    CHECK(renderFrameCount(s, spb145) == 100);        // 99.31 -> ceil, not round/floor
    long long n = renderFrameCount(s, spb145);        // the contract, asserted directly:
    CHECK(renderFrameSeconds(s, spb145, n - 1) <  spb145);   // last frame starts in range
    CHECK(renderFrameSeconds(s, spb145, n)     >= spb145);   // the next one does not
}

TEST_CASE("renderFrameCount: a non-finite or absurd range yields no frames, never UB") {
    RenderSettings s; s.startBar = 0.0; s.fps = 60;
    s.endBar = std::numeric_limits<double>::quiet_NaN();
    CHECK(renderFrameCount(s, kSpb120) == 0);
    s.endBar = std::numeric_limits<double>::infinity();
    CHECK(renderFrameCount(s, kSpb120) == 0);
    s.endBar = 1e18;
    CHECK(renderFrameCount(s, kSpb120) == 0);
}

TEST_CASE("prerollFrameCount: same rule over prerollBars") {
    RenderSettings s; s.prerollBars = 1.0; s.fps = 60;
    CHECK(prerollFrameCount(s, kSpb120) == 120);
    s.prerollBars = 0.5; s.fps = 30;
    CHECK(prerollFrameCount(s, kSpb120) == 30);
    s.prerollBars = 0.0;
    CHECK(prerollFrameCount(s, kSpb120) == 0);
}

TEST_CASE("renderFrameSeconds: frame k from the start bar, clamped at zero") {
    RenderSettings s; s.startBar = 4.0; s.fps = 60;
    CHECK(renderFrameSeconds(s, kSpb120, 0)   == doctest::Approx(8.0));
    CHECK(renderFrameSeconds(s, kSpb120, 30)  == doctest::Approx(8.5));
    CHECK(renderFrameSeconds(s, kSpb120, -1)  == doctest::Approx(8.0 - 1.0 / 60.0));
    s.startBar = 0.0;
    CHECK(renderFrameSeconds(s, kSpb120, -120) == doctest::Approx(0.0));   // pre-roll sits at the start
    CHECK(renderFrameSeconds(s, kSpb120, -1)   == doctest::Approx(0.0));
}

TEST_CASE("pre-roll and the frame clock compose: frame -P sits prerollBars before the start") {
    RenderSettings s; s.startBar = 4.0; s.prerollBars = 1.0; s.fps = 60;
    const long long P = prerollFrameCount(s, kSpb120);
    CHECK(P == 120);
    CHECK(renderFrameSeconds(s, kSpb120, -P) == doctest::Approx(6.0));   // one bar before bar 4
}

TEST_CASE("audioSamplesPerFrame: exact at 48 kHz for every listed rate") {
    for (int fps : kRenderFrameRates) {
        CHECK(isRenderFrameRate(fps));
        CHECK(48000 % fps == 0);
        CHECK(audioSamplesPerFrame(48000, fps) == 48000 / fps);
    }
    CHECK(audioSamplesPerFrame(48000, 60) == 800);
    CHECK(audioSamplesPerFrame(48000, 0) == 0);
    CHECK_FALSE(isRenderFrameRate(29));
    CHECK_FALSE(isRenderFrameRate(0));
}
