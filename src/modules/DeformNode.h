#pragma once
#include <glad/gl.h>
#include <string>
#include <vector>
#include <glm/vec4.hpp>
#include "core/Node.h"
#include "core/Value.h"
#include "gfx/GLUtil.h"

namespace oss {

// Runs a vertex shader (from a Shader edge) over an input vertex buffer via GPU transform
// feedback, capturing the transformed vertices into a Pos3Color3 output buffer. `position`
// (uPos) and `colour` (uColour) drive the shader; the input may be Pos3 or Pos3Color3.
// No/invalid shader -> the input passes through unchanged.
class DeformNode : public Node {
public:
    DeformNode() : Node("Deform") {
        addInput("geometry", PortType::Vertex, VertexRef{});
        addInput("position", PortType::Float, 0.5f, -2.0f, 2.0f);
        addInput("colour", PortType::Colour, glm::vec4(1.0f));
        addInput("shader", PortType::Shader, ShaderRef{});
        addOutput("geometry", PortType::Vertex);
    }
    ~DeformNode() override {
        if (program_) glDeleteProgram(program_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (outVbo_) glDeleteBuffers(1, &outVbo_);
    }
    void initGL() override { glGenVertexArrays(1, &vao_); glGenBuffers(1, &outVbo_); }

    void evaluate(EvalContext& ctx) override {
        const ShaderRef& sh = ctx.in<ShaderRef>(3);
        if (sh.vertexSrc != cachedSrc_) {        // (re)compile only when the source changes
            if (program_) { glDeleteProgram(program_); program_ = 0; }
            if (!sh.vertexSrc.empty())
                program_ = linkFeedbackProgram(sh.vertexSrc, { "vPosition", "vColor" });
            cachedSrc_ = sh.vertexSrc;
            status_ = sh.vertexSrc.empty() ? "no shader" : (program_ ? "ok" : "compile error");
        }

        VertexRef in = ctx.in<VertexRef>(0);
        if (program_ == 0 || in.vbo == 0 || in.count <= 0) {
            out_ = in;                            // pass through
            ctx.out<VertexRef>(0, out_);
            return;
        }

        if (in.count > outCap_) {                 // grow the capture buffer on demand
            glBindBuffer(GL_ARRAY_BUFFER, outVbo_);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)in.count * 6 * sizeof(float), nullptr, GL_DYNAMIC_COPY);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            outCap_ = in.count;
        }

        glUseProgram(program_);
        glUniform1f(glGetUniformLocation(program_, "uPos"), ctx.in<float>(1));
        glm::vec4 c = ctx.in<glm::vec4>(2);
        glUniform4f(glGetUniformLocation(program_, "uColour"), c.r, c.g, c.b, c.a);

        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, in.vbo);
        bool hasColor = (in.format == VertexFormat::Pos3Color3);
        GLsizei stride = (in.format == VertexFormat::Pos3) ? 3 * sizeof(float) : 6 * sizeof(float);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
        if (hasColor) {
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
        } else {
            glDisableVertexAttribArray(1);
            glVertexAttrib4f(1, 0.0f, 0.0f, 0.0f, 1.0f);   // pin aColor=0 (generic attrib is context state)
        }

        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, outVbo_);
        glEnable(GL_RASTERIZER_DISCARD);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, in.count);
        glEndTransformFeedback();
        glDisable(GL_RASTERIZER_DISCARD);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);

        out_ = VertexRef{ outVbo_, in.count, in.primitive, VertexFormat::Pos3Color3 };
        ctx.out<VertexRef>(0, out_);
    }

    VertexRef   output() const { return out_; }        // last published geometry (for tests)
    std::string statusLine() const override { return status_; }

private:
    GLuint program_ = 0, vao_ = 0, outVbo_ = 0;
    int    outCap_ = 0;
    VertexRef   out_;
    std::string cachedSrc_, status_ = "no shader";
};

} // namespace oss
