#include <doctest/doctest.h>
#include "core/MidiFile.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

// A tiny SMF: format 0, 1 track, 480 ticks/quarter; note-on 60 @ t0, note-off 60 @
// t480, note-on 64 @ t480, note-off 64 @ t960, end-of-track.
static std::vector<unsigned char> sampleSmf() {
    return {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0x01,0xE0,
        'M','T','r','k', 0,0,0,22,
        0x00, 0x90,0x3C,0x64,
        0x83,0x60, 0x80,0x3C,0x00,
        0x00, 0x90,0x40,0x64,
        0x83,0x60, 0x80,0x40,0x00,
        0x00, 0xFF,0x2F,0x00
    };
}

TEST_CASE("parses a simple MIDI file into beat-positioned events") {
    auto smf = sampleSmf();
    MidiSequence s = parseMidiFile(smf.data(), smf.size());
    REQUIRE(s.ok);
    REQUIRE(s.events.size() == 4);
    CHECK(s.events[0].beats == doctest::Approx(0.0));
    CHECK(midiIsNoteOn(s.events[0].ev));
    CHECK(s.events[0].ev.data1 == 60);
    CHECK(s.events[1].beats == doctest::Approx(1.0));     // 480 / 480
    CHECK(midiIsNoteOff(s.events[1].ev));
    CHECK(s.events[1].ev.data1 == 60);
    CHECK(s.events[2].beats == doctest::Approx(1.0));
    CHECK(s.events[2].ev.data1 == 64);
    CHECK(s.events[3].beats == doctest::Approx(2.0));
    CHECK(s.lengthBeats == doctest::Approx(2.0));
}

TEST_CASE("rejects a non-MIDI buffer and a truncated header") {
    std::vector<unsigned char> bad = {'X','X','X','X', 0,0,0,6};
    CHECK_FALSE(parseMidiFile(bad.data(), bad.size()).ok);
    std::vector<unsigned char> trunc = {'M','T','h','d', 0,0};
    CHECK_FALSE(parseMidiFile(trunc.data(), trunc.size()).ok);
}

TEST_CASE("handles running status (an omitted repeated status byte)") {
    std::vector<unsigned char> smf = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0x01,0xE0,
        'M','T','r','k', 0,0,0,12,
        0x00, 0x90,0x3C,0x64,           // note-on 60
        0x81,0x70, 0x40,0x64,           // +240, running status -> note-on 64
        0x00, 0xFF,0x2F,0x00
    };
    MidiSequence s = parseMidiFile(smf.data(), smf.size());
    REQUIRE(s.ok);
    REQUIRE(s.events.size() == 2);
    CHECK(s.events[0].ev.data1 == 60);
    CHECK(s.events[1].ev.data1 == 64);
    CHECK(s.events[1].beats == doctest::Approx(240.0 / 480.0));
}

TEST_CASE("rejects a track whose first event has no status byte (bad running status)") {
    std::vector<unsigned char> smf = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0x01,0xE0,
        'M','T','r','k', 0,0,0,5,
        0x00, 0x3C,0x64, 0xFF,0x2F       // delta 0, then a data byte with no prior status
    };
    CHECK_FALSE(parseMidiFile(smf.data(), smf.size()).ok);
}
