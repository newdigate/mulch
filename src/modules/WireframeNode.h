#pragma once
#include <glad/gl.h>
#include "core/Node.h"
#include "gfx/Framebuffer.h"

namespace oss {

// Renders a streamed vertex buffer (a line strip of vec3 positions) as a
// wireframe into its own framebuffer, viewed through a slowly rotating 3D
// camera, and publishes the result as a texture on output 0.
class WireframeNode : public Node {
public:
    WireframeNode();
    ~WireframeNode() override;
    void initGL() override;
    void evaluate(EvalContext& ctx) override;

private:
    Framebuffer fbo_;
    GLuint program_ = 0;
    GLuint vao_     = 0;
    float  angle_   = 0.0f;   // accumulated camera rotation (radians)
};

} // namespace oss
