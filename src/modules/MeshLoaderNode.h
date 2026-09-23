#pragma once
#include <glad/gl.h>
#include <string>
#include "core/Node.h"
#include "core/AsyncLoader.h"
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
    std::string statusLine() const override { return status_; }
    bool loading() const override { return loader_.pending(); }

    // Test/inspection accessor: how many times the geometry has been uploaded to GL. Lets a test
    // see both that a load DID upload and that a steady state stops re-uploading.
    int uploadCount() const { return uploadCount_; }

private:
    void uploadScaled(float scale);

    std::string status_;   // "loading...", "loaded: N triangles", or "load failed: ..."

    GLuint vboLines_ = 0;
    GLuint vboTris_  = 0;
    int    lineCount_ = 0;
    int    triCount_  = 0;

    AsyncLoader<MeshData> loader_;               // worker-thread parse, keyed on path
    MeshData              unit_;                 // cached unit-scale geometry
    bool                  haveUnit_     = false;
    float                 appliedScale_ = 0.0f;  // scale currently uploaded (meaningless until needsUpload_ is false)
    // "upload on the next evaluate", set when a parse lands. A separate flag, NOT a sentinel
    // value of appliedScale_: `scale` is a normal Float port, so a connected node can hand over
    // any value including the sentinel itself, and the upload guard (`scale != appliedScale_`)
    // would then be false on exactly the frame the mesh needed uploading.
    bool                  needsUpload_  = false;
    int                   uploadCount_  = 0;     // test/inspection only
};

} // namespace oss
