#pragma once
#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include "gfx/ShaderNode.h"

namespace oss {

// Folds an input texture into a mirrored kaleidoscopic pattern. A ShaderNode -- the
// polar fold math lives in shaders/kaleidoscope.frag. All params are input ports, so
// `rotation` can be wired to an LFO/Automation to spin. Mirrors CompositorNode.
class KaleidoscopeNode : public ShaderNode {
public:
    KaleidoscopeNode() : ShaderNode("Kaleidoscope", "shaders/kaleidoscope.frag") {
        addInput("image", PortType::Texture, TexRef{});
        addIntInput("segments", 6, 2, 32);
        addInput("rotation", PortType::Float, 0.0f, 0.0f, 1.0f);   // turns
        addInput("zoom",     PortType::Float, 1.0f, 0.1f, 4.0f);
        addInput("center x", PortType::Float, 0.5f, 0.0f, 1.0f);
        addInput("center y", PortType::Float, 0.5f, 0.0f, 1.0f);
        addOutput("out", PortType::Texture);
    }
    void evaluate(EvalContext& ctx) override { render(ctx); }

protected:
    void setUniforms(EvalContext& ctx) override {
        TexRef in = ctx.in<TexRef>(0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, in.id);
        glUniform1i(glGetUniformLocation(program_, "uImage"), 0);

        int segments = std::max(1, (int)std::lround(ctx.in<float>(1)));
        glUniform1i(glGetUniformLocation(program_, "uSegments"), segments);
        glUniform1f(glGetUniformLocation(program_, "uRotation"),
                    ctx.in<float>(2) * 6.28318530718f);            // turns -> radians
        glUniform1f(glGetUniformLocation(program_, "uZoom"), ctx.in<float>(3));
        glUniform2f(glGetUniformLocation(program_, "uCenter"),
                    ctx.in<float>(4), ctx.in<float>(5));
    }
};

} // namespace oss
