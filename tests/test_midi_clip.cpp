#include <doctest/doctest.h>
#include "core/MidiClip.h"
#include "core/MidiFile.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

static MidiSequence makeSeq() {
    MidiSequence s; s.ok = true;
    s.events = {
        { 0.0, midiNoteOn(60, 100) },
        { 1.0, midiNoteOff(60) },
        { 2.0, midiNoteOn(64, 100) },
    };
    s.lengthBeats = 3.0;
    return s;
}
static int countNoteOns(const std::vector<MidiEvent>& e) {
    int n = 0; for (auto& x : e) if (midiIsNoteOn(x)) ++n; return n;
}
static int countNoteOffs(const std::vector<MidiEvent>& e) {
    int n = 0; for (auto& x : e) if (midiIsNoteOff(x)) ++n; return n;
}

TEST_CASE("clip emits events in each frame's beat window") {
    MidiSequence s = makeSeq();
    MidiClipPlayer p;
    bool mute[16] = {};
    CHECK(p.advance(s, 0.0, 4, 0.0, false, 4, mute).empty());            // entry: nothing
    CHECK(countNoteOns(p.advance(s, 0.5, 4, 0.0, false, 4, mute)) == 1); // [0,0.5) -> note-on 60
    CHECK(countNoteOffs(p.advance(s, 1.5, 4, 0.0, false, 4, mute)) == 1);// [0.5,1.5) -> note-off 60
    CHECK(countNoteOns(p.advance(s, 2.5, 4, 0.0, false, 4, mute)) == 1); // note-on 64
}

TEST_CASE("clip mutes a channel") {
    MidiSequence s = makeSeq();          // all on channel 0
    MidiClipPlayer p;
    bool mute[16] = {}; mute[0] = true;
    p.advance(s, 0.0, 4, 0.0, false, 4, mute);
    CHECK(p.advance(s, 2.5, 4, 0.0, false, 4, mute).empty());
}

TEST_CASE("clip respects the start offset") {
    MidiSequence s = makeSeq();
    MidiClipPlayer p;
    bool mute[16] = {};
    CHECK(p.advance(s, 2.0, 4, 1.0, false, 4, mute).empty());            // offset 1 bar; local < 0
    p.advance(s, 4.0, 4, 1.0, false, 4, mute);                          // entry at local 0
    CHECK(countNoteOns(p.advance(s, 4.5, 4, 1.0, false, 4, mute)) == 1); // local 0.5 -> note-on 60
}

TEST_CASE("clip flushes a sounding note at the loop seam") {
    MidiSequence s; s.ok = true;
    s.events = { { 0.0, midiNoteOn(60, 100) } };   // note-on with no note-off in the file
    s.lengthBeats = 2.0;
    MidiClipPlayer p;
    bool mute[16] = {};
    p.advance(s, 0.0, 4, 0.0, true, 0.5, mute);                         // entry, loopLen=2 beats
    CHECK(countNoteOns(p.advance(s, 0.5, 4, 0.0, true, 0.5, mute)) == 1); // note-on 60 (beat 0)
    auto wrap = p.advance(s, 2.25, 4, 0.0, true, 0.5, mute);            // playPos wraps to 0.25
    CHECK(countNoteOffs(wrap) >= 1);                                    // released at the seam
}

TEST_CASE("a paused transport emits nothing") {
    MidiSequence s = makeSeq();
    MidiClipPlayer p;
    bool mute[16] = {};
    p.advance(s, 1.5, 4, 0.0, false, 4, mute);
    CHECK(p.advance(s, 1.5, 4, 0.0, false, 4, mute).empty());           // same position
}

TEST_CASE("clip releases a sounding note when its channel is muted mid-note") {
    MidiSequence s; s.ok = true;
    s.events = { { 0.0, midiNoteOn(60, 100) }, { 4.0, midiNoteOff(60) } };
    s.lengthBeats = 5.0;
    MidiClipPlayer p;
    bool mute[16] = {};
    p.advance(s, 0.0, 4, 0.0, false, 8, mute);                          // entry
    CHECK(countNoteOns(p.advance(s, 0.5, 4, 0.0, false, 8, mute)) == 1);// note-on 60 now sounding
    mute[0] = true;
    auto e = p.advance(s, 1.0, 4, 0.0, false, 8, mute);                 // channel 0 muted
    CHECK(countNoteOffs(e) >= 1);                                       // released by the mute
}

TEST_CASE("clip releases a held note when a non-looping clip ends") {
    MidiSequence s; s.ok = true;
    s.events = { { 0.0, midiNoteOn(60, 100) } };   // note-on with no note-off in the file
    s.lengthBeats = 2.0;
    MidiClipPlayer p;
    bool mute[16] = {};
    p.advance(s, 0.0, 4, 0.0, false, 8, mute);                          // entry
    p.advance(s, 0.5, 4, 0.0, false, 8, mute);                          // note-on 60 sounding
    auto e = p.advance(s, 2.5, 4, 0.0, false, 8, mute);                 // past the clip end (2.0)
    CHECK(countNoteOffs(e) >= 1);                                       // released at the end
}
