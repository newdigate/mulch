#pragma once
#include <glad/gl.h>

namespace oss {

// Saves the GL state a foreign renderer (projectM) disturbs and restores it on scope exit, so the
// nodes and ImGui that run afterwards see what they left. Element-array bindings are VAO state and
// come back with the VAO. Only the 2D texture binding of the unit that was active on entry is
// restored; other units are left as the foreign renderer set them (our nodes bind what they sample).
class GLStateGuard {
public:
    GLStateGuard() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo_);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo_);
        glGetIntegerv(GL_VIEWPORT, viewport_);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao_);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer_);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture_);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2d_);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb_);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb_);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha_);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha_);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment_);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask_);
        blend_   = glIsEnabled(GL_BLEND);
        depth_   = glIsEnabled(GL_DEPTH_TEST);
        cull_    = glIsEnabled(GL_CULL_FACE);
        scissor_ = glIsEnabled(GL_SCISSOR_TEST);
    }
    ~GLStateGuard() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)drawFbo_);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)readFbo_);
        glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        glUseProgram((GLuint)program_);
        glBindVertexArray((GLuint)vao_);
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)arrayBuffer_);
        glActiveTexture((GLenum)activeTexture_);
        glBindTexture(GL_TEXTURE_2D, (GLuint)texture2d_);
        glBlendFuncSeparate((GLenum)blendSrcRgb_, (GLenum)blendDstRgb_,
                            (GLenum)blendSrcAlpha_, (GLenum)blendDstAlpha_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment_);
        glDepthMask(depthMask_);
        set(GL_BLEND, blend_);
        set(GL_DEPTH_TEST, depth_);
        set(GL_CULL_FACE, cull_);
        set(GL_SCISSOR_TEST, scissor_);
    }
    GLStateGuard(const GLStateGuard&) = delete;
    GLStateGuard& operator=(const GLStateGuard&) = delete;

private:
    static void set(GLenum cap, GLboolean on) { if (on) glEnable(cap); else glDisable(cap); }

    GLint drawFbo_ = 0, readFbo_ = 0, viewport_[4] = {0, 0, 0, 0};
    GLint program_ = 0, vao_ = 0, arrayBuffer_ = 0, activeTexture_ = GL_TEXTURE0, texture2d_ = 0;
    GLint blendSrcRgb_ = GL_ONE, blendDstRgb_ = GL_ZERO, blendSrcAlpha_ = GL_ONE, blendDstAlpha_ = GL_ZERO;
    GLint unpackAlignment_ = 4;
    GLboolean depthMask_ = GL_TRUE, blend_ = GL_FALSE, depth_ = GL_FALSE, cull_ = GL_FALSE, scissor_ = GL_FALSE;
};

} // namespace oss
