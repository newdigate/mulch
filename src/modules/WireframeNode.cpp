#include "modules/WireframeNode.h"
#include "gfx/GLUtil.h"
#include "gfx/Canvas.h"
#include "core/Preferences.h"
#include "core/Value.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace oss {

static const char* kWireVS = R"(#version 410 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
void main() { gl_Position = uMVP * vec4(aPos, 1.0); }
)";

static const char* kWireFS = R"(#version 410 core
out vec4 FragColor;
void main() { FragColor = vec4(0.12, 0.95, 0.45, 1.0); }
)";

static const char* kWireColorVS = R"(#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform mat4 uMVP;
out vec3 vColor;
void main() { vColor = aColor; gl_Position = uMVP * vec4(aPos, 1.0); }
)";

static const char* kWireColorFS = R"(#version 410 core
in vec3 vColor;
out vec4 FragColor;
void main() { FragColor = vec4(vColor, 1.0); }
)";

WireframeNode::WireframeNode() : Node("Wireframe") {
    addInput("geometry", PortType::Vertex, VertexRef{});
    addInput("spin", PortType::Float, 0.5f, 0.0f, 2.0f);   // self-rotation speed (rad/s)
    addInput("transform", PortType::Transform, Transform{});   // shared world transform (overrides spin)
    addOutput("out", PortType::Texture);
}

WireframeNode::~WireframeNode() {
    if (program_) glDeleteProgram(program_);
    if (program_color_) glDeleteProgram(program_color_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void WireframeNode::initGL() {
    program_ = linkProgram(kWireVS, kWireFS);
    program_color_ = linkProgram(kWireColorVS, kWireColorFS);
    fbo_.create(kCanvasW, kCanvasH);
    glGenVertexArrays(1, &vao_);
}

void WireframeNode::evaluate(EvalContext& ctx) {
    int texW = ctx.prefs ? ctx.prefs->textureWidth  : kCanvasW;
    int texH = ctx.prefs ? ctx.prefs->textureHeight : kCanvasH;
    if (fbo_.width() != texW || fbo_.height() != texH) fbo_.create(texW, texH);

    VertexRef geo = ctx.in<VertexRef>(0);

    // A connected World Transform overrides the node's own spin so several
    // renderers share one rotation and stay aligned.
    Transform tf = ctx.in<Transform>(2);
    float yaw, pitch;
    if (tf.active) { yaw = tf.angle; pitch = tf.pitch; }
    else { angle_ += ctx.dt * ctx.in<float>(1); yaw = angle_; pitch = 0.0f; }

    fbo_.bind();
    glClearColor(0.03f, 0.03f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (geo.vbo != 0 && geo.count > 0) {
        float aspect = fbo_.height() ? (float)fbo_.width() / (float)fbo_.height() : 1.7778f;
        // Camera matches Shaded Render so the two views register when blended.
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.4f, 2.8f),
                                     glm::vec3(0.0f, 0.0f, 0.0f),
                                     glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 model = glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f))
                        * glm::rotate(glm::mat4(1.0f), pitch, glm::vec3(1.0f, 0.0f, 0.0f));
        glm::mat4 mvp = proj * view * model;

        bool colored = (geo.format == VertexFormat::Pos3Color3);
        GLuint prog = colored ? program_color_ : program_;
        glUseProgram(prog);
        glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));

        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, geo.vbo);
        glEnableVertexAttribArray(0);
        if (colored) {
            const GLsizei stride = 6 * sizeof(float);   // pos(3) + colour(3)
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
        } else {
            glDisableVertexAttribArray(1);   // clear any stale colour attribute (one shared VAO)
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        }
        GLenum prim = geo.primitive == Primitive::Lines     ? GL_LINES
                    : geo.primitive == Primitive::Triangles ? GL_TRIANGLES
                                                            : GL_LINE_STRIP;
        glDrawArrays(prim, 0, geo.count);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }

    Framebuffer::unbind();
    ctx.out<TexRef>(0, TexRef{fbo_.texture(), fbo_.width(), fbo_.height()});
}

} // namespace oss
