#pragma once
#include <glad/gl.h>
#include <algorithm>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/PitchGraph.h"

namespace oss {

// Turns incoming MIDI into a scrolling pitch-vs-time graph as colored line geometry: each
// held note is a horizontal segment at its pitch, coloured by pitch class (rainbow hue)
// and velocity (brightness), scrolling right-to-left over `window` seconds. Publishes a
// Pos3Color3 VertexRef on output 0 -> wire into the Wireframe renderer (set its spin to 0
// for a static view). The graph math is the GL-free core/PitchGraph.
class PitchGraphNode : public Node {
public:
    PitchGraphNode() : Node("Pitch Graph") {
        addInput("midi", PortType::Midi, MidiRef{});
        addInput("window", PortType::Float, 8.0f, 1.0f, 30.0f);   // seconds of history shown
        addOutput("geometry", PortType::Vertex);
    }
    ~PitchGraphNode() override { if (vbo_) glDeleteBuffers(1, &vbo_); }
    void initGL() override { glGenBuffers(1, &vbo_); }

    void evaluate(EvalContext& ctx) override {
        float window = std::clamp(ctx.in<float>(1), 1.0f, 30.0f);
        pg_.ingest(ctx.in<MidiRef>(0), ctx.dt, window);
        int n = pg_.build(window, verts_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts_.size() * sizeof(float)),
                     verts_.empty() ? nullptr : verts_.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        ctx.out<VertexRef>(0, VertexRef{vbo_, n, Primitive::Lines, VertexFormat::Pos3Color3});
        held_ = pg_.activeCount();
    }

    std::string statusLine() const override { return std::to_string(held_) + " held"; }

private:
    PitchGraph         pg_;
    std::vector<float> verts_;       // owns the VertexRef storage (Pos3Color3 floats)
    GLuint             vbo_  = 0;
    int                held_ = 0;
};

} // namespace oss
