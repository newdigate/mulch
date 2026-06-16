#include "modules/WireframeNode.h"
#include "gfx/GLUtil.h"
#include "gfx/Canvas.h"
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

WireframeNode::WireframeNode() : Node("Wireframe") {
    addInput("geometry", PortType::Vertex, VertexRef{});
    addInput("spin", PortType::Float, 0.5f, 0.0f, 2.0f);   // rotation speed (rad/s)
    addOutput("out", PortType::Texture);
}

WireframeNode::~WireframeNode() {
    if (program_) glDeleteProgram(program_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void WireframeNode::initGL() {
    program_ = linkProgram(kWireVS, kWireFS);
    fbo_.create(kCanvasW, kCanvasH);
    glGenVertexArrays(1, &vao_);
}

void WireframeNode::evaluate(EvalContext& ctx) {
    VertexRef geo = ctx.in<VertexRef>(0);
    angle_ += ctx.dt * ctx.in<float>(1);

    fbo_.bind();
    glClearColor(0.03f, 0.03f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (geo.vbo != 0 && geo.count > 0) {
        glUseProgram(program_);
        float aspect = fbo_.height() ? (float)fbo_.width() / (float)fbo_.height() : 1.7778f;
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.4f, 2.6f),
                                     glm::vec3(0.0f, 0.2f, 0.0f),
                                     glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 model = glm::rotate(glm::mat4(1.0f), angle_, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 mvp = proj * view * model;
        glUniformMatrix4fv(glGetUniformLocation(program_, "uMVP"), 1, GL_FALSE,
                           glm::value_ptr(mvp));

        // Bind the upstream node's VBO to our own VAO and draw it as a line strip.
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, geo.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glDrawArrays(GL_LINE_STRIP, 0, geo.count);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }

    Framebuffer::unbind();
    ctx.out<TexRef>(0, TexRef{fbo_.texture(), fbo_.width(), fbo_.height()});
}

} // namespace oss
