#pragma once
#include <glad/gl.h>
#include <string>
#include "core/Node.h"

namespace oss {

// Loads a .obj or .gltf/.glb file named by its string input and streams the
// mesh as a wireframe (GL_LINES) vertex buffer on its geometry output. The file
// is parsed only when the path or scale changes. Wire its geometry output into
// the Wireframe node to view the model.
class MeshLoaderNode : public Node {
public:
    MeshLoaderNode();
    ~MeshLoaderNode() override;
    void initGL() override;
    void evaluate(EvalContext& ctx) override;

private:
    void reload(const std::string& path, float scale);

    GLuint      vbo_         = 0;
    int         count_       = 0;     // line-list vertex count
    std::string loadedPath_;          // last path parsed (cache key)
    float       loadedScale_ = -1.0f; // last scale applied (cache key)
};

} // namespace oss
