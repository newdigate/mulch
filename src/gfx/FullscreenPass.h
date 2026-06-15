#pragma once
#include <glad/gl.h>

namespace oss {

// Draws a single fullscreen triangle from gl_VertexID (no vertex buffer needed).
// GL 4.1 core still requires a bound VAO, so we keep an empty one.
class FullscreenPass {
public:
    FullscreenPass() = default;
    ~FullscreenPass();
    // Owns a GL VAO handle; copying would double-free it on destruction.
    FullscreenPass(const FullscreenPass&)            = delete;
    FullscreenPass& operator=(const FullscreenPass&) = delete;

    void create();
    void draw() const;
private:
    GLuint vao_ = 0;
};

} // namespace oss
