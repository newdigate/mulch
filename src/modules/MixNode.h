#pragma once
#include <glad/gl.h>
#include "gfx/ShaderNode.h"

namespace oss {

class MixNode : public ShaderNode {
public:
    MixNode() : ShaderNode("Mix", "shaders/mix.frag") {
        addInput("a", PortType::Texture, TexRef{});
        addInput("b", PortType::Texture, TexRef{});
        addInput("factor", PortType::Float, 0.5f);
        addOutput("out", PortType::Texture);
    }
    void evaluate(EvalContext& ctx) override { render(ctx); }

protected:
    void setUniforms(EvalContext& ctx) override {
        TexRef a = ctx.in<TexRef>(0);
        TexRef b = ctx.in<TexRef>(1);
        float  f = ctx.in<float>(2);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, a.id);
        glUniform1i(glGetUniformLocation(program_, "uA"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, b.id);
        glUniform1i(glGetUniformLocation(program_, "uB"), 1);
        glUniform1f(glGetUniformLocation(program_, "uFactor"), f);
    }
};

} // namespace oss
