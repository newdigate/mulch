#pragma once
#include <glad/gl.h>

namespace oss {

// Saves the GL state a foreign renderer (projectM) disturbs and restores it on scope exit, so the
// nodes and ImGui that run afterwards see what they left. Audited against projectM 4.2 with every
// GL call traced: on the hot path it leaves the draw AND read framebuffer, the viewport and the
// blend func/enable changed, and sampler objects bound on texture units 1..N.
//  - Texture bindings: only the 2D binding of the unit that was active on entry is restored; other
//    units are left as the foreign renderer set them, because our nodes bind what they sample.
//  - Sampler bindings on units 0..15 are CLEARED to 0 (not restored): our nodes never bind sampler
//    objects, and a leftover one silently overrides a texture's own filter/wrap parameters.
//  - An element-array binding made while a VAO is bound is VAO state and returns with the VAO.
// A few saved items (unpack alignment, depth mask, depth/cull/scissor enables) projectM 4.2 does
// not touch today; they are cheap insurance (the whole guard costs well under a microsecond).
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
        // projectM binds a sampler object per texture unit (main + blur + preset textures) and
        // unbinds only unit 0, so units 1..N keep its wrap/filter and would override the texture
        // parameters of whatever WE bind there next. Nothing in this app binds sampler objects, so
        // 0 is the state every node assumes: clearing is cheaper and safer than saving 16 of them.
        for (GLuint unit = 0; unit < kSamplerUnits; ++unit) glBindSampler(unit, 0);
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

    static constexpr GLuint kSamplerUnits = 16;   // GL 4.1 guarantees MAX_TEXTURE_IMAGE_UNITS >= 16

    GLint drawFbo_ = 0, readFbo_ = 0, viewport_[4] = {0, 0, 0, 0};
    GLint program_ = 0, vao_ = 0, arrayBuffer_ = 0, activeTexture_ = GL_TEXTURE0, texture2d_ = 0;
    GLint blendSrcRgb_ = GL_ONE, blendDstRgb_ = GL_ZERO, blendSrcAlpha_ = GL_ONE, blendDstAlpha_ = GL_ZERO;
    GLint unpackAlignment_ = 4;
    GLboolean depthMask_ = GL_TRUE, blend_ = GL_FALSE, depth_ = GL_FALSE, cull_ = GL_FALSE, scissor_ = GL_FALSE;
};

} // namespace oss
