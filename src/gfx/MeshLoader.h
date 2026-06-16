#pragma once
#include <string>
#include <vector>

namespace oss {

// Load a .obj or .gltf/.glb file (selected by extension) and produce a flat
// GL_LINES vertex array (tightly-packed vec3 endpoints) of the mesh's triangle
// edges, centered and uniformly scaled to roughly fit a unit box times `scale`.
// Returns false (leaving outLines untouched) if the file can't be read or has
// no triangle geometry. GL-free -- parsing and math only.
bool loadMeshLineList(const std::string& path, float scale, std::vector<float>& outLines);

} // namespace oss
