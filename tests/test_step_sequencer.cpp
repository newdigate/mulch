#include <doctest/doctest.h>
#include "modules/StepSequencerNode.h"
#include "core/Node.h"
#include "core/Value.h"
#include <array>
#include <vector>

using namespace oss;

// Drive one evaluate() with a 16-step pattern + params; return generated events.
static std::vector<MidiEvent> step(StepSequencerNode& seq, const std::array<bool, 16>& pat,
                                   float dt, float tempo = 120.0f, float note = 36.0f,
                                   float channel = 9.0f) {
    std::vector<Value> in(19);
    for (int i = 0; i < 16; ++i) in[i] = pat[i];
    in[16] = tempo; in[17] = note; in[18] = channel;
    std::vector<Value> out(1);
    EvalContext ctx{in, out, dt};
    seq.evaluate(ctx);
    MidiRef o = std::get<MidiRef>(out[0]);
    return std::vector<MidiEvent>(o.events, o.events + o.count);
}

static int countNoteOns(const std::vector<MidiEvent>& evs) {
    int n = 0;
    for (auto& e : evs) if (midiIsNoteOn(e)) ++n;
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
