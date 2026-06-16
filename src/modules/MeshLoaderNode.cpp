#include "modules/MeshLoaderNode.h"
#include "gfx/MeshLoader.h"
#include "core/Value.h"
#include <vector>

namespace oss {

MeshLoaderNode::MeshLoaderNode() : Node("Mesh Loader") {
    addInput("file",  PortType::String, std::string(""));   // .obj / .gltf / .glb path
    addInput("scale", PortType::Float, 1.0f, 0.1f, 5.0f);
    addOutput("geometry", PortType::Vertex);
}

MeshLoaderNode::~MeshLoaderNode() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
}

void MeshLoaderNode::initGL() {
    glGenBuffers(1, &vbo_);
}

void MeshLoaderNode::evaluate(EvalContext& ctx) {
    const std::string& path = ctx.in<std::string>(0);
    float scale = ctx.in<float>(1);
    if (path != loadedPath_ || scale != loadedScale_) reload(path, scale);
    ctx.out<VertexRef>(0, VertexRef{count_ > 0 ? vbo_ : 0u, count_, Primitive::Lines});
}

// Parse the file (once per path/scale change) and upload its wireframe edges.
void MeshLoaderNode::reload(const std::string& path, float scale) {
    loadedPath_  = path;
    loadedScale_ = scale;
    count_ = 0;

    std::vector<float> lines;
    if (path.empty() || !loadMeshLineList(path, scale, lines)) return;

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(lines.size() * sizeof(float)),
                 lines.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    count_ = (int)(lines.size() / 3);
}

} // namespace oss
