# MIDI Chord Player Node Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a **Chord Player** node that holds 8 root+octave+chord patterns and plays one at a time as a chord on its MIDI output, switching on a transport-synced Bar/Beat boundary (auto-progress or manual), to feed the Arpeggiator.

**Architecture:** A GL-free chord-interval helper (`src/core/Chords.h`) plus a GL-free, header-only node (`src/modules/ChordPlayerNode.h`) that derives the active pattern statelessly from `transport.bars()` (loop-robust, like Step Seq), emits the chord's note-ons, and releases the prior chord's exact notes on every switch/stop. Both are unit-tested directly in `core_tests`, exactly like `ArpeggiatorNode`/`StepSequencerNode`.

**Tech Stack:** C++17, doctest, CMake. Design: `docs/superpowers/specs/2026-06-18-chord-player-design.md`.

**IMPORTANT for the implementer:** `Chords.h` and `ChordPlayerNode.h` are both **GL-free and header-only** — no `<glad/gl.h>`, no `.cpp`, no `APP_SOURCES`/`gl_smoke` change. `core_tests` links no GL; the node is GL-free so the test includes the node header directly (this is how `tests/test_arpeggiator.cpp` tests `ArpeggiatorNode`). Only the two test files are added to `CMakeLists.txt`. Both tasks are TDD: write the test, watch it fail (the header doesn't exist yet → compile error), write the header, watch it pass.

---

### Task 1: GL-free chord helper + unit tests

**Files:**
- Create: `tests/test_chords.cpp`
- Create: `src/core/Chords.h`
- Modify: `CMakeLists.txt` (add `tests/test_chords.cpp` to `core_tests`)

- [ ] **Step 1: Write the failing tests**

Create `tests/test_chords.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/Chords.h"
#include <vector>

using namespace oss;

TEST_CASE("buildChordNotes builds a C major triad at octave 4") {
    std::vector<int> n;
    buildChordNotes(0, 4, 0, n);              // C, oct 4, maj
    REQUIRE(n.size() == 3);
    CHECK(n[0] == 60); CHECK(n[1] == 64); CHECK(n[2] == 67);
}

TEST_CASE("buildChordNotes builds an A minor triad") {
    std::vector<int> n;
    buildChordNotes(9, 4, 1, n);              // A, oct 4, min
    REQUIRE(n.size() == 3);
    CHECK(n[0] == 69); CHECK(n[1] == 72); CHECK(n[2] == 76);
}

TEST_CASE("seventh chords have four notes") {
    std::vector<int> n;
    buildChordNotes(0, 4, 6, n);              // C maj7
    REQUIRE(n.size() == 4);
    CHECK(n[3] == 71);                        // 60 + 11
}

TEST_CASE("raising the octave by one adds 12 to every note") {
    std::vector<int> a, b;
    buildChordNotes(2, 3, 8, a);              // D dom7, oct 3
    buildChordNotes(2, 4, 8, b);              // D dom7, oct 4
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(b[i] == a[i] + 12);
}

TEST_CASE("notes above 127 are dropped but the root remains") {
    std::vector<int> n;
    buildChordNotes(11, 8, 13, n);            // B add9, oct 8: base 119, +{0,4,7,14}; 133 dropped
    REQUIRE(n.size() == 3);
    CHECK(n[0] == 119);
    for (int x : n) CHECK(x <= 127);
}

TEST_CASE("buildChordNotes appends without clearing") {
    std::vector<int> n = {1, 2};
    buildChordNotes(0, 4, 0, n);
    REQUIRE(n.size() == 5);
    CHECK(n[0] == 1); CHECK(n[2] == 60);
}

TEST_CASE("root and chord label counts match the spec") {
    CHECK(rootNoteLabels().size() == 12);
    CHECK(chordNames().size() == 14);
}
```

- [ ] **Step 2: Wire the test into the build**

In `CMakeLists.txt`, in the `core_tests` target's test-file list, add `tests/test_chords.cpp` right after `tests/test_oscilloscope.cpp`:

```cmake
  tests/test_oscilloscope.cpp
  tests/test_chords.cpp
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — compile error `'core/Chords.h' file not found` (the header doesn't exist yet).

- [ ] **Step 4: Write the header to make them pass**

Create `src/core/Chords.h`:

```cpp
#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace oss {

// 12 pitch-class labels for a root-note dropdown (index = semitone within an octave).
inline const std::vector<std::string>& rootNoteLabels() {
    static const std::vector<std::string> labels = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    return labels;
}

// 14 chord-type labels for a chord dropdown (index = chord type).
inline const std::vector<std::string>& chordNames() {
    static const std::vector<std::string> names = {
        "maj", "min", "dim", "aug", "sus2", "sus4", "maj7",
        "min7", "dom7", "6", "m6", "m7b5", "dim7", "add9"
    };
    return names;
}

// Append the MIDI notes of a chord to `out` (does not clear it). rootPitchClass 0..11,
// octave with C4 = 60 (note = (octave+1)*12 + rootPitchClass + interval), chordIndex
// into chordNames(). Notes > 127 are dropped; root and chord indices are clamped.
inline void buildChordNotes(int rootPitchClass, int octave, int chordIndex,
                            std::vector<int>& out) {
    static const std::vector<std::vector<int>> intervals = {
        {0, 4, 7},      // maj
        {0, 3, 7},      // min
        {0, 3, 6},      // dim
        {0, 4, 8},      // aug
        {0, 2, 7},      // sus2
        {0, 5, 7},      // sus4
        {0, 4, 7, 11},  // maj7
        {0, 3, 7, 10},  // min7
        {0, 4, 7, 10},  // dom7
        {0, 4, 7, 9},   // 6
        {0, 3, 7, 9},   // m6
        {0, 3, 6, 10},  // m7b5
        {0, 3, 6, 9},   // dim7
        {0, 4, 7, 14},  // add9
    };
    int pc   = std::clamp(rootPitchClass, 0, 11);
    int ci   = std::clamp(chordIndex, 0, (int)intervals.size() - 1);
    int base = (octave + 1) * 12 + pc;   // C4 = 60
    for (int iv : intervals[ci]) {
        int note = base + iv;
        if (note >= 0 && note <= 127) out.push_back(note);
    }
}

} // namespace oss
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all `core_tests` green, including the 7 chord cases. (Run the full binary to confirm nothing else broke.)

- [ ] **Step 6: Commit**

```bash
git add src/core/Chords.h tests/test_chords.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(core): add a GL-free chord-interval helper

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Chord Player node + registration + screenshot demo

**Files:**
- Create: `tests/test_chord_player.cpp`
- Create: `src/modules/ChordPlayerNode.h`
- Modify: `CMakeLists.txt` (add `tests/test_chord_player.cpp` to `core_tests`)
- Modify: `src/app/Application.cpp` (include + `makeNode` + MIDI category)
- Modify: `src/main.cpp` (screenshot demo)

- [ ] **Step 1: Write the failing node tests**

Create `tests/test_chord_player.cpp`:

```cpp
#include <doctest/doctest.h>
#include "modules/ChordPlayerNode.h"
#include "core/Node.h"
#include "core/Transport.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

// 28 float inputs: each pattern defaults to C(0)/oct4/maj(0); globals Auto/Bar/len8/pat1.
static std::vector<Value> defaultInputs() {
    std::vector<Value> in(28);
    for (int p = 0; p < 8; ++p) {
        in[3 * p + 0] = 0.0f;   // root C
        in[3 * p + 1] = 4.0f;   // oct 4
        in[3 * p + 2] = 0.0f;   // chord maj
    }
    in[24] = 0.0f;   // mode Auto
    in[25] = 0.0f;   // quantize Bar
    in[26] = 8.0f;   // length 8
    in[27] = 0.0f;   // pattern 1 (index 0)
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
    CHECK(countNoteOffs(e) == 3);                    // release C maj
    CHECK(noteOns(e) == std::vector<int>{60, 63, 67});   // C min
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
```

- [ ] **Step 2: Wire the node test into the build**

In `CMakeLists.txt`, in the `core_tests` target's test-file list, add `tests/test_chord_player.cpp` right after the `tests/test_chords.cpp` line added in Task 1:

```cmake
  tests/test_chords.cpp
  tests/test_chord_player.cpp
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build -j --target core_tests`
Expected: FAIL — compile error `'modules/ChordPlayerNode.h' file not found` (the node header doesn't exist yet).

- [ ] **Step 4: Write the node to make them pass**

Create `src/modules/ChordPlayerNode.h`:

```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/Chords.h"

namespace oss {

// Transport-synced chord sequencer: holds 8 patterns (root pitch-class + octave + chord
// name) and plays one at a time as a chord (simultaneous note-ons) on its MIDI output,
// switching on a quantized boundary (next Bar, or Beat). Patterns auto-progress
// (unitAbs % length) or are manually selected. Releases the prior chord's exact notes on
// every switch / stop, so nothing hangs (and the downstream Arpeggiator folds the
// note-ons into its held set). GL-free, header-only.
//
// Inputs: per pattern p (0..7): 3p root (choice), 3p+1 oct, 3p+2 chord (choice);
//   24 mode (choice Auto/Manual), 25 quantize (choice Bar/Beat), 26 length (1..8),
//   27 pattern (choice 1..8, manual selection). Output 0 = midi.
class ChordPlayerNode : public Node {
public:
    ChordPlayerNode() : Node("Chord Player") {
        for (int p = 0; p < kPatterns; ++p) {
            std::string s = std::to_string(p + 1);
            addChoiceInput("root " + s, rootNoteLabels(), 0);          // C
            addInput("oct " + s, PortType::Float, 4.0f, 0.0f, 8.0f);   // octave (C4=60)
            addChoiceInput("chord " + s, chordNames(), 0);             // maj
        }
        addChoiceInput("mode", {"Auto", "Manual"}, 0);
        addChoiceInput("quantize", {"Bar", "Beat"}, 0);
        addInput("length", PortType::Float, 8.0f, 1.0f, 8.0f);            // auto loop length
        addChoiceInput("pattern", {"1","2","3","4","5","6","7","8"}, 0);  // manual select
        addOutput("midi", PortType::Midi);
    }

    void evaluate(EvalContext& ctx) override {
        out_.clear();

        int mode      = std::clamp((int)std::lround(ctx.in<float>(kGlobals + 0)), 0, 1);
        int quantize  = std::clamp((int)std::lround(ctx.in<float>(kGlobals + 1)), 0, 1);
        int length    = std::clamp((int)std::lround(ctx.in<float>(kGlobals + 2)), 1, kPatterns);
        int manualSel = std::clamp((int)std::lround(ctx.in<float>(kGlobals + 3)), 0, kPatterns - 1);

        int beatsPerBar = (ctx.transport && ctx.transport->beatsPerBar > 0)
                              ? ctx.transport->beatsPerBar : 4;
        double unitBars = (quantize == 1) ? 1.0 / (double)beatsPerBar : 1.0;

        if (ctx.transport && ctx.transport->playing && unitBars > 0.0) {
            double    unitPos  = ctx.transport->bars() / unitBars;
            long long unitAbs  = (long long)std::floor(unitPos);
            bool      boundary = !primed_ || unitAbs != lastUnitAbs_;

            if (boundary) {
                int target = (mode == 1)
                    ? manualSel
                    : (int)(((unitAbs % length) + length) % length);
                if (!primed_ || target != activePattern_) {
                    for (int note : activeNotes_) out_.push_back(midiNoteOff(note));
                    activeNotes_.clear();
                    buildChordNotes(rootOf(ctx, target), octOf(ctx, target),
                                    chordOf(ctx, target), activeNotes_);
                    for (int note : activeNotes_) out_.push_back(midiNoteOn(note, 100));
                    activePattern_ = target;
                }
                lastUnitAbs_ = unitAbs;
                primed_ = true;
            }
        } else {
            // paused/stopped: release everything and re-prime so resume re-fires cleanly
            for (int note : activeNotes_) out_.push_back(midiNoteOff(note));
            activeNotes_.clear();
            activePattern_ = -1;
            primed_ = false;
        }

        // Status line: mode, the sounding chord, and pattern/length.
        std::string st = (mode == 1) ? "Manual" : "Auto";
        if (activePattern_ >= 0) {
            st += " · " + rootNoteLabels()[(std::size_t)rootOf(ctx, activePattern_)]
                + " " + chordNames()[(std::size_t)chordOf(ctx, activePattern_)]
                + " · ▸" + std::to_string(activePattern_ + 1)
                + "/" + std::to_string(length);
        }
        status_ = st;

        ctx.out<MidiRef>(0, MidiRef{out_.data(), out_.size()});
    }

    std::string statusLine() const override { return status_; }

private:
    int rootOf(EvalContext& ctx, int p) const {
        return std::clamp((int)std::lround(ctx.in<float>((std::size_t)(3 * p + 0))), 0, 11);
    }
    int octOf(EvalContext& ctx, int p) const {
        return std::clamp((int)std::lround(ctx.in<float>((std::size_t)(3 * p + 1))), 0, 8);
    }
    int chordOf(EvalContext& ctx, int p) const {
        return std::clamp((int)std::lround(ctx.in<float>((std::size_t)(3 * p + 2))),
                          0, (int)chordNames().size() - 1);
    }

    static constexpr int kPatterns = 8;
    static constexpr int kGlobals  = 3 * kPatterns;   // 24 (first global input index)
    std::vector<MidiEvent> out_;          // events produced this frame (owns MidiRef storage)
    std::vector<int>       activeNotes_;  // exact MIDI notes currently sounding
    int                    activePattern_ = -1;
    long long              lastUnitAbs_   = 0;
    bool                   primed_        = false;
    std::string            status_;
};

} // namespace oss
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — all `core_tests` green, including the 5 Chord Player cases. If a case fails, fix `src/modules/ChordPlayerNode.h` to match the spec.

- [ ] **Step 6: Register the node**

In `src/app/Application.cpp`, add the include right after `#include "modules/ArpeggiatorNode.h"`:

```cpp
#include "modules/ArpeggiatorNode.h"
#include "modules/ChordPlayerNode.h"
```

Add the `makeNode` branch right after the `if (type == "MIDI File") ...` line:

```cpp
    if (type == "MIDI File")   return std::make_unique<MidiFilePlayerNode>();
    if (type == "Chord Player") return std::make_unique<ChordPlayerNode>();
```

Add `"Chord Player"` to the **MIDI** category list, right before `"Arpeggiator"`:

```cpp
        { "MIDI",    { "MIDI In", "MIDI File", "Step Seq", "Chord Player", "Arpeggiator", "MIDI Merge", "MIDI Out" } },
```

- [ ] **Step 7: Add it to the screenshot demo**

In `src/main.cpp`, add this line right after the existing `app.addNodeOfType("Oscilloscope", ...);` demo line:

```cpp
        app.addNodeOfType("Chord Player", glm::vec2(1080.0f, 320.0f));
```

- [ ] **Step 8: Build everything**

Run: `cmake --build build -j`
Expected: clean build of `shader_streamer`, `core_tests`, and `gl_smoke` (the node header is only pulled in by `Application.cpp`; no GL leak into `core_tests`).

- [ ] **Step 9: Verify the node renders**

Run: `./build/shader_streamer --screenshot build/_ui.png`
Expected: exit 0, prints `wrote screenshot build/_ui.png`. Open the PNG with the Read tool and confirm a **Chord Player** node is present with its title, a status line (`Auto · C maj · ▸1/8` once the demo's playhead lands on a bar), and at least its first input rows (`root 1`, `oct 1`, `chord 1`, …). This node is tall (28 inputs); it is fine for the lower rows to extend off the bottom or behind the Automation panel, as long as the title and top rows are clearly visible. If the node is off-screen or fully clipped, move it to a clear on-screen spot (the demo's top row sits at y≈60 near x=40/420/740/1060; other nodes at y≈240/320) and re-screenshot until the title and top rows show.

- [ ] **Step 10: Commit**

```bash
git add src/modules/ChordPlayerNode.h tests/test_chord_player.cpp src/app/Application.cpp src/main.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(modules): add the Chord Player node + register it

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Add the README module-table row**

In `README.md`, add this row immediately after the `| **Arpeggiator** | ... |` row (search for `Arpeggiator` in the module table; place the new row directly beneath it):

```markdown
| **Chord Player** | 8 chord patterns → MIDI: each pattern is a root + octave + chord name (14 types); plays one as a chord, switching on a transport-synced **Bar**/**Beat** boundary. Auto-progress through the first `length` patterns, or pick one manually. Wire into the **Arpeggiator** or a synth |
```

- [ ] **Step 2: Add the CLAUDE.md architecture bullet**

In `CLAUDE.md`, in the **Architecture** section, add this bullet immediately after the **Oscilloscope** bullet (it ends with "... The trace math is unit-tested in `core_tests`."):

```markdown
- **Chord Player** — `ChordPlayerNode` (`src/modules/ChordPlayerNode.h`, header-only,
  GL-free) holds 8 patterns (root pitch-class + octave + chord name from the 14-chord
  GL-free `core/Chords.h`) and emits one at a time as a chord (simultaneous note-ons) on
  its `midi` output, switching on a transport-synced Bar/Beat boundary — auto-progressing
  `unitAbs % length` (stateless from `transport.bars()`, so loop-robust) or manually
  selected. It tracks the sounding chord's exact notes and releases them on every
  switch / stop, so nothing hangs; wire it into the Arpeggiator (which folds the chord's
  note-ons into its held set) or any synth. Unit-tested in `core_tests`.
```

- [ ] **Step 3: Sanity check + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (no code changed).

```bash
git add README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: document the Chord Player node

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (`core_tests` incl. the chord + chord-player cases, `gl_smoke`)
- [ ] `./build/shader_streamer --screenshot build/_ui.png` — the Chord Player node renders with its title + ports
- [ ] Use superpowers:finishing-a-development-branch
</content>
