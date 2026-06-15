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
      spectrum_(kBins, 0.0f) {
    addInput("audio", PortType::Audio, AudioRef{});   // unconnected -> internal synth
    addOutput("out", PortType::Texture);
}

SpectrographNode::~SpectrographNode() {
    if (specTex_) glDeleteTextures(1, &specTex_);
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
}

void SpectrographNode::evaluate(EvalContext& ctx) {
    // Advance the rolling window by one frame's worth of samples.
    int adv = std::clamp((int)std::lround(gen_.sampleRate() * (double)ctx.dt), 1, kWindow);
    std::move(window_.begin() + adv, window_.end(), window_.begin());
    float* tail = window_.data() + (kWindow - adv);

    AudioRef a = ctx.in<AudioRef>(0);
    if (a.samples && a.count >= (std::size_t)adv) {
        std::copy(a.samples + (a.count - adv), a.samples + a.count, tail);
    } else {
        gen_.generate(tail, adv);   // unconnected default: synth
    }

    auto mag = magnitudeSpectrum(window_);
    float maxv = 1e-6f;
    for (float m : mag) maxv = std::max(maxv, m);
    for (int i = 0; i < kBins; ++i) spectrum_[i] = mag[i] / maxv;

    render(ctx);   // binds program/FBO, calls setUniforms, draws, outputs TexRef
}

void SpectrographNode::setUniforms(EvalContext&) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, specTex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kBins, 1, GL_RED, GL_FLOAT, spectrum_.data());
    glUniform1i(glGetUniformLocation(program_, "uSpectrum"), 0);
}

} // namespace oss
