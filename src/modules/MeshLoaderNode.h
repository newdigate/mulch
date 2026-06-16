#pragma once
#include <glad/gl.h>
#include <future>
#include <string>
#include "core/Node.h"
#include "gfx/MeshLoader.h"   // MeshData

namespace oss {

// Loads a .obj or .gltf/.glb file (named by its string input) ON A WORKER THREAD
// and streams it as two geometry outputs: a wireframe edge list (output 0) and a
// shaded triangle list with normals (output 1). Parsing -- the expensive part --
// runs off the main thread and is re-run only when the file path changes; the
// resulting GL buffers are uploaded on the main thread once parsing completes.
// `scale` is applied cheaply on the main thread (no re-parse). Until a load
// finishes, the previously-loaded geometry keeps streaming.
class MeshLoaderNode : public Node {
public:
    MeshLoaderNode();
    ~MeshLoaderNode() override;
    void initGL() override;
    void evaluate(EvalContext& ctx) override;

private:
    void uploadScaled(float scale);

    GLuint vboLines_ = 0;
    GLuint vboTris_  = 0;
    int    lineCount_ = 0;
    int    triCount_  = 0;

    std::string           requestedPath_;       // last path a parse was started for
    std::future<MeshData> pending_;             // in-flight worker parse
    MeshData              unit_;                 // cached unit-scale geometry
    bool                  haveUnit_     = false;
    float                 appliedScale_ = -1.0f; // scale currently uploaded
};

} // namespace oss
