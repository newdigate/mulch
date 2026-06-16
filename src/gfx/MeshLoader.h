#pragma once
#include <string>
#include <vector>

namespace oss {

// A parsed mesh, normalized to roughly fit a unit box, ready for GL upload:
//   lines - Pos3 GL_LINES edge list (wireframe), 3 floats/vertex
//   tris  - Pos3Normal3 GL_TRIANGLES list (shaded),  6 floats/vertex
// `ok` is false if the file couldn't be read or had no triangle geometry.
struct MeshData {
    bool ok = false;
    std::vector<float> lines;
    std::vector<float> tris;
};

// Load a .obj or .gltf/.glb file (by extension), center + uniformly scale it,
// and build both representations. GL-free -- safe to call on a worker thread.
MeshData loadMeshData(const std::string& path, float scale);

} // namespace oss
