#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "audio/AcidVoice.h"

namespace oss {

// 303-style monophonic "acid bass" synth voice: MIDI in -> mono audio out. Wraps the
// GL-free AcidVoice DSP; every control is an input port (wire any for CV/automation).
//
// Inputs: 0 = midi, 1 = waveform (choice), 2 = cutoff, 3 = resonance, 4 = env mod,
//   5 = decay, 6 = accent, 7 = sub level, 8 = slide, 9 = slide time, 10 = filter FM,
//   11 = key track, 12 = distortion, 13 = level.
class AcidNode : public Node {
public:
    AcidNode() : Node("Acid Bass"), buffer_(kMaxBlock, 0.0f) {
        addInput("midi", PortType::Midi, MidiRef{});
        addChoiceInput("waveform", {"Saw", "Square"}, 0);
        addInput("cutoff",     PortType::Float, 800.0f, 20.0f,  12000.0f);
        addInput("resonance",  PortType::Float, 0.7f,   0.0f,   1.0f);
        addInput("env mod",    PortType::Float, 0.6f,   0.0f,   1.0f);
        addInput("decay",      PortType::Float, 0.3f,   0.03f,  2.0f);
        addInput("accent",     PortType::Float, 0.4f,   0.0f,   1.0f);
        addInput("sub level",  PortType::Float, 0.0f,   0.0f,   1.0f);
        addInput("slide",      PortType::Float, 0.0f,   0.0f,   1.0f);
        addInput("slide time", PortType::Float, 0.08f,  0.005f, 0.5f);
        addInput("filter FM",  PortType::Float, 0.0f,   0.0f,   1.0f);
        addInput("key track",  PortType::Float, 0.0f,   0.0f,   1.0f);
        addInput("distortion", PortType::Float, 0.0f,   0.0f,   1.0f);
        addInput("level",      PortType::Float, 0.7f,   0.0f,   1.0f);
        addOutput("audio", PortType::Audio);
        voice_.setSampleRate(sampleRate_);
    }

    void evaluate(EvalContext& ctx) override {
        bool slide = ctx.in<float>(8) >= 0.5f;
        MidiRef midi = ctx.in<MidiRef>(0);
        for (std::size_t i = 0; i < midi.count; ++i) {
            const MidiEvent& e = midi.events[i];
            if (midiIsNoteOn(e))       voice_.noteOn(e.data1, e.data2, slide);
            else if (midiIsNoteOff(e)) voice_.noteOff(e.data1);
        }
        voice_.setWaveform((int)std::lround(ctx.in<float>(1)));
        voice_.setCutoff(ctx.in<float>(2));
        voice_.setResonance(ctx.in<float>(3));
        voice_.setEnvMod(ctx.in<float>(4));
        voice_.setDecay(ctx.in<float>(5));
        voice_.setAccent(ctx.in<float>(6));
        voice_.setSubLevel(ctx.in<float>(7));
        voice_.setSlideTime(ctx.in<float>(9));
        voice_.setFilterFM(ctx.in<float>(10));
        voice_.setKeyTrack(ctx.in<float>(11));
        voice_.setDistortion(ctx.in<float>(12));
        voice_.setLevel(ctx.in<float>(13));

        int n = std::clamp((int)std::lround(sampleRate_ * (double)ctx.dt), 1, kMaxBlock);
        voice_.process(buffer_.data(), n);
        ctx.out<AudioRef>(0, AudioRef{buffer_.data(), (std::size_t)n, sampleRate_});
    }

private:
    static constexpr int kMaxBlock = 2048;
    int sampleRate_ = 48000;
    AcidVoice voice_;
    std::vector<float> buffer_;   // owns the samples the AudioRef points at
};

} // namespace oss
