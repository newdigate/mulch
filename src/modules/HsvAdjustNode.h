#pragma once
#include <glad/gl.h>
#include "gfx/ShaderNode.h"

namespace oss {

// Shifts the hue and scales the saturation/brightness of an input texture. A ShaderNode -- the
// colour math lives in shaders/hsv_adjust.frag, which mirrors the GL-free core/ColorHsv.h
// (`adjustHsv`); a gl_smoke scenario cross-checks the two. Mirrors CompositorNode.
class HsvAdjustNode : public ShaderNode {
public:
    HsvAdjustNode() : ShaderNode("HSV Adjust", "shaders/hsv_adjust.frag") {
        addInput("image", PortType::Texture, TexRef{});
        addInput("hue",        PortType::Float, 0.0f, -1.0f, 1.0f);   // turns (wraps)
        addInput("saturation", PortType::Float, 1.0f,  0.0f, 2.0f);   // multiplier
        addInput("brightness", PortType::Float, 1.0f,  0.0f, 2.0f);   // multiplier
        addOutput("out", PortType::Texture);
    }
    void evaluate(EvalContext& ctx) override { render(ctx); }

protected:
    void setUniforms(EvalContext& ctx) override {
        TexRef in = ctx.in<TexRef>(0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, in.id);
        glUniform1i(glGetUniformLocation(program_, "uImage"), 0);
        glUniform1f(glGetUniformLocation(program_, "uHue"),    ctx.in<float>(1));
        glUniform1f(glGetUniformLocation(program_, "uSat"),    ctx.in<float>(2));
        glUniform1f(glGetUniformLocation(program_, "uBright"), ctx.in<float>(3));
    }
};

} // namespace oss
