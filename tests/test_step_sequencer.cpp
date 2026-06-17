#include <doctest/doctest.h>
#include "modules/StepSequencerNode.h"
#include "core/Node.h"
#include "core/Transport.h"
#include "core/StepSync.h"
#include "core/Value.h"
#include <array>
#include <vector>

using namespace oss;

// Drive one free-mode evaluate() with a 16-step pattern + params; return events.
static std::vector<MidiEvent> step(StepSequencerNode& seq, const std::array<bool, 16>& pat,
                                   float dt, float tempo = 120.0f, float note = 36.0f,
                                   float channel = 9.0f) {
    std::vector<Value> in(21);
    for (int i = 0; i < 16; ++i) in[i] = pat[i];
    in[16] = tempo; in[17] = note; in[18] = channel;
    in[19] = false;                              // sync off (free mode)
    in[20] = (float)kStepDivisionDefault;        // rate sync (unused when free)
    std::vector<Value> out(1);
    EvalContext ctx{in, out, dt};
    seq.evaluate(ctx);
    MidiRef o = std::get<MidiRef>(out[0]);
    return std::vector<MidiEvent>(o.events, o.events + o.count);
}

// Drive one synced evaluate() at a transport bar position (seconds) + division.
static std::vector<MidiEvent> syncStep(StepSequencerNode& seq, const std::array<bool, 16>& pat,
                                       Transport& t, double seconds, int div) {
    std::vector<Value> in(21);
    for (int i = 0; i < 16; ++i) in[i] = pat[i];
    in[16] = 120.0f; in[17] = 36.0f; in[18] = 9.0f;
    in[19] = true; in[20] = (float)div;
    std::vector<Value> out(1);
    t.seconds = seconds;
    EvalContext ctx{in, out, 0.0f, &t};
    seq.evaluate(ctx);
    MidiRef o = std::get<MidiRef>(out[0]);
    return std::vector<MidiEvent>(o.events, o.events + o.count);
}

static int countNoteOns(const std::vector<MidiEvent>& evs) {
    int n = 0;
    for (auto& e : evs) if (midiIsNoteOn(e)) ++n;
    return n;
}
static int countNoteOffs(const std::vector<MidiEvent>& evs) {
    int n = 0;
    for (auto& e : evs) if (midiIsNoteOff(e)) ++n;
    return n;
}

// Run a whole 16-step bar at 120 BPM (period 0.125s, bar = 2.0s) in 10ms frames,
// stopping just shy of the bar's end so the next bar's step 0 doesn't re-trigger.
static int noteOnsInOneBar(StepSequencerNode& seq, const std::array<bool, 16>& pat,
                           float note = 36.0f, float channel = 9.0f) {
    int total = countNoteOns(step(seq, pat, 0.001f, 120.0f, note, channel));
    for (int i = 0; i < 198; ++i)   // 0.001 + 198*0.01 = 1.981s  (< 2.0s)
        total += countNoteOns(step(seq, pat, 0.01f, 120.0f, note, channel));
    return total;
}

TEST_CASE("four-on-the-floor default fires four kicks per bar") {
    StepSequencerNode seq;
    std::array<bool, 16> pat{};
    for (int i = 0; i < 16; ++i) pat[i] = (i % 4) == 0;   // steps 0,4,8,12
    CHECK(noteOnsInOneBar(seq, pat) == 4);
}

TEST_CASE("only the enabled steps trigger") {
    StepSequencerNode seq;
    std::array<bool, 16> pat{};
    pat[0] = pat[8] = true;                               // two hits per bar
    CHECK(noteOnsInOneBar(seq, pat) == 2);
}

TEST_CASE("no enabled steps means no output") {
    StepSequencerNode seq;
    std::array<bool, 16> pat{};                            // all off
    CHECK(noteOnsInOneBar(seq, pat) == 0);
}

TEST_CASE("note-on carries the configured note and channel") {
    StepSequencerNode seq;
    std::array<bool, 16> pat{};
    pat[0] = true;
    auto e = step(seq, pat, 0.001f, 120.0f, 38.0f, 2.0f);  // note 38, channel 2
    bool found = false;
    for (auto& ev : e)
        if (midiIsNoteOn(ev)) {
            CHECK((ev.status & 0x0F) == 2);                // channel 2
            CHECK(ev.data1 == 38);                         // note 38
            found = true;
        }
    CHECK(found);
}

TEST_CASE("faster tempo packs more steps into the same wall-clock time") {
    std::array<bool, 16> all{};
    for (auto& b : all) b = true;                          // every step on
    StepSequencerNode slow, fast;
    int slowHits = 0, fastHits = 0;
    for (int i = 0; i < 50; ++i) {                         // 0.5s of audio time
        slowHits += countNoteOns(step(slow, all, 0.01f, 120.0f));
        fastHits += countNoteOns(step(fast, all, 0.01f, 240.0f));
    }
    CHECK(fastHits > slowHits);
}

TEST_CASE("synced sequencer fires steps on transport bar boundaries") {
    StepSequencerNode seq;
    std::array<bool, 16> pat{};
    pat[0] = pat[1] = true;                                // steps 0 and 1 on
    Transport t; t.bpm = 120.0; t.play();                  // 2 s/bar
    // div 0 = 1/4 = 0.25 bar/step = 0.5 s/step.
    CHECK(countNoteOns(syncStep(seq, pat, t, 0.0, 0)) == 1);   // bars 0   -> step 0 (downbeat)
    auto e1 = syncStep(seq, pat, t, 0.5, 0);                   // bars 0.25 -> step 1
    CHECK(countNoteOns(e1) == 1);
    CHECK(countNoteOffs(e1) == 1);                             // released step 0 first
    CHECK(countNoteOns(syncStep(seq, pat, t, 1.0, 0)) == 0);   // bars 0.5  -> step 2 (off)
}

TEST_CASE("a paused transport produces no synced steps") {
    StepSequencerNode seq;
    std::array<bool, 16> pat{};
    pat[0] = true;
    Transport t; t.bpm = 120.0; t.pause();
    CHECK(countNoteOns(syncStep(seq, pat, t, 0.0, 0)) == 0);
}

TEST_CASE("synced note releases on its gate within the step") {
    StepSequencerNode seq;
    std::array<bool, 16> pat{};
    pat[0] = true;
    Transport t; t.bpm = 120.0; t.play();                     // 2 s/bar; div 0 = 0.5 s/step
    CHECK(countNoteOns(syncStep(seq, pat, t, 0.0, 0)) == 1);   // bars 0 -> step 0 on
    // bars 0.125 -> frac 0.5 of a 1/4 step == kGate: release, same step, no new note
    auto e = syncStep(seq, pat, t, 0.25, 0);
    CHECK(countNoteOffs(e) == 1);
    CHECK(countNoteOns(e) == 0);
}

TEST_CASE("synced sequencer fires the loop-start step when the transport wraps") {
    StepSequencerNode seq;
    std::array<bool, 16> pat{};
    pat[0] = pat[4] = true;                                   // steps 0 and 4 on
    Transport t; t.bpm = 120.0; t.play();
    // div 0 = 1/4: bars 1.0 -> stepAbs 4 (step 4 on)
    CHECK(countNoteOns(syncStep(seq, pat, t, 2.0, 0)) == 1);   // seconds 2.0 -> bars 1.0
    // loop wraps the position back to bar 0 -> stepAbs drops 4 -> 0; step 0 fires
    auto wrap = syncStep(seq, pat, t, 0.0, 0);
    CHECK(countNoteOns(wrap) == 1);                            // step 0 (loop start)
    CHECK(countNoteOffs(wrap) == 1);                           // released step 4 first
}

TEST_CASE("stopping then resuming re-fires the step at the current position") {
    StepSequencerNode seq;
    std::array<bool, 16> pat{};
    pat[0] = true;
    Transport t; t.bpm = 120.0; t.play();
    CHECK(countNoteOns(syncStep(seq, pat, t, 0.0, 0)) == 1);   // playing: step 0 fires
    t.pause();
    CHECK(countNoteOffs(syncStep(seq, pat, t, 0.0, 0)) == 1);  // paused: release, re-prime
    t.play();
    CHECK(countNoteOns(syncStep(seq, pat, t, 0.0, 0)) == 1);   // resume: step 0 fires again
}
