#include "modules/SpectrographNode.h"
#include "audio/FFT.h"
#include "core/Value.h"
#include <algorithm>
#include <cmath>

namespace oss {

SpectrographNode::SpectrographNode()
    : ShaderNode("Spectrograph", "shaders/spectrograph.frag"),
      gen_(48000, 220.0f),
      window_(kWindow, 0.0f),
      spectrum_(kBins, 0.0f),
      verts_(kBins * 3, 0.0f) {
    addInput("audio", PortType::Audio, AudioRef{});   // unconnected -> internal synth
    addOutput("out", PortType::Texture);
    addOutput("geometry", PortType::Vertex);          // spectrum as a 3D line strip
}

SpectrographNode::~SpectrographNode() {
    if (specTex_) glDeleteTextures(1, &specTex_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
}

void SpectrographNode::initGL() {
    ShaderNode::initGL();
    glGenTextures(1, &specTex_);
    glBindTexture(GL_TEXTURE_2D, specTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, kBins, 1, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenBuffers(1, &vbo_);
}

void SpectrographNode::evaluate(EvalContext& ctx) {
    AudioRef a = ctx.in<AudioRef>(0);

    // Advance the rolling window by one frame's worth of samples. Use the
    // connected source's sample rate when present, else the internal synth's.
    int sr = (a.samples && a.sampleRate > 0) ? a.sampleRate : gen_.sampleRate();
    int adv = std::clamp((int)std::lround(sr * (double)ctx.dt), 1, kWindow);
    // When an external source is connected, ingest exactly the block it gave us
    // (capped to the window). This keeps the rolling window gap-free regardless
    // of the source's per-frame block size, instead of recomputing adv from dt.
    if (a.samples) adv = std::clamp((int)a.count, 1, kWindow);
    std::move(window_.begin() + adv, window_.end(), window_.begin());
    float* tail = window_.data() + (kWindow - adv);

    if (a.samples && a.count >= (std::size_t)adv) {
        std::copy(a.samples + (a.count - adv), a.samples + a.count, tail);
    } else {
        // No audio connected (a.samples==nullptr), or an upstream underrun
        // (a.count<adv): fill the new tail from the internal synth.
        gen_.generate(tail, adv);
    }

    auto mag = magnitudeSpectrum(window_);
    float maxv = 1e-6f;
    for (float m : mag) maxv = std::max(maxv, m);
    for (int i = 0; i < kBins; ++i) spectrum_[i] = mag[i] / maxv;

    render(ctx);   // binds program/FBO, calls setUniforms, draws, outputs TexRef

    // Also stream the spectrum as a 3D line strip (vec3 positions) into a VBO:
    // x = frequency, y = magnitude, z = a gentle bow so it reads as 3D.
    for (int i = 0; i < kBins; ++i) {
        float t = (kBins > 1) ? (float)i / (kBins - 1) : 0.0f;
        verts_[i * 3 + 0] = -1.0f + 2.0f * t;
        verts_[i * 3 + 1] = spectrum_[i] * 1.5f - 0.4f;
        verts_[i * 3 + 2] = 0.25f * std::sin(t * 3.14159265f);
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts_.size() * sizeof(float)),
                 verts_.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    ctx.out<VertexRef>(1, VertexRef{vbo_, kBins});
}

void SpectrographNode::setUniforms(EvalContext&) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, specTex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kBins, 1, GL_RED, GL_FLOAT, spectrum_.data());
    glUniform1i(glGetUniformLocation(program_, "uSpectrum"), 0);
}

} // namespace oss
