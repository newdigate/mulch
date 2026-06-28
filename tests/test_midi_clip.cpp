#include <doctest/doctest.h>
#include "core/MidiClip.h"
#include "core/MidiFile.h"
#include "core/Value.h"
#include <vector>
#include "modules/MidiFilePlayerNode.h"
#include "core/Node.h"
#include "core/Transport.h"
#include <fstream>
#include <cstdio>
#include <string>
#include <variant>

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

TEST_CASE("MidiFilePlayerNode loads a file and emits notes synced to the transport") {
    // A tiny SMF (note-on 60 @ beat 0) written to a temp file.
    std::vector<unsigned char> smf = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0x01,0xE0,
        'M','T','r','k', 0,0,0,8,
        0x00, 0x90,0x3C,0x64,
        0x00, 0xFF,0x2F,0x00
    };
    const char* path = "oss_midi_test.mid";
    { std::ofstream f(path, std::ios::binary);
      REQUIRE(f.write((const char*)smf.data(), (std::streamsize)smf.size())); }

    MidiFilePlayerNode node;
    auto eval = [&](Transport& t) {
        std::vector<Value> in(20);
        in[0] = std::string(path);
        in[1] = 0.0f;   // start offset
        in[2] = true;   // loop
        in[3] = 4.0f;   // loop length (bars)
        for (int i = 0; i < 16; ++i) in[4 + i] = false;   // no mutes
        std::vector<Value> out(1);
        EvalContext ctx{ in, out, 0.0f, &t };
        node.evaluate(ctx);
        MidiRef m = std::get<MidiRef>(out[0]);
        return std::vector<MidiEvent>(m.events, m.events + m.count);
    };
    Transport t; t.bpm = 120.0; t.play();   // 0.5 s/beat
    t.seconds = 0.0; eval(t);               // entry frame (also loads the file)
    t.seconds = 0.5; auto e = eval(t);      // 1 beat in -> window [0,1) includes beat 0
    std::remove(path);

    int ons = 0; int firstNote = -1;
    for (auto& x : e) if (midiIsNoteOn(x)) { ++ons; if (firstNote < 0) firstNote = x.data1; }
    CHECK(ons == 1);
    CHECK(firstNote == 60);
}

TEST_CASE("MidiFilePlayerNode loop length is a whole-number bars control") {
    MidiFilePlayerNode node;
    REQUIRE(node.inputs().size() > 3);
    const Port& p = node.inputs()[3];
    CHECK(p.name == "loop length");
    CHECK(p.type == PortType::Float);
    CHECK(p.integer == true);                              // renders as a SliderInt
    CHECK(p.minVal == doctest::Approx(1.0f));
    CHECK(p.maxVal == doctest::Approx(64.0f));
    CHECK(std::get<float>(p.defaultValue) == doctest::Approx(4.0f));
}

TEST_CASE("MidiFilePlayerNode releases a held note when the file changes") {
    // Two tiny SMFs, each a single note-on with NO note-off (so the note sounds until
    // it is explicitly released). They differ only in the note number.
    auto writeSmf = [](const char* path, unsigned char note) {
        std::vector<unsigned char> smf = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0x01,0xE0,
            'M','T','r','k', 0,0,0,8,
            0x00, 0x90, note, 0x64,        // note-on, channel 0
            0x00, 0xFF,0x2F,0x00           // end of track
        };
        std::ofstream f(path, std::ios::binary);
        f.write((const char*)smf.data(), (std::streamsize)smf.size());
    };
    const char* pathA = "oss_midi_change_a.mid";
    const char* pathB = "oss_midi_change_b.mid";
    writeSmf(pathA, 0x3C);   // note 60
    writeSmf(pathB, 0x3E);   // note 62

    MidiFilePlayerNode node;
    auto eval = [&](Transport& t, const char* file) {
        std::vector<Value> in(20);
        in[0] = std::string(file);
        in[1] = 0.0f; in[2] = true; in[3] = 4.0f;          // offset 0, loop on, 4 bars
        for (int i = 0; i < 16; ++i) in[4 + i] = false;     // no mutes
        std::vector<Value> out(1);
        EvalContext ctx{ in, out, 0.0f, &t };
        node.evaluate(ctx);
        MidiRef m = std::get<MidiRef>(out[0]);
        return std::vector<MidiEvent>(m.events, m.events + m.count);
    };

    Transport t; t.bpm = 120.0; t.play();
    t.seconds = 0.0; eval(t, pathA);                        // load A, enter
    t.seconds = 0.5; auto on = eval(t, pathA);              // note-on 60 now sounding
    REQUIRE(countNoteOns(on) == 1);
    t.seconds = 1.0; auto chg = eval(t, pathB);             // switch to B while 60 is held
    std::remove(pathA); std::remove(pathB);

    // The file change must release the note the OLD file left sounding (else it hangs).
    int offs = 0, offNote = -1;
    for (auto& x : chg) if (midiIsNoteOff(x)) { ++offs; offNote = x.data1; }
    CHECK(offs >= 1);
    CHECK(offNote == 60);
}

TEST_CASE("MidiClipPlayer reset releases sounding notes and re-enters fresh") {
    MidiSequence s; s.ok = true;
    s.events = { { 0.0, midiNoteOn(60, 100) } };   // note-on, no note-off -> stays sounding
    s.lengthBeats = 4.0;
    MidiClipPlayer p;
    bool mute[16] = {};
    p.advance(s, 0.0, 4, 0.0, true, 4, mute);                             // enter
    REQUIRE(countNoteOns(p.advance(s, 0.5, 4, 0.0, true, 4, mute)) == 1); // 60 sounding
    std::vector<MidiEvent> off;
    p.reset(off);
    CHECK(countNoteOffs(off) >= 1);                                       // reset released it
    CHECK(p.advance(s, 0.6, 4, 0.0, true, 4, mute).empty());             // re-enters fresh, emits nothing
}
