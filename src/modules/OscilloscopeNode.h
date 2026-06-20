#pragma once
#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/Oscilloscope.h"
#include "audio/SignalGenerator.h"

namespace oss {

// Turns an audio signal into an oscilloscope trace as streamed geometry: a line strip of
// vec3 positions published on output 0 (wire into the Wireframe node). Two modes: a
// Two mono inputs (left, right); waveform uses left, X-Y uses left->x, right->y. A lone connected side mirrors to the other.
// The trace math is the GL-free buildScopeVertices; this node owns the rolling audio
// history, an internal synth fallback when `audio` is unconnected, and the VBO upload.
class OscilloscopeNode : public Node {
public:
    OscilloscopeNode()
        : Node("Oscilloscope"), gen_(48000, 220.0f),
          histL_(kHistory, 0.0f), histR_(kHistory, 0.0f),
          verts_((std::size_t)kPoints * 3, 0.0f) {
        addInput("left",  PortType::Audio, AudioRef{});       // unconnected -> internal synth
        addInput("right", PortType::Audio, AudioRef{});
        addChoiceInput("mode", {"Waveform", "X-Y"}, 0);
        addInput("trigger", PortType::Bool, true);            // rising-edge lock (waveform only)
        addInput("window", PortType::Float, 20.0f, 1.0f, 100.0f);   // milliseconds
        addInput("gain", PortType::Float, 1.0f, 0.1f, 4.0f);
        addOutput("geometry", PortType::Vertex);
    }
    ~OscilloscopeNode() override { if (vbo_) glDeleteBuffers(1, &vbo_); }

    void initGL() override { glGenBuffers(1, &vbo_); }

    void evaluate(EvalContext& ctx) override {
        AudioRef aL = ctx.in<AudioRef>(0);
        AudioRef aR = ctx.in<AudioRef>(1);
        bool hasAudio = (aL.samples && aL.count > 0) || (aR.samples && aR.count > 0);
        const AudioRef& sL = (aL.samples && aL.count > 0) ? aL : aR;   // mirror lone side
        const AudioRef& sR = (aR.samples && aR.count > 0) ? aR : aL;
        int sr = hasAudio ? sL.sampleRate : gen_.sampleRate();
        std::size_t framesL = sL.samples ? sL.count : 0;
        std::size_t framesR = sR.samples ? sR.count : 0;
        std::size_t frames  = std::max(framesL, framesR);

        // Advance the rolling history by one frame's worth of samples.
        int adv = hasAudio ? std::clamp((int)frames, 1, kHistory)
                           : std::clamp((int)std::lround(sr * (double)ctx.dt), 1, kHistory);
        std::move(histL_.begin() + adv, histL_.end(), histL_.begin());
        std::move(histR_.begin() + adv, histR_.end(), histR_.begin());
        float* tailL = histL_.data() + (kHistory - adv);
        float* tailR = histR_.data() + (kHistory - adv);

        if (hasAudio && frames >= (std::size_t)adv) {
            std::size_t start = frames - (std::size_t)adv;
            for (int i = 0; i < adv; ++i) {
                std::size_t f = start + (std::size_t)i;
                tailL[i] = (f < framesL) ? sL.samples[f] : 0.0f;
                tailR[i] = (f < framesR) ? sR.samples[f] : 0.0f;
            }
        } else {
            scratch_.resize((std::size_t)adv);                 // no audio / underrun -> synth
            gen_.generate(scratch_.data(), (std::size_t)adv);
            for (int i = 0; i < adv; ++i) { tailL[i] = scratch_[i]; tailR[i] = scratch_[i]; }
        }

        float windowMs = ctx.in<float>(4);
        int windowSamples = std::clamp((int)std::lround(windowMs / 1000.0 * sr),
                                       2, kHistory / 2);
        ScopeMode mode = ((int)std::lround(ctx.in<float>(2)) == 1) ? ScopeMode::XY
                                                                   : ScopeMode::Waveform;
        buildScopeVertices(histL_.data(), histR_.data(), (std::size_t)kHistory,
                           windowSamples, kPoints, mode, ctx.in<bool>(3), ctx.in<float>(5), verts_);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts_.size() * sizeof(float)),
                     verts_.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        ctx.out<VertexRef>(0, VertexRef{vbo_, kPoints, Primitive::LineStrip, VertexFormat::Pos3});

        mode_ = mode;
        windowMs_ = (int)std::lround(windowMs);
    }

    std::string statusLine() const override {
        return mode_ == ScopeMode::XY ? std::string("X-Y")
                                      : ("waveform · " + std::to_string(windowMs_) + " ms");
    }

private:
    static constexpr int kHistory = 16384;   // rolling sample history (per channel)
    static constexpr int kPoints  = 512;     // fixed vertex count of the trace
    SignalGenerator    gen_;
    std::vector<float> histL_, histR_;
    std::vector<float> verts_;               // kPoints*3 floats (owns the VertexRef storage)
    std::vector<float> scratch_;             // synth fill buffer
    GLuint             vbo_ = 0;
    ScopeMode          mode_ = ScopeMode::Waveform;
    int                windowMs_ = 20;
};

} // namespace oss
