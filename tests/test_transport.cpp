#include <doctest/doctest.h>
#include "core/Transport.h"
#include "core/Graph.h"
#include "core/Node.h"
#include <memory>

using namespace oss;

TEST_CASE("transport advances only while playing") {
    Transport t;
    t.advance(1.0);
    CHECK(t.seconds == doctest::Approx(0.0));   // not playing -> frozen
    t.play();
    t.advance(0.5);
    CHECK(t.seconds == doctest::Approx(0.5));
    t.pause();
    t.advance(1.0);
    CHECK(t.seconds == doctest::Approx(0.5));   // paused -> frozen again
}

TEST_CASE("beats, bars and millis derive from the tempo (120 BPM, 4/4)") {
    Transport t;
    t.bpm = 120.0;        // 0.5 s per beat, 2 s per bar
    t.seconds = 2.0;
    CHECK(t.beats()  == doctest::Approx(4.0));
    CHECK(t.bars()   == doctest::Approx(1.0));
    CHECK(t.millis() == doctest::Approx(2000.0));
    CHECK(t.barNumber() == 2);   // start of the second bar
    CHECK(t.beatInBar() == 1);
}

TEST_CASE("position starts at bar 1, beat 1") {
    Transport t;
    CHECK(t.barNumber() == 1);
    CHECK(t.beatInBar() == 1);
}

TEST_CASE("changing the tempo rescales the beat position") {
    Transport t;
    t.seconds = 1.0;
    t.bpm = 60.0;                                 // 1 beat / second
    CHECK(t.beats() == doctest::Approx(1.0));
    t.bpm = 120.0;                                // 2 beats / second
    CHECK(t.beats() == doctest::Approx(2.0));
}

TEST_CASE("stop pauses and returns to the start") {
    Transport t;
    t.play();
    t.seconds = 5.0;
    t.stop();
    CHECK(t.playing == false);
    CHECK(t.seconds == doctest::Approx(0.0));
}

TEST_CASE("rewind/forward move by one bar; rewind clamps at zero") {
    Transport t;
    t.bpm = 120.0;                                // 2 s per bar
    t.forwardBar();
    CHECK(t.seconds == doctest::Approx(2.0));
    t.forwardBar();
    CHECK(t.seconds == doctest::Approx(4.0));
    t.rewindBar();
    CHECK(t.seconds == doctest::Approx(2.0));
    t.rewindBar();
    t.rewindBar();                               // would go negative
    CHECK(t.seconds == doctest::Approx(0.0));
}

TEST_CASE("a zero tempo never divides by zero") {
    Transport t;
    t.bpm = 0.0;
    t.seconds = 1.0;
    CHECK(t.beats() == doctest::Approx(2.0));     // falls back to 120 BPM
}

TEST_CASE("looping wraps from the loop end back to the loop start") {
    Transport t;
    t.bpm = 120.0;                 // 2 s per bar
    t.looping = true;
    t.loopStartBar = 1.0;          // 2 s
    t.loopEndBar   = 3.0;          // 6 s -> loop length 4 s
    t.play();
    t.seconds = 5.9;
    t.advance(0.2);                // 6.1 >= 6.0 -> 2 + fmod(6.1-2, 4) = 2.1
    CHECK(t.seconds == doctest::Approx(2.1));
}

TEST_CASE("looping does nothing when the toggle is off") {
    Transport t;
    t.bpm = 120.0;
    t.loopStartBar = 0.0; t.loopEndBar = 2.0;
    t.play();
    t.seconds = 10.0;
    t.advance(0.1);
    CHECK(t.seconds == doctest::Approx(10.1));   // keeps advancing past the would-be loop
}

TEST_CASE("an inverted loop (end <= start) never wraps") {
    Transport t;
    t.bpm = 120.0; t.looping = true;
    t.loopStartBar = 4.0; t.loopEndBar = 2.0;
    t.play(); t.seconds = 20.0;
    t.advance(0.1);
    CHECK(t.seconds == doctest::Approx(20.1));
}

TEST_CASE("a paused transport does not loop-wrap") {
    Transport t;
    t.bpm = 120.0; t.looping = true;
    t.loopStartBar = 0.0; t.loopEndBar = 2.0;   // ends at 4 s
    t.pause();
    t.seconds = 10.0;                            // parked past the loop while paused
    t.advance(0.1);
    CHECK(t.seconds == doctest::Approx(10.0));   // stays put
}

namespace {
// Records the transport it was handed during evaluation.
struct TransportProbe : Node {
    TransportProbe() : Node("Probe") {}
    void evaluate(EvalContext& ctx) override {
        seen  = ctx.transport;
        beats = ctx.transport ? ctx.transport->beats() : -1.0;
    }
    const Transport* seen  = nullptr;
    double           beats = -1.0;
};
} // namespace

TEST_CASE("Graph::evaluate advances the transport and hands it to nodes") {
    Graph g;
    g.transport().bpm = 120.0;
    g.transport().play();

    auto probe = std::make_unique<TransportProbe>();
    TransportProbe* p = probe.get();
    g.addNode(std::move(probe));

    g.evaluate(1.0f);                            // advance one second

    CHECK(g.transport().seconds == doctest::Approx(1.0));
    CHECK(p->seen == &g.transport());            // node saw the graph's transport
    CHECK(p->beats == doctest::Approx(2.0));     // 1 s at 120 BPM = 2 beats
}
