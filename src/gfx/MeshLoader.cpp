#include "gfx/MeshLoader.h"
#include "gfx/MeshEdges.h"

// tinygltf is header-only: pull its implementation in here, geometry only.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include "tiny_gltf.h"
// tinyobjloader's implementation is provided by its linked library target.
#include "tiny_obj_loader.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>

namespace oss {
namespace {

bool endsWith(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    std::string tail = s.substr(s.size() - suffix.size());
    std::transform(tail.begin(), tail.end(), tail.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return tail == suffix;
}

bool loadObj(const std::string& path, std::vector<float>& pos,
             std::vector<unsigned int>& idx) {
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string err;
    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &err, path.c_str(),
                          nullptr, /*triangulate=*/true)) {
        std::fprintf(stderr, "[Mesh] obj load failed: %s\n", err.c_str());
        return false;
    }
    pos = attrib.vertices;   // flat xyz, already in float
    for (const auto& shape : shapes)
        for (const auto& corner : shape.mesh.indices)
            idx.push_back((unsigned int)corner.vertex_index);
    return !pos.empty() && !idx.empty();
}

bool loadGltf(const std::string& path, std::vector<float>& pos,
              std::vector<unsigned int>& idx) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;
    bool ok = endsWith(path, ".glb")
                  ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
                  : loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!ok) { std::fprintf(stderr, "[Mesh] gltf load failed: %s\n", err.c_str()); return false; }

    for (const auto& mesh : model.meshes) {
        for (const auto& prim : mesh.primitives) {
            if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1) continue;
            auto pit = prim.attributes.find("POSITION");
            if (pit == prim.attributes.end()) continue;

            const tinygltf::Accessor&   pa = model.accessors[pit->second];
            const tinygltf::BufferView& pv = model.bufferViews[pa.bufferView];
            const tinygltf::Buffer&     pb = model.buffers[pv.buffer];
            int pstride = pa.ByteStride(pv);
            if (pstride <= 0) pstride = (int)(3 * sizeof(float));
            const unsigned char* pd = pb.data.data() + pv.byteOffset + pa.byteOffset;

            const unsigned int base = (unsigned int)(pos.size() / 3);
            for (size_t v = 0; v < pa.count; ++v) {
                const float* f = reinterpret_cast<const float*>(pd + v * (size_t)pstride);
                pos.push_back(f[0]); pos.push_back(f[1]); pos.push_back(f[2]);
            }

            if (prim.indices >= 0) {
                const tinygltf::Accessor&   ia = model.accessors[prim.indices];
                const tinygltf::BufferView& iv = model.bufferViews[ia.bufferView];
                const tinygltf::Buffer&     ib = model.buffers[iv.buffer];
                int istride = ia.ByteStride(iv);
                const unsigned char* idp = ib.data.data() + iv.byteOffset + ia.byteOffset;
                for (size_t e = 0; e < ia.count; ++e) {
                    const unsigned char* p = idp + e * (size_t)(istride > 0 ? istride : 1);
                    unsigned int v = 0;
                    switch (ia.componentType) {
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  v = *p; break;
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: v = *reinterpret_cast<const uint16_t*>(p); break;
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   v = *reinterpret_cast<const uint32_t*>(p); break;
                        default: break;
                    }
                    idx.push_back(base + v);
                }
            } else {
                for (size_t v = 0; v < pa.count; ++v) idx.push_back(base + (unsigned int)v);
            }
        }
    }
    return !pos.empty() && !idx.empty();
}

// Center the mesh at the origin and uniformly scale it to ~fit a unit box.
void normalize(std::vector<float>& pos, float scale) {
    if (pos.empty()) return;
    float mn[3] = {pos[0], pos[1], pos[2]};
    float mx[3] = {pos[0], pos[1], pos[2]};
    for (size_t i = 0; i < pos.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], pos[i + k]);
            mx[k] = std::max(mx[k], pos[i + k]);
        }
    float ctr[3], ext = 0.0f;
    for (int k = 0; k < 3; ++k) { ctr[k] = 0.5f * (mn[k] + mx[k]); ext = std::max(ext, mx[k] - mn[k]); }
    if (ext <= 0.0f) ext = 1.0f;
    const float s = (1.6f / ext) * scale;
    for (size_t i = 0; i < pos.size(); i += 3)
        for (int k = 0; k < 3; ++k) pos[i + k] = (pos[i + k] - ctr[k]) * s;
}

} // namespace

bool loadMeshLineList(const std::string& path, float scale, std::vector<float>& outLines) {
    std::vector<float> pos;
    std::vector<unsigned int> idx;
    bool ok = endsWith(path, ".obj")  ? loadObj(path, pos, idx)
            : (endsWith(path, ".gltf") || endsWith(path, ".glb")) ? loadGltf(path, pos, idx)
            : false;
    if (!ok) return false;
    normalize(pos, scale);
    outLines = trianglesToLineList(pos, idx);
    return !outLines.empty();
}

} // namespace oss
