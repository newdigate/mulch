#include <doctest/doctest.h>
#include "core/MidiClock.h"
#include "core/Transport.h"

using namespace oss;

TEST_CASE("BeatClockReader derives tempo, position, and play state") {
    BeatClockReader r;
    r.onStart();
    CHECK(r.playing());
    for (int i = 0; i < 24; ++i) r.onTick(i * (0.5 / 24.0));   // 24 ticks over 0.5 s -> 120 BPM, 1 beat
    CHECK(r.bpm() == doctest::Approx(120.0).epsilon(0.02));
    CHECK(r.positionBeats() == doctest::Approx(1.0));
    r.onStop();
    CHECK_FALSE(r.playing());
    r.onSongPosition(16);                                       // 16 sixteenths = 4 beats
    CHECK(r.positionBeats() == doctest::Approx(4.0));
}

TEST_CASE("SPP + message helpers") {
    CHECK(beatsToSixteenths(4.0) == 16);
    unsigned char m[3];
    sppMessage(16, m);
    CHECK(m[0] == 0xF2);
    CHECK(m[1] == 16);
    CHECK(m[2] == 0);
    CHECK(sppToSixteenths(16, 0) == 16);
    CHECK(sppToSixteenths(0x7F, 0x01) == 255);                 // 14-bit: (1<<7)|127
}

TEST_CASE("Transport externalClock suppresses local advance") {
    Transport t; t.playing = true; t.seconds = 5.0; t.externalClock = true;
    t.advance(1.0);
    CHECK(t.seconds == doctest::Approx(5.0));                  // frozen: engine owns the clock
    t.externalClock = false;
    t.advance(1.0);
    CHECK(t.seconds == doctest::Approx(6.0));                  // local advance resumes
}
