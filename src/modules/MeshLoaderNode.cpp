#include "modules/MeshLoaderNode.h"
#include "core/Value.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace oss {

MeshLoaderNode::MeshLoaderNode() : Node("Mesh Loader") {
    addInput("file",  PortType::String, std::string(""));   // .obj / .gltf / .glb path
    addInput("scale", PortType::Float, 1.0f, 0.1f, 5.0f);
    addOutput("wireframe", PortType::Vertex);   // output 0: Pos3 / Lines
    addOutput("shaded",    PortType::Vertex);   // output 1: Pos3Normal3 / Triangles
}

MeshLoaderNode::~MeshLoaderNode() {
    // pending_'s destructor joins the worker; its task captures the path by value
    // (not `this`), so finishing after we're gone would be harmless anyway.
    if (vboLines_) glDeleteBuffers(1, &vboLines_);
    if (vboTris_)  glDeleteBuffers(1, &vboTris_);
}

void MeshLoaderNode::initGL() {
    glGenBuffers(1, &vboLines_);
    glGenBuffers(1, &vboTris_);
}

void MeshLoaderNode::evaluate(EvalContext& ctx) {
    const std::string& path = ctx.in<std::string>(0);
    float scale = ctx.in<float>(1);

    // Start a worker parse when the file path changes (the expensive step).
    if (path != requestedPath_) {
        requestedPath_ = path;
        haveUnit_ = false;
        lineCount_ = triCount_ = 0;
        appliedScale_ = -1.0f;
        if (path.empty()) {
            pending_ = {};
            status_.clear();
        } else {
            // Parse to unit scale on a worker thread; `scale` is applied later.
            status_ = "loading...";
            std::fprintf(stderr, "[Mesh] loading %s\n", path.c_str());
            pending_ = std::async(std::launch::async,
                                  [path] { return loadMeshData(path, 1.0f); });
        }
    }

    // When the worker has finished, cache its result (GL upload happens below).
    if (pending_.valid() &&
        pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        unit_ = pending_.get();
        haveUnit_ = unit_.ok;
        appliedScale_ = -1.0f;   // force an upload at the current scale
        if (unit_.ok) {
            int tris = (int)(unit_.tris.size() / 18);   // 18 floats per triangle
            status_ = "loaded: " + std::to_string(tris) + " triangles";
            std::fprintf(stderr, "[Mesh] loaded %s: %d triangles\n", requestedPath_.c_str(), tris);
        } else {
            status_ = "load failed: " + unit_.error;
            std::fprintf(stderr, "[Mesh] load failed: %s\n", unit_.error.c_str());
        }
    }

    // Apply scale cheaply on the main thread (re-upload, no re-parse).
    if (haveUnit_ && scale != appliedScale_) uploadScaled(scale);

    ctx.out<VertexRef>(0, VertexRef{lineCount_ > 0 ? vboLines_ : 0u, lineCount_,
                                    Primitive::Lines, VertexFormat::Pos3});
    ctx.out<VertexRef>(1, VertexRef{triCount_ > 0 ? vboTris_ : 0u, triCount_,
                                    Primitive::Triangles, VertexFormat::Pos3Normal3});
}

void MeshLoaderNode::uploadScaled(float scale) {
    appliedScale_ = scale;

    std::vector<float> lines = unit_.lines;            // every float is a position
    for (float& f : lines) f *= scale;

    std::vector<float> tris = unit_.tris;              // 6 floats/vertex: pos*scale, normal kept
    for (std::size_t i = 0; i + 5 < tris.size(); i += 6) {
        tris[i + 0] *= scale; tris[i + 1] *= scale; tris[i + 2] *= scale;
    }

    glBindBuffer(GL_ARRAY_BUFFER, vboLines_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(lines.size() * sizeof(float)),
                 lines.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, vboTris_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(tris.size() * sizeof(float)),
                 tris.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    lineCount_ = (int)(lines.size() / 3);
    triCount_  = (int)(tris.size() / 6);
}

} // namespace oss
