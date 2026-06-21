# Chord Player Presets Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `ChordPlayerNode` 8 preset progressions switched by 8 buttons / a MIDI input / quantized timing, with all presets saved & loaded.

**Architecture:** The node owns `Preset presets_[8]` (each a full 8-step sequence + length) as the playback source of truth; the active preset is mirrored into the existing per-step input ports and captured back each frame, so the existing inline chord editors edit it in place. A small generic GL-free button-bank hook on `Node` lets `NodeEditorPanel` draw the 8 buttons without putting ImGui in the (unit-tested) node. A MIDI `select` input maps note-on C–G → preset; a `switch` choice quantizes the change (Immediate/Beat/Bar/4 Bars). `saveState`/`loadState` carry all 8 presets.

**Tech Stack:** C++17, header-only node, Dear ImGui (editor only), doctest, CMake. Design: `docs/superpowers/specs/2026-06-21-chord-player-presets-design.md`.

**Notes:**
- `ChordPlayerNode.h` and `core/Node.h` stay GL-free/ImGui-free (the node is unit-tested in `core_tests`). All ImGui lives in `src/ui/NodeEditorPanel.cpp`.
- No CMake change (files already compiled), no node re-registration (already registered).
- Playback reads from the `presets_` struct, **not** `ctx.in` — `ctx.in` is resolved at frame start, so a same-frame preset switch must build its chord from the struct.

---

### Task 1: Generic `Node` button-bank hook

**Files:**
- Modify: `src/core/Node.h`

- [ ] **Step 1: Add the five virtuals (`src/core/Node.h`)**

After the existing `virtual void loadState(const std::string& /*s*/) {}` line, add:
```cpp
    // Optional button bank, rendered by the node editor as a row of buttons under the
    // node's name (GL-free: ints/strings only). Default = none. A node exposes preset/
    // mode buttons by overriding these; the editor calls onButtonPressed() on a click.
    virtual int         buttonCount() const { return 0; }       // 0 = no buttons
    virtual std::string buttonLabel(int /*i*/) const { return std::string(); }
    virtual int         buttonActive() const { return -1; }     // index drawn highlighted
    virtual int         buttonPending() const { return -1; }    // index drawn as "pending"
    virtual void        onButtonPressed(int /*i*/) {}           // a button was clicked
```

- [ ] **Step 2: Build to verify no regression**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build of all targets; all tests pass (pure additive interface, inert defaults — no behavior change).

- [ ] **Step 3: Commit**

```bash
git add src/core/Node.h
git commit -m "$(cat <<'EOF'
feat(core): add a generic GL-free button-bank hook to Node

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: `ChordPlayerNode` preset model + switching + defaults + persistence

**Files:**
- Modify (rewrite): `src/modules/ChordPlayerNode.h`
- Modify: `tests/test_chord_player.cpp`

- [ ] **Step 1: Grow the test harness + append failing tests (`tests/test_chord_player.cpp`)**

Replace the `defaultInputs()` helper (it builds a size-28 vector) with a size-30 one (two new ports):
```cpp
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
```
Then append these new cases at the end of the file:
```cpp
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
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `buttonActive`/`onButtonPressed`/`saveState` content etc. don't exist yet, and the size-30 `defaultInputs` will read new ports the node doesn't declare.

- [ ] **Step 3: Rewrite `src/modules/ChordPlayerNode.h`**

Replace the entire file with:
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

// Transport-synced chord sequencer with 8 presets. Each preset is a full 8-step chord
// progression (root/oct/chord per step + loop length). The active preset is the playback
// source of truth (presets_[]); it is mirrored into the existing per-step input ports and
// captured back each frame, so the inline editors edit the active preset in place. Switch the
// active preset with the 8 buttons (generic Node button hook), a MIDI note on `select`
// (pitch-class C..G -> preset 1..8), or a load; the change lands on a `switch` quantize
// boundary (Immediate/Beat/Bar/4 Bars). Within a preset the chords still auto-step (or are
// manually picked) on the Bar/Beat `quantize`. Releases the prior chord's exact notes on every
// switch/stop, so nothing hangs. GL-free, header-only.
//
// Inputs: per step k (0..7): 3k root (choice), 3k+1 oct, 3k+2 chord (choice);
//   24 mode (Auto/Manual), 25 quantize (Bar/Beat), 26 length (1..8), 27 pattern (1..8 manual),
//   28 switch (Immediate/Beat/Bar/4 Bars), 29 select (MIDI in). Output 0 = midi.
class ChordPlayerNode : public Node {
public:
    static constexpr int kSteps    = 8;            // steps per preset
    static constexpr int kPresets  = 8;
    static constexpr int kGlobals  = 3 * kSteps;   // 24
    static constexpr int kModeIdx     = kGlobals + 0;   // 24
    static constexpr int kQuantizeIdx = kGlobals + 1;   // 25
    static constexpr int kLengthIdx   = kGlobals + 2;   // 26
    static constexpr int kPatternIdx  = kGlobals + 3;   // 27
    static constexpr int kSwitchIdx   = kGlobals + 4;   // 28
    static constexpr int kSelectIdx   = kGlobals + 5;   // 29

    struct Preset { int root[kSteps]; int oct[kSteps]; int chord[kSteps]; int length; };

    ChordPlayerNode() : Node("Chord Player") {
        for (int p = 0; p < kSteps; ++p) {
            std::string s = std::to_string(p + 1);
            addChoiceInput("root " + s, rootNoteLabels(), 0);
            addInput("oct " + s, PortType::Float, 4.0f, 0.0f, 8.0f);
            addChoiceInput("chord " + s, chordNames(), 0);
        }
        addChoiceInput("mode", {"Auto", "Manual"}, 0);
        addChoiceInput("quantize", {"Bar", "Beat"}, 0);
        addInput("length", PortType::Float, 8.0f, 1.0f, 8.0f);
        addChoiceInput("pattern", {"1","2","3","4","5","6","7","8"}, 0);
        addChoiceInput("switch", {"Immediate","Beat","Bar","4 Bars"}, 2);   // preset-switch quantize
        addInput("select", PortType::Midi, MidiRef{});                      // note -> preset
        addOutput("midi", PortType::Midi);

        defaultPresets(presets_);
        writePorts(presets_[0]);   // a fresh node shows preset 1
    }

    // --- generic button bank (rendered by NodeEditorPanel) ---
    int         buttonCount() const override { return kPresets; }
    std::string buttonLabel(int i) const override { return std::to_string(i + 1); }
    int         buttonActive() const override { return activePreset_; }
    int         buttonPending() const override { return requestedPreset_ != activePreset_ ? requestedPreset_ : -1; }
    void        onButtonPressed(int i) override { requestedPreset_ = std::clamp(i, 0, kPresets - 1); }

    void evaluate(EvalContext& ctx) override {
        out_.clear();

        int mode      = clampi(ctx.in<float>(kModeIdx), 0, 1);
        int quantize  = clampi(ctx.in<float>(kQuantizeIdx), 0, 1);
        int length    = clampi(ctx.in<float>(kLengthIdx), 1, kSteps);
        int manualSel = clampi(ctx.in<float>(kPatternIdx), 0, kSteps - 1);
        int swMode    = clampi(ctx.in<float>(kSwitchIdx), 0, 3);

        // (1) MIDI select: a note-on with pitch-class C..G (0..7) requests that preset.
        MidiRef sel = ctx.in<MidiRef>(kSelectIdx);
        for (std::size_t i = 0; i < sel.count; ++i) {
            const MidiEvent& e = sel.events[i];
            if (midiIsNoteOn(e)) { int pc = e.data1 % 12; if (pc < kPresets) requestedPreset_ = pc; }
        }

        // (2) capture the active preset's current port values (persist live edits / automation)
        captureActive(ctx, length);

        int beatsPerBar = (ctx.transport && ctx.transport->beatsPerBar > 0)
                              ? ctx.transport->beatsPerBar : 4;
        bool playing = ctx.transport && ctx.transport->playing;

        // (3) preset switch on the chosen quantize boundary
        double swUnit = (swMode == 1) ? 1.0 / (double)beatsPerBar
                      : (swMode == 2) ? 1.0
                      : (swMode == 3) ? 4.0 : 0.0;
        long long su = (playing && swUnit > 0.0) ? (long long)std::floor(ctx.transport->bars() / swUnit) : 0;
        bool swBoundary = !switchPrimed_ || su != lastSwitchUnit_;
        lastSwitchUnit_ = su; switchPrimed_ = true;

        bool forceRefire = false;
        if (requestedPreset_ != activePreset_) {
            bool doSwitch = (swMode == 0) || !playing || swBoundary;
            if (doSwitch) {
                activePreset_ = requestedPreset_;
                writePorts(presets_[activePreset_]);
                forceRefire = true;
            }
        }

        const Preset& pr = presets_[activePreset_];
        int plen = std::clamp(pr.length, 1, kSteps);

        // (4) within-preset chord stepping (reads the preset struct, not ctx.in)
        double unitBars = (quantize == 1) ? 1.0 / (double)beatsPerBar : 1.0;
        if (playing && unitBars > 0.0) {
            double    unitPos  = ctx.transport->bars() / unitBars;
            long long unitAbs  = (long long)std::floor(unitPos);
            bool      boundary = !primed_ || unitAbs != lastUnitAbs_;
            if (boundary || forceRefire) {
                int target = (mode == 1) ? std::clamp(manualSel, 0, plen - 1)
                                         : (int)(((unitAbs % plen) + plen) % plen);
                if (!primed_ || target != activeStep_ || forceRefire) {
                    for (int note : activeNotes_) out_.push_back(midiNoteOff(note));
                    activeNotes_.clear();
                    buildChordNotes(pr.root[target], pr.oct[target], pr.chord[target], activeNotes_);
                    for (int note : activeNotes_) out_.push_back(midiNoteOn(note, 100));
                    activeStep_ = target;
                }
                lastUnitAbs_ = unitAbs;
                primed_ = true;
            }
        } else {
            for (int note : activeNotes_) out_.push_back(midiNoteOff(note));
            activeNotes_.clear();
            activeStep_ = -1;
            primed_ = false;
        }

        // status: preset (+ pending), mode, sounding chord, step/length
        std::string st = "P" + std::to_string(activePreset_ + 1);
        if (requestedPreset_ != activePreset_) st += "\xE2\x86\x92" + std::to_string(requestedPreset_ + 1);  // ->
        st += (mode == 1) ? " \xE2\x80\xA2 Manual" : " \xE2\x80\xA2 Auto";
        if (activeStep_ >= 0) {
            st += " \xE2\x80\xA2 " + rootNoteLabels()[(std::size_t)pr.root[activeStep_]]
                + " " + chordNames()[(std::size_t)pr.chord[activeStep_]]
                + " \xE2\x80\xA2 \xE2\x96\xB8" + std::to_string(activeStep_ + 1) + "/" + std::to_string(plen);
        }
        status_ = st;

        ctx.out<MidiRef>(0, MidiRef{out_.data(), out_.size()});
    }

    std::string statusLine() const override { return status_; }

    // Persist all 8 presets + the active index. "<active>;<p0>;...;<p7>", each preset
    // "<length>,<r0>,<o0>,<c0>,...,<r7>,<o7>,<c7>". presets_ is the source of truth (kept
    // current each frame by captureActive), so saving reads it directly. No spaces/':'/'|'.
    std::string saveState() const override {
        std::string s = std::to_string(activePreset_);
        for (int p = 0; p < kPresets; ++p) {
            const Preset& pr = presets_[p];
            s += ';' + std::to_string(std::clamp(pr.length, 1, kSteps));
            for (int k = 0; k < kSteps; ++k)
                s += ',' + std::to_string(pr.root[k]) + ',' + std::to_string(pr.oct[k])
                   + ',' + std::to_string(pr.chord[k]);
        }
        return s;
    }
    void loadState(const std::string& s) override {
        if (s.empty()) return;
        std::vector<std::string> blocks = splitc(s, ';');
        if (blocks.empty()) return;
        activePreset_ = std::clamp(atoiSafe(blocks[0]), 0, kPresets - 1);
        requestedPreset_ = activePreset_;
        for (int p = 0; p < kPresets && p + 1 < (int)blocks.size(); ++p) {
            std::vector<std::string> n = splitc(blocks[(std::size_t)(p + 1)], ',');
            if (n.empty()) continue;
            Preset pr = presets_[p];
            pr.length = std::clamp(atoiSafe(n[0]), 1, kSteps);
            for (int k = 0; k < kSteps; ++k) {
                int base = 1 + 3 * k;
                if (base + 2 < (int)n.size()) {
                    pr.root[k]  = std::clamp(atoiSafe(n[(std::size_t)(base + 0)]), 0, 11);
                    pr.oct[k]   = std::clamp(atoiSafe(n[(std::size_t)(base + 1)]), 0, 8);
                    pr.chord[k] = std::clamp(atoiSafe(n[(std::size_t)(base + 2)]), 0, (int)chordNames().size() - 1);
                }
            }
            presets_[p] = pr;
        }
        writePorts(presets_[activePreset_]);
    }

private:
    static int clampi(float v, int lo, int hi) { return std::clamp((int)std::lround(v), lo, hi); }

    void writePorts(const Preset& pr) {
        for (int k = 0; k < kSteps; ++k) {
            inputDefault((std::size_t)(3 * k + 0)) = Value((float)pr.root[k]);
            inputDefault((std::size_t)(3 * k + 1)) = Value((float)pr.oct[k]);
            inputDefault((std::size_t)(3 * k + 2)) = Value((float)pr.chord[k]);
        }
        inputDefault((std::size_t)kLengthIdx) = Value((float)std::clamp(pr.length, 1, kSteps));
    }
    void captureActive(EvalContext& ctx, int length) {
        Preset& pr = presets_[activePreset_];
        for (int k = 0; k < kSteps; ++k) {
            pr.root[k]  = clampi(ctx.in<float>((std::size_t)(3 * k + 0)), 0, 11);
            pr.oct[k]   = clampi(ctx.in<float>((std::size_t)(3 * k + 1)), 0, 8);
            pr.chord[k] = clampi(ctx.in<float>((std::size_t)(3 * k + 2)), 0, (int)chordNames().size() - 1);
        }
        pr.length = std::clamp(length, 1, kSteps);
    }

    static std::vector<std::string> splitc(const std::string& s, char d) {
        std::vector<std::string> v; std::string cur;
        for (char c : s) { if (c == d) { v.push_back(cur); cur.clear(); } else cur += c; }
        v.push_back(cur); return v;
    }
    static int atoiSafe(const std::string& s) { try { return std::stoi(s); } catch (...) { return 0; } }

    static void defaultPresets(Preset out[kPresets]) {
        // {length, root per step, chord per step}; oct = 4; steps past length copy step 0.
        struct Row { int len; int root[kSteps]; int chord[kSteps]; };
        static const Row rows[kPresets] = {
            {4, {0,7,9,5, 0,0,0,0},        {0,0,1,0, 0,0,0,0}},         // Pop I-V-vi-IV (C G Am F)
            {4, {0,9,5,7, 0,0,0,0},        {0,1,0,0, 0,0,0,0}},         // Doo-wop I-vi-IV-V (C Am F G)
            {4, {2,7,0,9, 2,2,2,2},        {7,8,6,7, 7,7,7,7}},         // Jazz ii-V-I (Dm7 G7 Cmaj7 Am7)
            {4, {9,7,5,4, 9,9,9,9},        {1,0,0,0, 1,1,1,1}},         // Andalusian (Am G F E)
            {4, {9,5,0,7, 9,9,9,9},        {1,0,0,0, 1,1,1,1}},         // vi-IV-I-V (Am F C G)
            {8, {0,0,0,0, 5,5,0,7},        {8,8,8,8, 8,8,8,8}},         // 12-bar blues (dom7)
            {8, {0,7,9,4, 5,0,5,7},        {0,0,1,1, 0,0,0,0}},         // Pachelbel (C G Am Em F C F G)
            {3, {11,4,9,11, 11,11,11,11},  {11,8,1,11, 11,11,11,11}},   // Minor ii-V-i (Bm7b5 E7 Am)
        };
        for (int p = 0; p < kPresets; ++p) {
            out[p].length = rows[p].len;
            for (int k = 0; k < kSteps; ++k) {
                out[p].root[k]  = rows[p].root[k];
                out[p].oct[k]   = 4;
                out[p].chord[k] = rows[p].chord[k];
            }
        }
    }

    std::vector<MidiEvent> out_;
    std::vector<int>       activeNotes_;
    Preset                 presets_[kPresets];
    int                    activePreset_    = 0;
    int                    requestedPreset_ = 0;
    int                    activeStep_      = -1;
    long long              lastUnitAbs_     = 0;
    bool                   primed_          = false;
    long long              lastSwitchUnit_  = 0;
    bool                   switchPrimed_    = false;
    std::string            status_;
};

} // namespace oss
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests`
Expected: PASS — the 6 existing chord tests (now with 30-input vectors; playback via `captureActive` mirrors `ctx.in`) plus the 6 new cases.

- [ ] **Step 5: Full build**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build (incl. `shader_streamer`, `gl_smoke`); all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/modules/ChordPlayerNode.h tests/test_chord_player.cpp
git commit -m "$(cat <<'EOF'
feat(modules): 8 preset progressions in Chord Player (switch + save/load)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: `NodeEditorPanel` renders the button bank

**Files:**
- Modify: `src/ui/NodeEditorPanel.cpp`

- [ ] **Step 1: Draw the buttons (`src/ui/NodeEditorPanel.cpp`)**

In the per-node body, immediately after the `statusLine()` block:
```cpp
        std::string status = n.statusLine();
        if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
```
and before the input-pins loop (`for (std::size_t i = 0; i < n.inputs().size(); ++i)`), insert:
```cpp
        int nbtn = n.buttonCount();
        if (nbtn > 0) {
            int bAct = n.buttonActive(), bPend = n.buttonPending();
            for (int b = 0; b < nbtn; ++b) {
                if (b) ImGui::SameLine();
                int pushed = 0;
                if (b == bAct)        { ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 130, 200, 255)); pushed = 1; }
                else if (b == bPend)  { ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70,  90, 110, 255)); pushed = 1; }
                if (ImGui::SmallButton((n.buttonLabel(b) + "##btn" + std::to_string(b)).c_str()))
                    n.onButtonPressed(b);
                if (pushed) ImGui::PopStyleColor(pushed);
            }
        }
```
(`<string>` is already included; `IM_COL32`/`SmallButton`/`PushStyleColor` come from `imgui.h`, already included. This is generic — any node exposing `buttonCount() > 0` gets a button row.)

- [ ] **Step 2: Build + verify**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: clean build; all tests pass (editor-only change).

Run: `./build/shader_streamer --screenshot build/_chord.png && echo "exit $?"`
Expected: `exit 0`. Open `build/_chord.png` with the Read tool; confirm the app renders. If a Chord Player node is present (e.g. in a loaded `project.oss`), confirm a row of 8 numbered buttons appears under its name with one highlighted; otherwise note the app launched cleanly. Report what you saw.

**Manual checklist (report which you verified; needs a display):** run `./build/shader_streamer`, add a **Chord Player** node, confirm a row of buttons **1–8** under its name; click a button → the highlight moves (immediately, or showing a dim "pending" highlight until the next bar depending on `switch`); the sounding chord changes; wire a **MIDI File** / keyboard into `select` and confirm notes C–G change the preset.

- [ ] **Step 3: Commit**

```bash
git add src/ui/NodeEditorPanel.cpp
git commit -m "$(cat <<'EOF'
feat(ui): render a node button bank (Chord Player preset buttons)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: README.md**

Replace the **Chord Player** module-table row (search for `**Chord Player**`) with:
```markdown
| **Chord Player** | 8 preset chord progressions → MIDI. Each preset is an 8-step sequence (root + octave + chord, 14 types) that auto-steps (or is manually stepped) on a **Bar**/**Beat** boundary. Switch the active preset with the 8 buttons, a MIDI `select` note (C–G → preset 1–8), or save/load; the change lands **Immediately / next Beat / Bar / 4 Bars**. All 8 presets are saved with the project. Wire into the **Arpeggiator** or a synth |
```

- [ ] **Step 2: CLAUDE.md**

Replace the **Chord Player** architecture bullet (search for `` `ChordPlayerNode` ``) with:
```markdown
- **Chord Player** — `ChordPlayerNode` (`src/modules/ChordPlayerNode.h`, header-only, GL-free)
  holds **8 presets**, each a full 8-step chord progression (root pitch-class + octave + chord
  name from the 14-chord GL-free `core/Chords.h`, + a loop length). `presets_[8]` is the playback
  source of truth; the active preset is mirrored into the existing per-step input ports and
  captured back each frame, so the inline editors edit the active preset in place (playback reads
  the struct, not `ctx.in`, so a same-frame switch fires the right chord). Within the active
  preset it emits one step at a time as a chord (simultaneous note-ons) on `midi`, auto-stepping
  `unitAbs % length` (stateless from `transport.bars()`) or manually. Switch the active preset via
  the 8 buttons (a generic GL-free `Node` button-bank hook — `buttonCount/Label/Active/Pending` +
  `onButtonPressed` — rendered by `NodeEditorPanel`), a `select` MIDI input (note pitch-class C–G
  → preset), or a load; the switch lands on a `switch` quantize boundary (Immediate/Beat/Bar/4
  Bars). It releases the sounding notes on every switch/stop so nothing hangs. `saveState` carries
  all 8 presets + the active index. Unit-tested in `core_tests`; wire into the Arpeggiator or any synth.
```

- [ ] **Step 3: Verify + commit**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

```bash
git add README.md CLAUDE.md
git commit -m "$(cat <<'EOF'
docs: document Chord Player presets

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] `cmake --build build -j` — clean build (`shader_streamer`, `core_tests`, `gl_smoke`)
- [ ] `ctest --test-dir build --output-on-failure` — all pass (existing 6 chord tests + 6 new: button switch, Bar quantize, MIDI select, save/load round-trip incl. edits + active index + empty, default-content sanity, button hook)
- [ ] `./build/shader_streamer --screenshot build/_chord.png` — exit 0
- [ ] Manual: add a Chord Player node, click the 8 buttons (Immediate vs Bar quantize), wire MIDI into `select`, save + reload — presets persist
- [ ] Use superpowers:finishing-a-development-branch
```
