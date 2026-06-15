#include "gfx/FullscreenPass.h"

namespace oss {

void FullscreenPass::create() { glGenVertexArrays(1, &vao_); }

void FullscreenPass::draw() const {
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

FullscreenPass::~FullscreenPass() { if (vao_) glDeleteVertexArrays(1, &vao_); }

} // namespace oss
