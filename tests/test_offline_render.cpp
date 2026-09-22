#include <doctest/doctest.h>
#include <limits>
#include <memory>
#include "core/Graph.h"
#include "core/Node.h"
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

// Documented where the math lives, because the consequence is in OfflineRenderer::start(): a
// zero frame count is reachable through settings that pass EVERY validation rule, so start() has
// to reject it separately (`if (total < 1) ... "the render range is empty at this tempo"`).
// Without that, step() would capture one frame and then immediately finish(Done) -- a success
// reporting "0 frames" -- or, if the completion check were hoisted above the first frame instead,
// a Done outcome with no encoder ever opened and no file on disk at all.
TEST_CASE("renderFrameCount: zero is reachable from settings that validate, so start() must reject") {
    std::string err;
    // Route 1: an endBar that is finite, positive and after startBar -- nothing validateRenderSettings
    // tests -- whose frame count then overflows renderFramesOver's representable range.
    RenderSettings s; s.startBar = 0.0; s.endBar = 1e18; s.fps = 60;
    s.width = 64; s.height = 64; s.outPath = "out.mp4";
    CHECK(validateRenderSettings(s, true, err));       // every rule passes...
    CHECK(renderFrameCount(s, kSpb120) == 0);          // ...and there is still nothing to render

    // Route 2: a hand-edited project file with beatsPerBar = 0 -> no seconds in a bar, so any
    // bar range is zero-length however sane the settings are.
    RenderSettings ok; ok.startBar = 0.0; ok.endBar = 8.0; ok.fps = 60;
    ok.width = 64; ok.height = 64; ok.outPath = "out.mp4";
    Transport t; t.bpm = 120.0; t.beatsPerBar = 0;
    CHECK(t.secondsPerBar() == 0.0);
    CHECK(validateRenderSettings(ok, true, err));
    CHECK(renderFrameCount(ok, t.secondsPerBar()) == 0);
    CHECK(renderFrameCount(ok, kSpb120) == 960);       // the same settings at a real tempo
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

static RenderSettings validSettings() {
    RenderSettings s; s.startBar = 0.0; s.endBar = 4.0; s.prerollBars = 1.0;
    s.fps = 60; s.width = 1280; s.height = 720; s.outPath = "out.mp4";
    return s;
}

TEST_CASE("validateRenderSettings: a good set passes and each fault has its own message") {
    std::string err;
    CHECK(validateRenderSettings(validSettings(), true, err));
    CHECK(err.empty());

    RenderSettings s = validSettings();
    CHECK_FALSE(validateRenderSettings(s, false, err));
    CHECK(err == "add an Output node");

    s = validSettings(); s.endBar = s.startBar;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "finish bar must be after start bar");

    s = validSettings(); s.endBar = std::nan("");   // the negated `!(end > start)` is deliberate: pin it
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "finish bar must be after start bar");

    s = validSettings(); s.startBar = -1.0; s.endBar = 2.0;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "start bar must be 0 or later");

    // Both the finish-bar-order check and the start->=0 check fail at once: this pins which
    // one wins, i.e. that they run in this order and not the other way around.
    s = validSettings(); s.startBar = -2.0; s.endBar = -3.0;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "finish bar must be after start bar");

    s = validSettings(); s.prerollBars = -0.5;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "pre-roll must be 0 or more bars");

    s = validSettings(); s.fps = 29;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "frame rate must be 24, 25, 30, 50 or 60");

    s = validSettings(); s.width = 8;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "width and height must be between 16 and 8192");

    s = validSettings(); s.height = 9000;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "width and height must be between 16 and 8192");

    s = validSettings(); s.width = 9000;   // other axis, other direction
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "width and height must be between 16 and 8192");

    s = validSettings(); s.height = 8;     // other axis, other direction
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "width and height must be between 16 and 8192");

    s = validSettings(); s.width = 641;
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "width and height must be even");

    s = validSettings(); s.height = 481;   // the other axis of the even check
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "width and height must be even");

    s = validSettings(); s.outPath.clear();
    CHECK_FALSE(validateRenderSettings(s, true, err));
    CHECK(err == "choose an output file");

    err = "stale";   // a success must clear a leftover message from a prior failed call
    CHECK(validateRenderSettings(validSettings(), true, err));
    CHECK(err.empty());
}

TEST_CASE("parseRenderArgs: full option set") {
    RenderCliArgs a; std::string err;
    REQUIRE(parseRenderArgs({"song.oss", "out.mp4", "--start", "2", "--end", "10.5",
                             "--fps", "30", "--size", "1920x1080", "--preroll", "0.25"}, a, err));
    CHECK(a.projectPath == "song.oss");
    CHECK(a.settings.outPath == "out.mp4");
    CHECK(a.settings.startBar == doctest::Approx(2.0));
    CHECK(a.settings.endBar == doctest::Approx(10.5));
    CHECK(a.settings.fps == 30);
    CHECK(a.settings.width == 1920);
    CHECK(a.settings.height == 1080);
    CHECK(a.settings.prerollBars == doctest::Approx(0.25));
}

TEST_CASE("parseRenderArgs: defaults leave the sentinels for the driver to fill") {
    RenderCliArgs a; std::string err;
    REQUIRE(parseRenderArgs({"song.oss", "out.mp4"}, a, err));
    CHECK(a.settings.startBar == doctest::Approx(0.0));
    CHECK(a.settings.endBar == doctest::Approx(-1.0));     // -> the project's song length
    CHECK(a.settings.width == 0);                          // -> the Preferences texture size
    CHECK(a.settings.height == 0);
    CHECK(a.settings.fps == 60);
    CHECK(a.settings.prerollBars == doctest::Approx(1.0));

    // A second parse into the same struct must not inherit anything from the first: it has
    // to reset every field, not just the ones this call happens to set explicitly.
    a.settings.fps = 7;
    REQUIRE(parseRenderArgs({"song.oss", "out.mp4"}, a, err));
    CHECK(a.settings.fps == 60);
}

TEST_CASE("parseRenderArgs: bad input is rejected with a message") {
    RenderCliArgs a; std::string err;
    CHECK_FALSE(parseRenderArgs({"song.oss"}, a, err));                       // missing output
    CHECK(err.rfind("usage:", 0) == 0);
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--size", "12x"}, a, err));
    CHECK(err == "--size expects WxH, got 12x");
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--size", "abc"}, a, err));
    CHECK(err == "--size expects WxH, got abc");
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--size", "1920y1080"}, a, err));      // wrong separator
    CHECK(err == "--size expects WxH, got 1920y1080");
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--size", "1920x1080junk"}, a, err));  // trailing junk
    CHECK(err == "--size expects WxH, got 1920x1080junk");
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--fps", "sixty"}, a, err));
    CHECK(err == "bad value for --fps: sixty");
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--fps", "inf"}, a, err));             // not an integer
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--start", "2junk"}, a, err));         // trailing junk
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--start", "nan"}, a, err));           // non-finite
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--end"}, a, err));
    CHECK(err == "--end needs a value");
    CHECK_FALSE(parseRenderArgs({"song.oss", "out.mp4", "--bogus", "1"}, a, err));
    CHECK(err == "unknown option --bogus");
}

namespace {
struct OfflineProbe : Node {
    bool seen = false;
    OfflineProbe() : Node("probe") {}
    void evaluate(EvalContext& ctx) override { seen = ctx.offline; }
};
}

TEST_CASE("Graph::setOffline reaches every node through EvalContext::offline") {
    Graph g;
    auto p = std::make_unique<OfflineProbe>();
    OfflineProbe* probe = p.get();
    g.addNode(std::move(p));
    CHECK_FALSE(g.offline());
    g.evaluate(1.0f / 60.0f);
    CHECK_FALSE(probe->seen);
    g.setOffline(true);
    CHECK(g.offline());
    g.evaluate(1.0f / 60.0f);
    CHECK(probe->seen);
    g.setOffline(false);
    g.evaluate(1.0f / 60.0f);
    CHECK_FALSE(probe->seen);
}
