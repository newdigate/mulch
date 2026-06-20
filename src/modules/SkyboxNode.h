#pragma once
#include <glad/gl.h>
#include <cstddef>
#include "gfx/ShaderNode.h"

namespace oss {

// Renders 6 face textures as a cubemap background into a texture. The fragment shader
// builds a per-pixel view ray (45 deg FOV, -Z forward to match Wireframe/Shaded),
// rotates it by yaw/pitch (the shared World Transform, or a self yaw-spin when none is
// connected), picks the cube face by major axis, and samples the matching face texture.
class SkyboxNode : public ShaderNode {
public:
    SkyboxNode() : ShaderNode("Skybox", "shaders/skybox.frag") {
        addInput("+X", PortType::Texture, TexRef{});
        addInput("-X", PortType::Texture, TexRef{});
        addInput("+Y", PortType::Texture, TexRef{});
        addInput("-Y", PortType::Texture, TexRef{});
        addInput("+Z", PortType::Texture, TexRef{});
        addInput("-Z", PortType::Texture, TexRef{});
        addInput("rotation", PortType::Float, 0.2f, -2.0f, 2.0f);   // self yaw-spin (rad/s)
        addInput("transform", PortType::Transform, Transform{});    // shared World Transform (yaw+pitch)
        addOutput("out", PortType::Texture);
    }
    void evaluate(EvalContext& ctx) override { render(ctx); }

protected:
    void setUniforms(EvalContext& ctx) override {
        Transform tf = ctx.in<Transform>(7);
        float yaw, pitch;
        if (tf.active) { yaw = tf.angle; pitch = tf.pitch; }
        else { spin_ += ctx.dt * ctx.in<float>(6); yaw = spin_; pitch = 0.0f; }

        static const char* names[6] = { "uPX", "uNX", "uPY", "uNY", "uPZ", "uNZ" };
        for (int i = 0; i < 6; ++i) {
            glActiveTexture(GL_TEXTURE0 + (GLenum)i);
            glBindTexture(GL_TEXTURE_2D, ctx.in<TexRef>((std::size_t)i).id);
            glUniform1i(glGetUniformLocation(program_, names[i]), i);
        }
        glActiveTexture(GL_TEXTURE0);
        glUniform1f(glGetUniformLocation(program_, "uYaw"), yaw);
        glUniform1f(glGetUniformLocation(program_, "uPitch"), pitch);
        float aspect = fbo_.height() ? (float)fbo_.width() / (float)fbo_.height() : 1.7778f;
        glUniform1f(glGetUniformLocation(program_, "uAspect"), aspect);
    }

private:
    float spin_ = 0.0f;
};

} // namespace oss
