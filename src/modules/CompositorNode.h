#pragma once
#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include "gfx/ShaderNode.h"
#include "core/BlendModes.h"

namespace oss {

// Blends two input textures with a selectable operator (23 modes from core/BlendModes.h)
// and an opacity. A ShaderNode -- the per-mode math lives in shaders/compositor.frag,
// which mirrors blendPixel(); the GL-free reference is unit-tested and gl_smoke
// cross-checks the shader against it. Mirrors MixNode.
class CompositorNode : public ShaderNode {
public:
    CompositorNode() : ShaderNode("Compositor", "shaders/compositor.frag") {
        addInput("a", PortType::Texture, TexRef{});
        addInput("b", PortType::Texture, TexRef{});
        addChoiceInput("mode", blendModeLabels(), 0);            // Normal
        addInput("opacity", PortType::Float, 1.0f, 0.0f, 1.0f);
        addOutput("out", PortType::Texture);
    }
    void evaluate(EvalContext& ctx) override { render(ctx); }

protected:
    void setUniforms(EvalContext& ctx) override {
        TexRef a = ctx.in<TexRef>(0);
        TexRef b = ctx.in<TexRef>(1);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, a.id);
        glUniform1i(glGetUniformLocation(program_, "uA"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, b.id);
        glUniform1i(glGetUniformLocation(program_, "uB"), 1);
        int mode = std::clamp((int)std::lround(ctx.in<float>(2)),
                              0, (int)blendModeLabels().size() - 1);
        glUniform1i(glGetUniformLocation(program_, "uMode"), mode);
        glUniform1f(glGetUniformLocation(program_, "uOpacity"), ctx.in<float>(3));
        glActiveTexture(GL_TEXTURE0);   // restore default active unit
    }
};

} // namespace oss
