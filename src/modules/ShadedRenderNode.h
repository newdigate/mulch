#pragma once
#include <glad/gl.h>
#include "core/Node.h"
#include "gfx/Framebuffer.h"

namespace oss {

// Renders a streamed triangle vertex buffer (position + normal) as a solid,
// diffuse-lit surface into its own framebuffer, through a slowly rotating 3D
// camera, and publishes the result as a texture. Expects geometry in the
// Pos3Normal3 / Triangles format (e.g. the Mesh Loader's shaded output);
// anything else just clears to the background.
class ShadedRenderNode : public Node {
public:
    ShadedRenderNode();
    ~ShadedRenderNode() override;
    void initGL() override;
    void evaluate(EvalContext& ctx) override;

private:
    Framebuffer fbo_;
    GLuint program_ = 0;
    GLuint vao_     = 0;
    float  angle_   = 0.0f;   // accumulated camera rotation (radians)
};

} // namespace oss
