#include "gfx/ShaderNode.h"
#include "gfx/GLUtil.h"
#include "gfx/Canvas.h"
#include "core/Preferences.h"
#include "core/Value.h"

namespace oss {

static const char* kFullscreenVS = R"(#version 410 core
out vec2 vUV;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0,
                  gl_VertexID == 2 ? 3.0 : -1.0);
    vUV = (p + 1.0) * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

ShaderNode::~ShaderNode() {
    if (program_) glDeleteProgram(program_);
}

void ShaderNode::initGL() {
    program_ = linkProgram(kFullscreenVS, readFile(fragPath_));
    fbo_.create(kCanvasW, kCanvasH);
    fsq_.create();
}

void ShaderNode::render(EvalContext& ctx) {
    int w = ctx.prefs ? ctx.prefs->textureWidth  : kCanvasW;
    int h = ctx.prefs ? ctx.prefs->textureHeight : kCanvasH;
    if (fbo_.width() != w || fbo_.height() != h) fbo_.create(w, h);
    fbo_.bind();
    glUseProgram(program_);
    setUniforms(ctx);
    fsq_.draw();
    Framebuffer::unbind();
    ctx.out<TexRef>(0, TexRef{ fbo_.texture(), fbo_.width(), fbo_.height() });
}

} // namespace oss
