#pragma once
#include <glad/gl.h>
#include "gfx/ShaderNode.h"
#include "gfx/GLUtil.h"

namespace oss {

class ColourNode : public ShaderNode {
public:
    ColourNode() : ShaderNode("Colour", "shaders/colour.frag") {
        addInput("colour", PortType::Colour, glm::vec4(1.0f, 0.5f, 0.1f, 1.0f));
        addOutput("out", PortType::Texture);
    }
    void evaluate(EvalContext& ctx) override { render(ctx); }

protected:
    void setUniforms(EvalContext& ctx) override {
        glm::vec4 c = ctx.in<glm::vec4>(0);
        glUniform4f(glGetUniformLocation(program_, "uColour"), c.r, c.g, c.b, c.a);
    }
};

} // namespace oss
