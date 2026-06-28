#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/DrumPattern.h"
#include "core/StepSync.h"
#include "core/AudioPan.h"
#include "core/AsyncLoader.h"
#include "audio/AudioFile.h"
#include "audio/AudioBlock.h"
#include "audio/SampleVoice.h"

namespace oss {

// Sample-based drum machine: 4 sample voices sequenced on a 4x16 tri-state grid (8 pattern slots),
// mixed to a stereo left/right output. Per sample: file path, volume, signed rate (pitch/reverse),
// pan. Clock mirrors Step Seq (transport-synced or free). A step's on/accent cells (re)trigger that
// row's voice; accent (cell==2) plays louder. GL-free + header-only.
//
// Port layout: per voice v (0..3) a block at 4v: file(4v), vol(4v+1), rate(4v+2), pan(4v+3).
// Then: tempo(16), sync(17), rate sync(18, choice), pattern(19, 1..8). Outputs: left(0), right(1).
class DrumMachineNode : public Node {
public:
    static constexpr int kVoices = 4;
    static constexpr int kSteps  = DrumPatterns::kCols;   // 16
    static constexpr int kTempoIdx    = 4 * kVoices + 0;  // 16
    static constexpr int kSyncIdx     = 4 * kVoices + 1;  // 17
    static constexpr int kRateSyncIdx = 4 * kVoices + 2;  // 18
    static constexpr int kPatternIdx  = 4 * kVoices + 3;  // 19

    DrumMachineNode() : Node("Drum Machine") {
        for (int v = 0; v < kVoices; ++v) {
            std::string s = std::to_string(v + 1);
            addAssetInput("file " + s, AssetType::Audio);
            addInput("vol "  + s, PortType::Float, 0.8f,  0.0f, 1.0f);
            addInput("rate " + s, PortType::Float, 1.0f, -4.0f, 4.0f);  // signed: <0 reverses
            addInput("pan "  + s, PortType::Float, 0.0f, -1.0f, 1.0f);
        }
        addInput("tempo", PortType::Float, 120.0f, 40.0f, 240.0f);      // free-mode BPM
        addInput("sync",  PortType::Bool, false);                       // lock to transport
        addChoiceInput("rate sync", stepDivisionLabels(), kStepDivisionDefault);  // step length
        addInput("pattern", PortType::Float, 1.0f, 1.0f, 8.0f);         // active slot (automatable)
        addOutput("left",  PortType::Audio);
        addOutput("right", PortType::Audio);
        outL_.assign(kAudioMaxBlock, 0.0f);
        outR_.assign(kAudioMaxBlock, 0.0f);
        for (int c = 0; c < kSteps; c += 4) patterns_.setCell(0, 0, c, 1);  // default kick four-on-the-floor
    }

    // --- tri-state grid hook (rows = voices, cols = steps; edits the active pattern) ---
    int gridRows() const override { return DrumPatterns::kRows; }
    int gridCols() const override { return DrumPatterns::kCols; }
    int gridCell(int r, int c) const override { return patterns_.cell(r, c); }
    void onGridCellPressed(int r, int c) override { patterns_.cycleCell(r, c); }
    std::string gridRowLabel(int r) const override { return std::to_string(r + 1); }

    // --- 8 pattern-slot buttons ---
    int buttonCount() const override { return DrumPatterns::kSlots; }
    std::string buttonLabel(int i) const override { return std::to_string(i + 1); }
    int buttonActive() const override { return patterns_.active(); }
    void onButtonPressed(int i) override { patterns_.setActive(i); }

    void evaluate(EvalContext& ctx) override {
        // (1) pattern selection: buttons set active directly; the pattern port is edge-detected. On the
        // first frame we only record the port value (don't override) so a project-loaded active slot
        // -- or a non-default restored port default -- survives; edge-detection drives it from then on.
        float patPort = ctx.in<float>(kPatternIdx);
        if (!patPrimed_) {
            lastPatternPort_ = patPort;
            patPrimed_ = true;
        } else if (patPort != lastPatternPort_) {
            patterns_.setActive((int)std::lround(patPort) - 1);   // 1-based port -> 0-based slot
            lastPatternPort_ = patPort;
        }

        // (2) per-slot sample loading, mirroring AudioPlayerNode (worker-thread decode on path change).
        loaded_ = 0;
        for (int v = 0; v < kVoices; ++v) {
            const std::string& path = ctx.in<std::string>((std::size_t)(4 * v));
            if (loaders_[v].request(path, [path] { return decodeAudioFile(path); })) {
                haveClip_[v] = false; clips_[v] = AudioClip{};
            }
            AudioClip done;
            if (loaders_[v].poll(done)) { clips_[v] = std::move(done); haveClip_[v] = clips_[v].ok; }
            if (haveClip_[v]) ++loaded_;
        }

        // (3) derive the step that fires this frame (-1 = none).
        bool sync = ctx.in<bool>(kSyncIdx);
        int  fire = -1;
        if (sync) {
            int div = std::clamp((int)std::lround(ctx.in<float>(kRateSyncIdx)), 0, 7);
            double barsPerStep = stepDivisionBars(div);
            if (ctx.transport && ctx.transport->playing && barsPerStep > 0.0) {
                long long stepAbs = (long long)std::floor(ctx.transport->bars() / barsPerStep);
                if (!primed_ || stepAbs != lastStepAbs_) {
                    fire = (int)(((stepAbs % kSteps) + kSteps) % kSteps);
                    lastStepAbs_ = stepAbs; primed_ = true;
                }
            } else {
                primed_ = false;   // stopped: re-prime so resume fires cleanly
            }
        } else {
            float tempo = ctx.in<float>(kTempoIdx);
            if (tempo < 1.0f) tempo = 1.0f;
            const double period = 15.0 / tempo;          // seconds per 16th note (4 per beat)
            clock_ += ctx.dt;
            while (clock_ >= nextStep_) {                 // last crossed step wins (1/frame at normal dt)
                fire = freeStep_;
                freeStep_ = (freeStep_ + 1) % kSteps;
                nextStep_ += period;
            }
        }

        // (4) on a step boundary, (re)trigger the on/accent rows of that column.
        if (fire >= 0) {
            for (int v = 0; v < kVoices; ++v) {
                int st = patterns_.cell(v, fire);
                if (st != 0 && haveClip_[v]) {
                    double rate = (double)ctx.in<float>((std::size_t)(4 * v + 2));
                    voices_[v].trigger(clips_[v], rate < 0.0);
                    accent_[v] = (st == 2);
                }
            }
        }

        // (5) render + mix the voices into the stereo block.
        int n = audioBlockFrames(48000, ctx.dt);
        std::fill(outL_.begin(), outL_.begin() + n, 0.0f);
        std::fill(outR_.begin(), outR_.begin() + n, 0.0f);
        for (int v = 0; v < kVoices; ++v) {
            if (!haveClip_[v] || !voices_[v].active()) continue;
            float vol  = std::clamp(ctx.in<float>((std::size_t)(4 * v + 1)), 0.0f, 1.0f);
            float rate = ctx.in<float>((std::size_t)(4 * v + 2));
            float pan  = std::clamp(ctx.in<float>((std::size_t)(4 * v + 3)), -1.0f, 1.0f);
            float g    = vol * (accent_[v] ? 1.0f : 0.75f);   // accent flag is per-hit (set at trigger)
            PanGains pg = panGains(pan);
            voices_[v].render(clips_[v], (double)rate, g * pg.l, g * pg.r,
                              outL_.data(), outR_.data(), n);
        }

        lastN_ = n;
        ctx.out<AudioRef>(0, AudioRef{outL_.data(), (std::size_t)n, 48000});
        ctx.out<AudioRef>(1, AudioRef{outR_.data(), (std::size_t)n, 48000});
    }

    std::string statusLine() const override {
        return "P" + std::to_string(patterns_.active() + 1) + " \xE2\x80\xA2 "
             + std::to_string(loaded_) + "/4 loaded";
    }

    // Persist only the 8 grids + active index; paths/vol/rate/pan persist as control defaults.
    std::string saveState() const override { return patterns_.encode(); }
    void        loadState(const std::string& s) override { patterns_.decode(s); }

    // --- test seams (GL-free) ---
    void     injectClip(int v, AudioClip clip) {
        if (v < 0 || v >= kVoices) return;
        clips_[v] = std::move(clip); haveClip_[v] = clips_[v].ok;
    }
    DrumPatterns& patterns() { return patterns_; }
    AudioRef leftOut()  const { return AudioRef{outL_.data(), (std::size_t)lastN_, 48000}; }
    AudioRef rightOut() const { return AudioRef{outR_.data(), (std::size_t)lastN_, 48000}; }

private:
    AsyncLoader<AudioClip> loaders_[kVoices];
    AudioClip    clips_[kVoices];
    bool         haveClip_[kVoices] = {false, false, false, false};
    SampleVoice  voices_[kVoices];
    bool         accent_[kVoices]   = {false, false, false, false};
    DrumPatterns patterns_;
    std::vector<float> outL_, outR_;
    int    lastN_   = 0;
    int    loaded_  = 0;
    float  lastPatternPort_ = 1.0f;   // recorded on the first frame (patPrimed_) so a loaded active slot survives
    bool   patPrimed_       = false;
    // sync stepping
    long long lastStepAbs_ = 0;
    bool      primed_      = false;
    // free stepping
    double clock_ = 0.0, nextStep_ = 0.0;
    int    freeStep_ = 0;
};

} // namespace oss
