#pragma once
#include <glad/gl.h>

namespace oss {

// An FBO with a single RGBA8 colour-texture attachment.
class Framebuffer {
public:
    Framebuffer() = default;
    ~Framebuffer();
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    void create(int w, int h, bool depth = false);   // optional depth attachment
    void bind() const;            // bind FBO + set viewport to its size
    static void unbind();         // bind default framebuffer (0)

    GLuint texture() const { return tex_; }
    int width()  const { return w_; }
    int height() const { return h_; }

private:
    GLuint fbo_ = 0, tex_ = 0, depth_ = 0;
    int w_ = 0, h_ = 0;
};

} // namespace oss
