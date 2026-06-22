#pragma once
#include <glad/gl.h>
#include <vector>
#include "core/Node.h"
#include "gfx/Framebuffer.h"

namespace oss {

// Renders a streamed vertex buffer (Pos3 line strip/lines in a fixed green, or a
// Pos3Color3 buffer with per-vertex colour) as a wireframe into its own framebuffer,
// viewed through a slowly rotating 3D camera, and publishes a texture on output 0.
class WireframeNode : public Node {
public:
    WireframeNode();
    ~WireframeNode() override;
    void initGL() override;
    void evaluate(EvalContext& ctx) override;

private:
    Framebuffer fbo_;
    GLuint program_       = 0;
    GLuint program_color_ = 0;   // per-vertex-colour variant (Pos3Color3)
    GLuint vao_           = 0;
    float  angle_         = 0.0f;   // accumulated camera rotation (radians)
    std::vector<GLint>   firsts_;   // multi-draw scratch (per-strip first index)
    std::vector<GLsizei> counts_;   // multi-draw scratch (per-strip vertex count)
};

} // namespace oss
