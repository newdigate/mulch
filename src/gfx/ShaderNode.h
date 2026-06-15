#pragma once
#include <glad/gl.h>
#include <string>
#include "core/Node.h"
#include "gfx/Framebuffer.h"
#include "gfx/FullscreenPass.h"

namespace oss {

// Base for nodes that render a fragment shader into their own framebuffer and
// publish the result as a TexRef on output port 0.
class ShaderNode : public Node {
public:
    ShaderNode(std::string name, std::string fragPath)
        : Node(std::move(name)), fragPath_(std::move(fragPath)) {}

    void initGL() override;     // compile program + create FBO/VAO

protected:
    // Subclasses bind input textures / set uniforms here (program already bound).
    virtual void setUniforms(EvalContext& ctx) {}

    // Call from evaluate(): renders into fbo_ and writes TexRef to output 0.
    void render(EvalContext& ctx);

    GLuint        program_ = 0;
    Framebuffer   fbo_;
    FullscreenPass fsq_;
    std::string   fragPath_;
};

} // namespace oss
