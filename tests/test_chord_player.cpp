#include <doctest/doctest.h>
#include "modules/ChordPlayerNode.h"
#include "core/Node.h"
#include "core/Transport.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

// 30 inputs: 24 step controls (C/oct4/maj), globals Auto/Bar/len8/pat1, switch=Bar, select=none.
static std::vector<Value> defaultInputs() {
    std::vector<Value> in(30);
    for (int p = 0; p < 8; ++p) {
        in[3 * p + 0] = 0.0f;   // root C
        in[3 * p + 1] = 4.0f;   // oct 4
        in[3 * p + 2] = 0.0f;   // chord maj
    }
    in[24] = 0.0f;   // mode Auto
    in[25] = 0.0f;   // quantize Bar
    in[26] = 8.0f;   // length 8
    in[27] = 0.0f;   // pattern 1 (index 0)
    in[28] = 2.0f;   // switch = Bar (irrelevant unless a preset switch is requested)
    in[29] = MidiRef{};   // select: no MIDI
    return in;
}

static std::vector<MidiEvent> runFrame(ChordPlayerNode& node, std::vector<Value>& in,
                                       Transport& t, float dt = 0.0f) {
    std::vector<Value> out(1);
    EvalContext ctx{in, out, dt, &t};
    node.evaluate(ctx);
    MidiRef m = std::get<MidiRef>(out[0]);
    return std::vector<MidiEvent>(m.events, m.events + m.count);
}

static int countNoteOns(const std::vector<MidiEvent>& e) {
    int n = 0; for (auto& x : e) if (midiIsNoteOn(x)) ++n; return n;
}
static int countNoteOffs(const std::vector<MidiEvent>& e) {
    int n = 0; for (auto& x : e) if (midiIsNoteOff(x)) ++n; return n;
}
static std::vector<int> noteOns(const std::vector<MidiEvent>& e) {
    std::vector<int> v; for (auto& x : e) if (midiIsNoteOn(x)) v.push_back(x.data1); return v;
}
static std::vector<int> noteOffs(const std::vector<MidiEvent>& e) {
    std::vector<int> v; for (auto& x : e) if (midiIsNoteOff(x)) v.push_back(x.data1); return v;
}

TEST_CASE("Chord Player fires pattern 1's chord on the first played frame") {
    ChordPlayerNode node;
    auto in = defaultInputs();                       // pattern 1 = C maj
    Transport t; t.bpm = 120.0; t.play(); t.seconds = 0.0;   // bar 0
    auto e = runFrame(node, in, t);
    REQUIRE(countNoteOns(e) == 3);
    CHECK(countNoteOffs(e) == 0);
    CHECK(noteOns(e) == std::vector<int>{60, 64, 67});
}

TEST_CASE("auto-progress switches chords at the next bar") {
    ChordPlayerNode node;
    auto in = defaultInputs();
    in[3 * 1 + 2] = 1.0f;                            // pattern 2 chord = min
    Transport t; t.bpm = 120.0; t.play();            // 120 bpm -> 2.0 s/bar
    t.seconds = 0.0; runFrame(node, in, t);          // bar 0: C maj on
    t.seconds = 2.0; auto e = runFrame(node, in, t); // bar 1: switch to pattern 2
    CHECK(noteOffs(e) == std::vector<int>{60, 64, 67});  // release exactly C maj
    CHECK(noteOns(e) == std::vector<int>{60, 63, 67});   // play C min
}

TEST_CASE("Beat quantize switches on each beat, not each bar") {
    ChordPlayerNode node;
    auto in = defaultInputs();
    in[25] = 1.0f;                                   // quantize = Beat
    in[3 * 1 + 2] = 1.0f;                            // pattern 2 chord = min
    Transport t; t.bpm = 120.0; t.play();            // 120 bpm -> 0.5 s/beat
    t.seconds = 0.0; runFrame(node, in, t);          // beat 0: C maj on
    t.seconds = 0.5; auto e = runFrame(node, in, t); // beat 1: switch to pattern 2
    CHECK(noteOffs(e) == std::vector<int>{60, 64, 67});  // release C maj
    CHECK(noteOns(e) == std::vector<int>{60, 63, 67});   // play C min
}

TEST_CASE("length 1 sustains the chord across bars") {
    ChordPlayerNode node;
    auto in = defaultInputs();
    in[26] = 1.0f;                                   // length 1
    in[3 * 1 + 2] = 1.0f;                            // pattern 2 differs (ignored at len 1)
    Transport t; t.bpm = 120.0; t.play();
    t.seconds = 0.0; runFrame(node, in, t);          // bar 0 fires C maj
    t.seconds = 2.0; auto e = runFrame(node, in, t); // bar 1: same pattern -> sustain
    CHECK(e.empty());
}

TEST_CASE("manual selection takes effect on the next boundary, not mid-bar") {
    ChordPlayerNode node;
    auto in = defaultInputs();
    in[24] = 1.0f;                                   // mode Manual
    in[3 * 2 + 2] = 1.0f;                            // pattern 3 chord = min
    Transport t; t.bpm = 120.0; t.play();
    t.seconds = 0.0; runFrame(node, in, t);          // bar 0: pattern 1 (C maj)
    in[27] = 2.0f;                                   // select pattern 3 (index 2) mid-bar
    t.seconds = 1.0; auto mid = runFrame(node, in, t);   // still bar 0 -> no switch
    CHECK(mid.empty());
    t.seconds = 2.0; auto next = runFrame(node, in, t);  // bar 1 -> switch to pattern 3
    CHECK(countNoteOffs(next) == 3);
    CHECK(noteOns(next) == std::vector<int>{60, 63, 67});   // C min
}

TEST_CASE("pausing the transport flushes all sounding notes") {
    ChordPlayerNode node;
    auto in = defaultInputs();
    Transport t; t.bpm = 120.0; t.play();
    t.seconds = 0.0; runFrame(node, in, t);          // C maj sounding
    t.pause();       auto e = runFrame(node, in, t); // not playing -> flush
    CHECK(countNoteOffs(e) == 3);
    CHECK(countNoteOns(e) == 0);
    auto e2 = runFrame(node, in, t);                 // still paused -> nothing left
    CHECK(e2.empty());
}

TEST_CASE("a button selects a preset immediately and re-fires the chord") {
    ChordPlayerNode node;
    auto in = defaultInputs();
    in[28] = 0.0f;                                   // switch = Immediate
    Transport t; t.bpm = 120.0; t.play(); t.seconds = 0.0;
    runFrame(node, in, t);                           // preset 1 plays C maj
    CHECK(node.buttonActive() == 0);
    node.onButtonPressed(2);                         // request preset 3 (jazz default content)
    CHECK(node.buttonPending() == 2);
    auto e = runFrame(node, in, t);                  // immediate switch + re-fire
    CHECK(node.buttonActive() == 2);
    CHECK(node.buttonPending() == -1);
    CHECK(countNoteOffs(e) == 3);                    // released the old C maj
    CHECK(countNoteOns(e) >= 1);                     // fired preset 3's chord
    CHECK(noteOns(e) != std::vector<int>{60, 64, 67});   // and it is not C maj
}

TEST_CASE("Bar switch quantize defers the preset change to the next bar") {
    ChordPlayerNode node;
    auto in = defaultInputs();
    in[28] = 2.0f;                                   // switch = Bar
    Transport t; t.bpm = 120.0; t.play();            // 2.0 s/bar
    t.seconds = 0.0; runFrame(node, in, t);          // bar 0, preset 1
    node.onButtonPressed(1);                         // request preset 2 mid-bar
    t.seconds = 1.0; runFrame(node, in, t);          // still bar 0 -> no switch
    CHECK(node.buttonActive() == 0);
    CHECK(node.buttonPending() == 1);
    t.seconds = 2.0; runFrame(node, in, t);          // bar 1 -> switch
    CHECK(node.buttonActive() == 1);
    CHECK(node.buttonPending() == -1);
}

TEST_CASE("a select-MIDI note switches preset (C..G), other notes ignored") {
    ChordPlayerNode node;
    auto in = defaultInputs();
    in[28] = 0.0f;                                   // Immediate
    Transport t; t.bpm = 120.0; t.play(); t.seconds = 0.0;
    runFrame(node, in, t);                           // preset 1 active
    std::vector<MidiEvent> sel = { midiNoteOn(64, 100) };   // 64 = E, pc 4 -> preset index 4
    in[29] = MidiRef{ sel.data(), sel.size() };
    runFrame(node, in, t);
    CHECK(node.buttonActive() == 4);
    std::vector<MidiEvent> sel2 = { midiNoteOn(70, 100) };  // 70 = A#, pc 10 -> ignored
    in[29] = MidiRef{ sel2.data(), sel2.size() };
    runFrame(node, in, t);
    CHECK(node.buttonActive() == 4);                 // unchanged
    in[29] = MidiRef{};
}

TEST_CASE("saveState / loadState round-trips presets (incl. edits) and active index") {
    ChordPlayerNode a;
    auto in = defaultInputs(); in[28] = 0.0f;        // Immediate
    Transport t; t.bpm = 120.0; t.play(); t.seconds = 0.0;
    in[3 * 0 + 2] = 8.0f;                            // edit active preset (0) step 0 chord -> dom7
    runFrame(a, in, t);                              // captureActive stores the edit into presets_[0]
    std::string s = a.saveState();
    ChordPlayerNode b; b.loadState(s);
    CHECK(b.saveState() == s);                       // non-default content round-trips faithfully

    ChordPlayerNode c;                               // active index survives
    c.onButtonPressed(5);
    auto in2 = defaultInputs(); in2[28] = 0.0f;
    Transport t2;                                    // not playing -> immediate switch
    runFrame(c, in2, t2);
    ChordPlayerNode d; d.loadState(c.saveState());
    CHECK(d.buttonActive() == 5);

    ChordPlayerNode e; e.loadState("");              // empty load leaves constructor defaults intact
    CHECK(e.buttonActive() == 0);
}

TEST_CASE("each default preset holds a playable chord") {
    ChordPlayerNode node;
    auto in = defaultInputs(); in[28] = 0.0f;        // Immediate
    Transport t; t.bpm = 120.0; t.play(); t.seconds = 0.0;
    runFrame(node, in, t);
    for (int p = 1; p < 8; ++p) {                    // presets 2..8 keep their defaults until active
        node.onButtonPressed(p);
        auto ev = runFrame(node, in, t);
        CHECK(node.buttonActive() == p);
        CHECK(countNoteOns(ev) >= 1);                // a valid chord fired on switch
    }
}

TEST_CASE("the button hook reports counts and selection") {
    ChordPlayerNode node;
    CHECK(node.buttonCount() == 8);
    CHECK(node.buttonLabel(0) == "1");
    CHECK(node.buttonLabel(7) == "8");
    CHECK(node.buttonActive() == 0);
    node.onButtonPressed(3);
    CHECK(node.buttonPending() == 3);                // requested but not yet applied
}
