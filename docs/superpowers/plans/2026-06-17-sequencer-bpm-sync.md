# Step Sequencer + Arpeggiator BPM Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Step Sequencer and Arpeggiator an opt-in `sync` toggle that locks their step rate to the global transport — each step derived statelessly from `transport.bars()` at a musical division — while leaving free mode unchanged.

**Architecture:** A shared GL-free `core/StepSync.h` holds the 8 step divisions (bars-per-step). Each node gains two appended input ports (`sync` Bool, `rate sync` choice) and an `evaluate` branch: sync on → position-derived step (loop-robust); sync off → the existing free clock, untouched.

**Tech Stack:** C++17, doctest. GL-free core/modules.

**Spec:** `docs/superpowers/specs/2026-06-17-sequencer-bpm-sync-design.md`

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/core/StepSync.h` | 8 step-division labels + bars-per-step + default index | **create** (header-only) |
| `src/modules/StepSequencerNode.h` | +`sync`/`rate sync` ports + sync branch | modify |
| `src/modules/ArpeggiatorNode.h` | +`sync`/`rate sync` ports + sync branch | modify |
| `tests/test_step_sync.cpp` | division table values + clamping | **create** |
| `tests/test_step_sequencer.cpp` | bump helper to 21 inputs; add sync cases | modify |
| `tests/test_arpeggiator.cpp` | bump helper to 7 inputs; add sync cases | modify |
| `CMakeLists.txt` | add `tests/test_step_sync.cpp` to `core_tests` | modify |
| `README.md`, `CLAUDE.md` | document the sync toggle | modify |

Each task ends green (build + `ctest`).

---

### Task 1: `StepSync.h` — shared step-division table

**Files:**
- Create: `src/core/StepSync.h`
- Test: `tests/test_step_sync.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_step_sync.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/StepSync.h"

using namespace oss;

TEST_CASE("step division table has 8 entries with a 1/16 default") {
    CHECK(stepDivisionLabels().size() == 8);
    CHECK(kStepDivisionDefault == 4);
    CHECK(stepDivisionLabels()[kStepDivisionDefault] == "1/16");
}

TEST_CASE("step division bars are the right musical lengths (4/4)") {
    CHECK(stepDivisionBars(0) == doctest::Approx(0.25));        // 1/4 (a beat)
    CHECK(stepDivisionBars(1) == doctest::Approx(0.125));       // 1/8
    CHECK(stepDivisionBars(2) == doctest::Approx(0.1875));      // 1/8. dotted
    CHECK(stepDivisionBars(3) == doctest::Approx(0.0833333));   // 1/8T triplet
    CHECK(stepDivisionBars(4) == doctest::Approx(0.0625));      // 1/16
    CHECK(stepDivisionBars(5) == doctest::Approx(0.09375));     // 1/16. dotted
    CHECK(stepDivisionBars(6) == doctest::Approx(0.0416667));   // 1/16T triplet
    CHECK(stepDivisionBars(7) == doctest::Approx(0.03125));     // 1/32
}

TEST_CASE("step division index is clamped to a valid range") {
    CHECK(stepDivisionBars(-5) == doctest::Approx(0.25));       // clamps to 0
    CHECK(stepDivisionBars(99) == doctest::Approx(0.03125));    // clamps to 7
}
```

- [ ] **Step 2: Wire the test into CMake**

In `CMakeLists.txt`, in the `add_executable(core_tests ...)` list, add after
`  tests/test_lfo.cpp`:

```cmake
  tests/test_step_sync.cpp
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: compile error — `core/StepSync.h` does not exist.

- [ ] **Step 4: Create the header**

Create `src/core/StepSync.h`:

```cpp
#pragma once
#include <string>
#include <vector>

namespace oss {

// Per-step musical divisions for transport-synced sequencers/arps (length of one
// step). Index matches stepDivisionBars(). Assumes 4/4 (beatsPerBar = 4).
inline const std::vector<std::string>& stepDivisionLabels() {
    static const std::vector<std::string> labels = {
        "1/4", "1/8", "1/8.", "1/8T", "1/16", "1/16.", "1/16T", "1/32"
    };
    return labels;
}

// Length of one step in bars for division index `idx` (clamped to a valid index).
inline double stepDivisionBars(int idx) {
    static const double bars[8] = {
        0.25,                 // 1/4   (a beat)
        0.125,                // 1/8
        0.1875,               // 1/8.  (dotted = 1.5x)
        0.125 * 2.0 / 3.0,    // 1/8T  (triplet = 2/3x)
        0.0625,               // 1/16
        0.09375,              // 1/16. (dotted)
        0.0625 * 2.0 / 3.0,   // 1/16T (triplet)
        0.03125               // 1/32
    };
    if (idx < 0) idx = 0;
    if (idx > 7) idx = 7;
    return bars[idx];
}

inline constexpr int kStepDivisionDefault = 4;   // "1/16"

} // namespace oss
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build -j && ./build/core_tests --test-case="*step division*"`
Expected: PASS (3 cases).

- [ ] **Step 6: Full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/core/StepSync.h tests/test_step_sync.cpp CMakeLists.txt
git commit -m "feat(core): add shared step-division table for transport sync"
```

---

### Task 2: Step Sequencer transport sync

**Files:**
- Modify: `src/modules/StepSequencerNode.h`
- Test: `tests/test_step_sequencer.cpp`

- [ ] **Step 1: Update the test helper + add sync tests**

Replace the entire contents of `tests/test_step_sequencer.cpp` with:

```cpp
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
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -8`
Expected: the two `synced ...` cases fail. The old node ignores ports 19/20, so the
synced helper falls through to the free clock with `dt=0`; it can't step to step 1
or release on a bar boundary (and triggers regardless of `playing`), so the
boundary and `paused` assertions fail. The build still succeeds — the node simply
doesn't read indices 19/20 yet.

- [ ] **Step 3: Rewrite `StepSequencerNode.h`**

Replace the entire contents of `src/modules/StepSequencerNode.h` with:

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/StepSync.h"

namespace oss {

// 16-step drum sequencer -- a very basic TR-909 voice. A playhead advances through
// 16 on/off steps, triggering a single note on a single channel each time it lands
// on an enabled step. Outputs MIDI. GL-free.
//
// Two timing modes:
//   - free (sync off): the playhead runs off ctx.dt at the `tempo` BPM (16th notes).
//   - sync (sync on):  each step is derived from the transport bar position at the
//     selected `rate sync` division, so it locks to the project tempo, starts/stops
//     with the transport, and stays bar-aligned (loop-robust).
//
// Inputs: 0..15 = step toggles (Bool), 16 = tempo (BPM), 17 = note, 18 = channel,
//         19 = sync (Bool), 20 = rate sync (choice: step division).
class StepSequencerNode : public Node {
public:
    StepSequencerNode() : Node("Step Seq") {
        for (int i = 0; i < kSteps; ++i)
            addInput(std::to_string(i + 1), PortType::Bool, (i % 4) == 0);  // four-on-the-floor
        addInput("tempo",   PortType::Float, 120.0f, 40.0f, 240.0f);   // BPM (free mode)
        addInput("note",    PortType::Float, 36.0f,   0.0f, 127.0f);   // 36 = GM kick
        addInput("channel", PortType::Float, 9.0f,    0.0f,  15.0f);   // 9 = GM drums (ch 10)
        addInput("sync",    PortType::Bool, false);                    // lock to transport BPM
        addChoiceInput("rate sync", stepDivisionLabels(), kStepDivisionDefault);  // step length
        addOutput("midi", PortType::Midi);
    }

    void evaluate(EvalContext& ctx) override {
        out_.clear();

        int  note    = std::clamp((int)std::lround(ctx.in<float>(kSteps + 1)), 0, 127);
        int  channel = std::clamp((int)std::lround(ctx.in<float>(kSteps + 2)), 0, 15);
        bool sync    = ctx.in<bool>(kSteps + 3);
        int  div     = std::clamp((int)std::lround(ctx.in<float>(kSteps + 4)), 0, 7);

        if (sync) {
            // --- transport-synced: derive the step from the bar position ---
            const double barsPerStep = stepDivisionBars(div);
            if (ctx.transport && ctx.transport->playing && barsPerStep > 0.0) {
                double    stepPos  = ctx.transport->bars() / barsPerStep;
                long long stepAbs  = (long long)std::floor(stepPos);
                double    frac     = stepPos - (double)stepAbs;
                bool      boundary = !primed_ || stepAbs != lastStepAbs_;

                if (active_ >= 0 && (boundary || frac >= kGate)) {
                    out_.push_back(midiNoteOff(active_, activeCh_));
                    active_ = -1;
                }
                if (boundary) {
                    int s = (int)(((stepAbs % kSteps) + kSteps) % kSteps);
                    if (ctx.in<bool>((std::size_t)s)) {
                        out_.push_back(midiNoteOn(note, 100, channel));
                        active_ = note; activeCh_ = channel;
                    }
                    lastStepAbs_ = stepAbs;
                    primed_ = true;
                }
            } else {
                // paused/stopped: release any held note and re-prime so resume fires cleanly
                if (active_ >= 0) { out_.push_back(midiNoteOff(active_, activeCh_)); active_ = -1; }
                primed_ = false;
            }
        } else {
            // --- free-running: tempo-based clock (unchanged) ---
            float tempo = ctx.in<float>(kSteps + 0);
            if (tempo < 1.0f) tempo = 1.0f;
            const double period = 15.0 / tempo;   // seconds per 16th note (4 steps per beat)

            clock_ += ctx.dt;
            if (active_ >= 0 && clock_ >= noteOff_) {
                out_.push_back(midiNoteOff(active_, activeCh_));
                active_ = -1;
            }
            while (clock_ >= nextStep_) {
                if (active_ >= 0) { out_.push_back(midiNoteOff(active_, activeCh_)); active_ = -1; }
                if (ctx.in<bool>((std::size_t)step_)) {
                    out_.push_back(midiNoteOn(note, 100, channel));
                    active_   = note;
                    activeCh_ = channel;
                    noteOff_  = nextStep_ + kGate * period;
                }
                step_ = (step_ + 1) % kSteps;
                nextStep_ += period;
            }
        }

        ctx.out<MidiRef>(0, MidiRef{out_.data(), out_.size()});
    }

private:
    static constexpr int    kSteps = 16;
    static constexpr double kGate  = 0.5;   // note length as a fraction of a step
    std::vector<MidiEvent> out_;            // events produced this frame (owns MidiRef storage)
    double clock_    = 0.0;
    double nextStep_ = 0.0;
    double noteOff_  = 0.0;
    int    step_     = 0;                   // next step to trigger (free mode)
    int    active_   = -1;                  // currently sounding note, or -1
    int    activeCh_ = 0;
    long long lastStepAbs_ = 0;             // last synced step index (sync mode)
    bool      primed_      = false;         // synced playback has fired its first step
};

} // namespace oss
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build -j && ./build/core_tests --test-case="*sequencer*,*kicks*,*enabled steps*,*no output*,*note and channel*,*faster tempo*,*paused transport*"`
Expected: PASS (all Step Seq cases, free + synced).

- [ ] **Step 5: Full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add src/modules/StepSequencerNode.h tests/test_step_sequencer.cpp
git commit -m "feat(modules): Step Seq transport BPM sync (opt-in)"
```

---

### Task 3: Arpeggiator transport sync

**Files:**
- Modify: `src/modules/ArpeggiatorNode.h`
- Test: `tests/test_arpeggiator.cpp`

- [ ] **Step 1: Update the test helper + add sync test**

Replace the entire contents of `tests/test_arpeggiator.cpp` with:

```cpp
#include <doctest/doctest.h>
#include "modules/ArpeggiatorNode.h"
#include "core/Node.h"
#include "core/Transport.h"
#include "core/StepSync.h"
#include "core/Value.h"
#include <string>
#include <vector>

using namespace oss;

// Drive one free-mode evaluate() with the given input events + dt; return events.
static std::vector<MidiEvent> step(ArpeggiatorNode& arp,
                                   const std::vector<MidiEvent>& inEvents, float dt,
                                   float rate = 10.0f, float gate = 0.5f,
                                   float octaves = 1.0f, float mode = 0.0f) {
    std::vector<Value> in(7);
    in[0] = MidiRef{inEvents.data(), inEvents.size()};
    in[1] = rate; in[2] = gate; in[3] = octaves; in[4] = mode;
    in[5] = false;                              // sync off (free mode)
    in[6] = (float)kStepDivisionDefault;        // rate sync (unused when free)
    std::vector<Value> out(1);
    EvalContext ctx{in, out, dt};
    arp.evaluate(ctx);
    MidiRef o = std::get<MidiRef>(out[0]);
    return std::vector<MidiEvent>(o.events, o.events + o.count);
}

// Drive one synced evaluate() at a transport bar position (seconds) + division.
static std::vector<MidiEvent> syncStep(ArpeggiatorNode& arp,
                                       const std::vector<MidiEvent>& inEvents,
                                       Transport& t, double seconds, int div) {
    std::vector<Value> in(7);
    in[0] = MidiRef{inEvents.data(), inEvents.size()};
    in[1] = 10.0f; in[2] = 0.5f; in[3] = 1.0f; in[4] = 0.0f;
    in[5] = true; in[6] = (float)div;
    std::vector<Value> out(1);
    t.seconds = seconds;
    EvalContext ctx{in, out, 0.0f, &t};
    arp.evaluate(ctx);
    MidiRef o = std::get<MidiRef>(out[0]);
    return std::vector<MidiEvent>(o.events, o.events + o.count);
}

static std::vector<int> noteOns(const std::vector<MidiEvent>& evs) {
    std::vector<int> ns;
    for (auto& e : evs) if (midiIsNoteOn(e)) ns.push_back(e.data1);
    return ns;
}

TEST_CASE("MidiRef maps to PortType::Midi") {
    Value v = MidiRef{};
    CHECK(typeOf(v) == PortType::Midi);
    CHECK(std::string(portTypeName(PortType::Midi)) == "Midi");
}

TEST_CASE("arpeggiator steps up through held notes and cycles") {
    ArpeggiatorNode arp;
    std::vector<MidiEvent> chord = {midiNoteOn(60, 100), midiNoteOn(64, 100)};
    std::vector<int> ons = noteOns(step(arp, chord, 0.001f));   // first step fires
    for (int i = 0; i < 40; ++i)                                // ~0.4s -> several steps
        for (int n : noteOns(step(arp, {}, 0.01f))) ons.push_back(n);
    REQUIRE(ons.size() >= 4);
    CHECK(ons[0] == 60);
    CHECK(ons[1] == 64);
    CHECK(ons[2] == 60);
    CHECK(ons[3] == 64);
}

TEST_CASE("arpeggiator releases a note after its gate time") {
    ArpeggiatorNode arp;
    step(arp, {midiNoteOn(60, 100)}, 0.001f, 10.0f, 0.5f);     // note-on 60; off due ~0.05s
    auto e = step(arp, {}, 0.06f, 10.0f, 0.5f);                 // advance past the gate
    bool sawOff60 = false;
    for (auto& ev : e) if (midiIsNoteOff(ev) && ev.data1 == 60) sawOff60 = true;
    CHECK(sawOff60);
}

TEST_CASE("arpeggiator spans octaves") {
    ArpeggiatorNode arp;
    std::vector<int> seq = noteOns(step(arp, {midiNoteOn(60, 100)}, 0.001f, 10, 0.5f, 2.0f));
    for (int i = 0; i < 30; ++i)
        for (int n : noteOns(step(arp, {}, 0.01f, 10, 0.5f, 2.0f))) seq.push_back(n);
    REQUIRE(seq.size() >= 4);
    CHECK(seq[0] == 60);
    CHECK(seq[1] == 72);   // +1 octave
    CHECK(seq[2] == 60);
    CHECK(seq[3] == 72);
}

TEST_CASE("arpeggiator down mode descends") {
    ArpeggiatorNode arp;
    std::vector<MidiEvent> chord = {midiNoteOn(60, 100), midiNoteOn(64, 100)};
    std::vector<int> seq = noteOns(step(arp, chord, 0.001f, 10, 0.5f, 1.0f, 1.0f /*down*/));
    for (int i = 0; i < 20; ++i)
        for (int n : noteOns(step(arp, {}, 0.01f, 10, 0.5f, 1.0f, 1.0f))) seq.push_back(n);
    REQUIRE(seq.size() >= 3);
    CHECK(seq[0] == 64);   // highest first
    CHECK(seq[1] == 60);
    CHECK(seq[2] == 64);
}

TEST_CASE("arpeggiator emits nothing with no held notes") {
    ArpeggiatorNode arp;
    for (int i = 0; i < 10; ++i) CHECK(step(arp, {}, 0.02f).empty());
}

TEST_CASE("synced arpeggiator steps on transport bar boundaries") {
    ArpeggiatorNode arp;
    std::vector<MidiEvent> chord = {midiNoteOn(60, 100), midiNoteOn(64, 100)};
    Transport t; t.bpm = 120.0; t.play();          // 2 s/bar; div 0 = 1/4 = 0.5 s/step
    auto a = noteOns(syncStep(arp, chord, t, 0.0, 0));   // bars 0    -> 60 (downbeat)
    REQUIRE(a.size() == 1); CHECK(a[0] == 60);
    auto b = noteOns(syncStep(arp, {}, t, 0.5, 0));       // bars 0.25 -> 64
    REQUIRE(b.size() == 1); CHECK(b[0] == 64);
    auto c = noteOns(syncStep(arp, {}, t, 1.0, 0));       // bars 0.5  -> 60
    REQUIRE(c.size() == 1); CHECK(c[0] == 60);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j 2>&1 | tail -8`
Expected: the `synced arpeggiator ...` case fails. The old node ignores ports 5/6,
so the synced helper falls through to the free clock with `dt=0`; after the first
step it can't advance on a bar boundary, so the `b`/`c` assertions fail. The build
still succeeds.

- [ ] **Step 3: Rewrite `ArpeggiatorNode.h`**

Replace the entire contents of `src/modules/ArpeggiatorNode.h` with:

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <set>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/StepSync.h"

namespace oss {

// Arpeggiator: tracks the notes currently held on its MIDI input and steps through
// them, emitting generated note-on/note-off events on its MIDI output. GL-free.
//
// Two timing modes:
//   - free (sync off): steps at `rate` steps per second off ctx.dt.
//   - sync (sync on):  each step is derived from the transport bar position at the
//     selected `rate sync` division, locking to the project tempo (loop-robust).
//
// Inputs: 0 = midi, 1 = rate, 2 = gate, 3 = octaves, 4 = mode,
//         5 = sync (Bool), 6 = rate sync (choice: step division).
class ArpeggiatorNode : public Node {
public:
    ArpeggiatorNode() : Node("Arpeggiator") {
        addInput("midi",    PortType::Midi, MidiRef{});
        addInput("rate",    PortType::Float, 8.0f, 0.5f, 20.0f);   // steps per second (free)
        addInput("gate",    PortType::Float, 0.5f, 0.05f, 1.0f);   // note length fraction
        addInput("octaves", PortType::Float, 1.0f, 1.0f, 4.0f);    // span (rounded)
        addInput("mode",    PortType::Float, 0.0f, 0.0f, 2.0f);    // 0 up, 1 down, 2 up-down
        addInput("sync",    PortType::Bool, false);                // lock to transport BPM
        addChoiceInput("rate sync", stepDivisionLabels(), kStepDivisionDefault);  // step length
        addOutput("midi", PortType::Midi);
    }

    void evaluate(EvalContext& ctx) override {
        out_.clear();

        // 1) Fold incoming note on/off into the held-note set.
        MidiRef in = ctx.in<MidiRef>(0);
        for (std::size_t i = 0; i < in.count; ++i) {
            const MidiEvent& e = in.events[i];
            if (midiIsNoteOn(e))       held_.insert(e.data1);
            else if (midiIsNoteOff(e)) held_.erase(e.data1);
        }

        float gate    = std::clamp(ctx.in<float>(2), 0.0f, 1.0f);
        int   octaves = std::clamp((int)std::lround(ctx.in<float>(3)), 1, 4);
        int   mode    = std::clamp((int)std::lround(ctx.in<float>(4)), 0, 2);
        bool  sync    = ctx.in<bool>(5);
        int   div     = std::clamp((int)std::lround(ctx.in<float>(6)), 0, 7);

        std::vector<int> seq = buildSequence(octaves, mode);

        if (sync) {
            // --- transport-synced: derive the step from the bar position ---
            const double barsPerStep = stepDivisionBars(div);
            if (ctx.transport && ctx.transport->playing && barsPerStep > 0.0) {
                double    stepPos  = ctx.transport->bars() / barsPerStep;
                long long stepAbs  = (long long)std::floor(stepPos);
                double    frac     = stepPos - (double)stepAbs;
                bool      boundary = !primed_ || stepAbs != lastStepAbs_;

                if (active_ >= 0 && (boundary || frac >= gate)) {
                    out_.push_back(midiNoteOff(active_));
                    active_ = -1;
                }
                if (boundary) {
                    if (!seq.empty()) {
                        int note = seq[step_ % seq.size()];
                        out_.push_back(midiNoteOn(note, 100));
                        active_ = note;
                        ++step_;
                    }
                    lastStepAbs_ = stepAbs;
                    primed_ = true;
                }
            } else {
                // paused/stopped: release any held note and re-prime so resume fires cleanly
                if (active_ >= 0) { out_.push_back(midiNoteOff(active_)); active_ = -1; }
                primed_ = false;
            }
        } else {
            // --- free-running: rate (steps/sec) clock (unchanged) ---
            float rate = ctx.in<float>(1);
            if (rate < 0.01f) rate = 0.01f;
            const double period = 1.0 / rate;

            clock_ += ctx.dt;
            if (active_ >= 0 && clock_ >= noteOff_) {
                out_.push_back(midiNoteOff(active_));
                active_ = -1;
            }
            while (clock_ >= nextStep_) {
                if (active_ >= 0) { out_.push_back(midiNoteOff(active_)); active_ = -1; }
                if (!seq.empty()) {
                    int note = seq[step_ % seq.size()];
                    out_.push_back(midiNoteOn(note, 100));
                    active_  = note;
                    noteOff_ = nextStep_ + gate * period;
                    ++step_;
                } else {
                    step_ = 0;          // nothing held -- keep the clock in sync, emit nothing
                }
                nextStep_ += period;
            }
        }

        ctx.out<MidiRef>(0, MidiRef{out_.data(), out_.size()});
    }

private:
    // The ordered note sequence for one arpeggio cycle, given the held notes.
    std::vector<int> buildSequence(int octaves, int mode) const {
        std::vector<int> up;
        for (int o = 0; o < octaves; ++o)
            for (int n : held_) up.push_back(n + 12 * o);   // held_ iterates ascending
        if (mode == 1) { std::reverse(up.begin(), up.end()); return up; }   // down
        if (mode == 2 && up.size() > 1) {                                   // up-down
            std::vector<int> ud = up;
            for (int i = (int)up.size() - 2; i >= 1; --i) ud.push_back(up[i]);
            return ud;
        }
        return up;   // up (mode 0), or a single note
    }

    std::set<int>          held_;          // MIDI note numbers currently held
    std::vector<MidiEvent> out_;           // events produced this frame (owns MidiRef storage)
    double clock_    = 0.0;                // seconds since start (free mode)
    double nextStep_ = 0.0;                // clock time of the next note-on (free mode)
    double noteOff_  = 0.0;                // clock time to release the active note (free mode)
    int    step_     = 0;                  // index into the step sequence
    int    active_   = -1;                 // currently sounding note, or -1
    long long lastStepAbs_ = 0;            // last synced step index (sync mode)
    bool      primed_      = false;        // synced playback has fired its first step
};

} // namespace oss
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build -j && ./build/core_tests --test-case="*arpeggiator*,*MidiRef maps*"`
Expected: PASS (all Arp cases, free + synced).

- [ ] **Step 5: Full suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add src/modules/ArpeggiatorNode.h tests/test_arpeggiator.cpp
git commit -m "feat(modules): Arpeggiator transport BPM sync (opt-in)"
```

---

### Task 4: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Update the README rows**

In `README.md`, replace the Step Seq row:

```markdown
| **Step Seq** | 16-step drum sequencer → MIDI |
```
with:
```markdown
| **Step Seq** | 16-step drum sequencer → MIDI; `sync` toggle locks the step rate to the project BPM over musical divisions (1/4 … 1/32, incl. dotted + triplet), or runs free at its own `tempo` |
```

And replace the Arpeggiator row:

```markdown
| **Arpeggiator** | held notes → a stepped sequence |
```
with:
```markdown
| **Arpeggiator** | held notes → a stepped sequence (up / down / up-down); `sync` toggle locks the step rate to the project BPM, or runs free at `rate` steps/sec |
```

- [ ] **Step 2: Note the sync in CLAUDE.md**

In `CLAUDE.md`, in the **Architecture** section, add a new bullet immediately after
the **Choice input ports** bullet (the one ending "... so LFOs chain."):

```markdown
- **Transport-synced sequencers** — the **Step Seq** and **Arpeggiator** have a
  `sync` input that locks their step rate to the global transport: each step is
  derived statelessly from `transport.bars()` at a `rate sync` division (shared
  table in `src/core/StepSync.h`), so they follow the project BPM, start/stop with
  the transport, and stay bar-aligned through loops. With `sync` off they free-run
  off their own `tempo`/`rate`, unchanged.
```

- [ ] **Step 3: Sanity check + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code changed).

```bash
git add README.md CLAUDE.md
git commit -m "docs: document Step Seq + Arp transport BPM sync"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build
- [ ] `ctest --test-dir build --output-on-failure` — all pass (`core_tests`, `gl_smoke`)
- [ ] Use superpowers:finishing-a-development-branch
