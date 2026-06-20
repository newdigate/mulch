#pragma once
#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/VertexTrail.h"

namespace oss {

// Snapshots the incoming vertex buffer each frame into a trail: a queue of up to `max frames`
// copies, each offset in Z by age * `z spacing` and hue-rotated by age * `hue rate`. Emits the
// combined trail as one Pos3Color3 buffer -> wire into Wireframe. Reads the input VBO back to
// CPU each frame (a GPU sync; suits modest vertex counts).
class VertexTrailNode : public Node {
public:
    VertexTrailNode() : Node("Vertex Trail") {
        addInput("geometry",   PortType::Vertex, VertexRef{});
        addInput("max frames", PortType::Float, 16.0f, 1.0f, 240.0f);
        addInput("z spacing",  PortType::Float, 0.15f, -2.0f, 2.0f);
        addInput("hue rate",   PortType::Float, 0.03f, -1.0f, 1.0f);
        addOutput("geometry", PortType::Vertex);
    }
    ~VertexTrailNode() override { if (outVbo_) glDeleteBuffers(1, &outVbo_); }
    void initGL() override { glGenBuffers(1, &outVbo_); }

    void evaluate(EvalContext& ctx) override {
        VertexRef in = ctx.in<VertexRef>(0);
        maxF_ = std::clamp((int)std::lround(ctx.in<float>(1)), 1, 240);
        trail_.setMaxFrames(maxF_);

        if (in.vbo != 0 && in.count > 0) {
            int fl = (in.format == VertexFormat::Pos3) ? 3 : 6;
            readback_.resize((std::size_t)in.count * fl);
            glBindBuffer(GL_ARRAY_BUFFER, in.vbo);
            glGetBufferSubData(GL_ARRAY_BUFFER, 0,
                               (GLsizeiptr)in.count * fl * sizeof(float), readback_.data());
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            trail_.push(readback_.data(), in.count, in.format, in.primitive);
        }

        int total = trail_.build(ctx.in<float>(2), ctx.in<float>(3), built_);
        glBindBuffer(GL_ARRAY_BUFFER, outVbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(built_.size() * sizeof(float)),
                     built_.empty() ? nullptr : built_.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        frames_ = trail_.frameCount();
        out_ = VertexRef{ outVbo_, total, trail_.primitive(), VertexFormat::Pos3Color3 };
        ctx.out<VertexRef>(0, out_);
    }

    VertexRef   output() const { return out_; }     // last published geometry (for tests)
    std::string statusLine() const override { return std::to_string(frames_) + "/" + std::to_string(maxF_); }

private:
    VertexTrail trail_;
    GLuint outVbo_ = 0;
    int    frames_ = 0, maxF_ = 16;
    VertexRef out_;
    std::vector<float> readback_, built_;
};

} // namespace oss
