#include "modules/ShadedRenderNode.h"
#include "gfx/GLUtil.h"
#include "gfx/Canvas.h"
#include "core/Value.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace oss {

static const char* kVS = R"(#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat3 uNormalMat;
out vec3 vNormal;
void main() {
    vNormal = uNormalMat * aNormal;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

static const char* kFS = R"(#version 410 core
in vec3 vNormal;
out vec4 FragColor;
uniform vec3 uLightDir;
uniform vec4 uColour;
void main() {
    vec3 n = normalize(vNormal);
    float diff = max(dot(n, normalize(uLightDir)), 0.0);
    vec3 c = uColour.rgb * (0.2 + 0.8 * diff);   // ambient + diffuse
    FragColor = vec4(c, 1.0);
}
)";

ShadedRenderNode::ShadedRenderNode() : Node("Shaded Render") {
    addInput("geometry", PortType::Vertex, VertexRef{});
    addInput("colour", PortType::Colour, glm::vec4(0.55f, 0.65f, 0.85f, 1.0f));
    addInput("spin", PortType::Float, 0.5f, 0.0f, 2.0f);   // self-rotation speed (rad/s)
    addInput("transform", PortType::Transform, Transform{});   // shared world transform (overrides spin)
    addOutput("out", PortType::Texture);
}

ShadedRenderNode::~ShadedRenderNode() {
    if (program_) glDeleteProgram(program_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void ShadedRenderNode::initGL() {
    program_ = linkProgram(kVS, kFS);
    fbo_.create(kCanvasW, kCanvasH, /*depth=*/true);
    glGenVertexArrays(1, &vao_);
}

void ShadedRenderNode::evaluate(EvalContext& ctx) {
    VertexRef geo    = ctx.in<VertexRef>(0);
    glm::vec4 colour = ctx.in<glm::vec4>(1);

    // A connected World Transform overrides the node's own spin so several
    // renderers share one rotation and stay aligned.
    Transform tf = ctx.in<Transform>(3);
    float angle;
    if (tf.active) { angle = tf.angle; }
    else { angle_ += ctx.dt * ctx.in<float>(2); angle = angle_; }

    fbo_.bind();
    glClearColor(0.04f, 0.04f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (geo.vbo != 0 && geo.count > 0 &&
        geo.primitive == Primitive::Triangles && geo.format == VertexFormat::Pos3Normal3) {
        glEnable(GL_DEPTH_TEST);
        glUseProgram(program_);

        float aspect = fbo_.height() ? (float)fbo_.width() / (float)fbo_.height() : 1.7778f;
        glm::mat4 proj  = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        glm::mat4 view  = glm::lookAt(glm::vec3(0, 0.4f, 2.8f), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
        glm::mat4 model = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0, 1, 0));
        glm::mat4 mvp = proj * view * model;
        glm::mat3 nrm = glm::mat3(model);   // rotation only -> orthonormal
        glm::vec3 light = glm::normalize(glm::vec3(0.5f, 0.8f, 0.6f));

        glUniformMatrix4fv(glGetUniformLocation(program_, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
        glUniformMatrix3fv(glGetUniformLocation(program_, "uNormalMat"), 1, GL_FALSE, glm::value_ptr(nrm));
        glUniform3fv(glGetUniformLocation(program_, "uLightDir"), 1, glm::value_ptr(light));
        glUniform4fv(glGetUniformLocation(program_, "uColour"), 1, glm::value_ptr(colour));

        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, geo.vbo);
        const GLsizei stride = 6 * sizeof(float);   // pos(3) + normal(3)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
        glDrawArrays(GL_TRIANGLES, 0, geo.count);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
        glDisable(GL_DEPTH_TEST);
    }

    Framebuffer::unbind();
    ctx.out<TexRef>(0, TexRef{fbo_.texture(), fbo_.width(), fbo_.height()});
}

} // namespace oss
