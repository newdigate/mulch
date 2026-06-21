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
