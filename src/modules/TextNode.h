#pragma once
#include <glad/gl.h>
#include <string>
#include "core/Node.h"

namespace oss {

// Shared base for the text-to-geometry nodes. Rebuilds CPU geometry from the text
// and parameters only when they change, uploads two VBOs, and streams them with
// the same output shape as Mesh Loader: a wireframe outline (Lines/Pos3, output 0)
// and a filled/extruded mesh (Triangles/Pos3Normal3, output 1).
class TextNodeBase : public Node {
public:
    ~TextNodeBase() override;
    void initGL() override;
    std::string statusLine() const override { return status_; }

protected:
    explicit TextNodeBase(std::string name);
    void buildIfChanged(const std::string& text, float size, float depth, const std::string& font);
    void outputVertexRefs(EvalContext& ctx);

private:
    GLuint      vboLines_ = 0, vboTris_ = 0;
    int         lineCount_ = 0, triCount_ = 0;   // vertex counts
    std::string status_;
    std::string lastText_, lastFont_;
    float       lastSize_ = -1.0f, lastDepth_ = -1.0f;
    bool        built_ = false;
};

// Flat text: outlines + filled letters in the z = 0 plane (depth fixed at 0).
class Text2DNode : public TextNodeBase {
public:
    Text2DNode();
    void evaluate(EvalContext& ctx) override;
};

// Solid text: extruded letters with front, back, and side faces (real normals).
class Text3DNode : public TextNodeBase {
public:
    Text3DNode();
    void evaluate(EvalContext& ctx) override;
};

} // namespace oss
