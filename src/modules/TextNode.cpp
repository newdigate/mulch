#include "modules/TextNode.h"
#include "gfx/TextGeometry.h"
#include "core/Value.h"
#include <cstdio>
#include <string>

#ifndef OSS_DEFAULT_FONT
#define OSS_DEFAULT_FONT ""   // set by CMake to a bundled ImGui TTF
#endif

namespace oss {

TextNodeBase::TextNodeBase(std::string name) : Node(std::move(name)) {}

TextNodeBase::~TextNodeBase() {
    if (vboLines_) glDeleteBuffers(1, &vboLines_);
    if (vboTris_)  glDeleteBuffers(1, &vboTris_);
}

void TextNodeBase::initGL() {
    glGenBuffers(1, &vboLines_);
    glGenBuffers(1, &vboTris_);
}

void TextNodeBase::buildIfChanged(const std::string& text, float size, float depth,
                                  const std::string& font) {
    if (built_ && text == lastText_ && size == lastSize_ &&
        depth == lastDepth_ && font == lastFont_)
        return;   // nothing relevant changed -> keep the uploaded buffers
    lastText_ = text; lastSize_ = size; lastDepth_ = depth; lastFont_ = font;
    built_ = true;

    const std::string path = font.empty() ? std::string(OSS_DEFAULT_FONT) : font;
    TextGeometry geo = buildTextGeometry(text, path, size, depth);
    if (!geo.ok) {
        lineCount_ = triCount_ = 0;
        status_ = "font error: " + geo.error;
        std::fprintf(stderr, "[Text] %s\n", status_.c_str());
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, vboLines_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(geo.lines.size() * sizeof(float)),
                 geo.lines.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, vboTris_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(geo.tris.size() * sizeof(float)),
                 geo.tris.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    lineCount_ = (int)(geo.lines.size() / 3);   // Pos3
    triCount_  = (int)(geo.tris.size() / 6);    // Pos3Normal3
    status_ = std::to_string(triCount_ / 3) + " triangles";
}

void TextNodeBase::outputVertexRefs(EvalContext& ctx) {
    ctx.out<VertexRef>(0, VertexRef{lineCount_ > 0 ? vboLines_ : 0u, lineCount_,
                                    Primitive::Lines, VertexFormat::Pos3});
    ctx.out<VertexRef>(1, VertexRef{triCount_ > 0 ? vboTris_ : 0u, triCount_,
                                    Primitive::Triangles, VertexFormat::Pos3Normal3});
}

// --- Text 2D: flat outlines + fills (depth fixed at 0) ---
Text2DNode::Text2DNode() : TextNodeBase("Text 2D") {
    addInput("text", PortType::String, std::string("Hello"));
    addInput("size", PortType::Float, 1.0f, 0.1f, 3.0f);
    addInput("font", PortType::String, std::string(""));   // empty -> bundled default
    addOutput("wireframe", PortType::Vertex);   // outline lines (Pos3)
    addOutput("shaded",    PortType::Vertex);   // filled tris (Pos3Normal3)
}

void Text2DNode::evaluate(EvalContext& ctx) {
    buildIfChanged(ctx.in<std::string>(0), ctx.in<float>(1), 0.0f, ctx.in<std::string>(2));
    outputVertexRefs(ctx);
}

// --- Text 3D: extruded solid letters ---
Text3DNode::Text3DNode() : TextNodeBase("Text 3D") {
    addInput("text",  PortType::String, std::string("Hello"));
    addInput("size",  PortType::Float, 1.0f,  0.1f, 3.0f);
    addInput("depth", PortType::Float, 0.25f, 0.0f, 1.0f);   // extrusion thickness
    addInput("font",  PortType::String, std::string(""));
    addOutput("wireframe", PortType::Vertex);
    addOutput("shaded",    PortType::Vertex);
}

void Text3DNode::evaluate(EvalContext& ctx) {
    buildIfChanged(ctx.in<std::string>(0), ctx.in<float>(1), ctx.in<float>(2), ctx.in<std::string>(3));
    outputVertexRefs(ctx);
}

} // namespace oss
