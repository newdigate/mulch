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
             std::vector<unsigned int>& idx, std::string& err) {
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &err, path.c_str(),
                          nullptr, /*triangulate=*/true))
        return false;
    pos = attrib.vertices;   // flat xyz, already in float
    for (const auto& shape : shapes)
        for (const auto& corner : shape.mesh.indices)
            idx.push_back((unsigned int)corner.vertex_index);
    if (pos.empty() || idx.empty()) { err = "no triangle geometry in .obj"; return false; }
    return true;
}

bool loadGltf(const std::string& path, std::vector<float>& pos,
              std::vector<unsigned int>& idx, std::string& err) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string warn;
    bool ok = endsWith(path, ".glb")
                  ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
                  : loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!ok) return false;

    // Geometry compression we can't decode (needs the draco / meshopt libraries).
    for (const auto& ext : model.extensionsRequired)
        if (ext == "KHR_draco_mesh_compression" || ext == "EXT_meshopt_compression") {
            err = "glTF requires " + ext + " (compressed geometry, not supported)";
            return false;
        }

    int primCount = 0, posPrims = 0, compressedPrims = 0;
    for (const auto& mesh : model.meshes) {
        for (const auto& prim : mesh.primitives) {
            ++primCount;
            if (prim.extensions.count("KHR_draco_mesh_compression")) { ++compressedPrims; continue; }

            const int mode = prim.mode < 0 ? TINYGLTF_MODE_TRIANGLES : prim.mode;
            if (mode != TINYGLTF_MODE_TRIANGLES &&
                mode != TINYGLTF_MODE_TRIANGLE_STRIP &&
                mode != TINYGLTF_MODE_TRIANGLE_FAN) continue;

            auto pit = prim.attributes.find("POSITION");
            if (pit == prim.attributes.end()) continue;
            const tinygltf::Accessor& pa = model.accessors[pit->second];
            if (pa.bufferView < 0) { ++compressedPrims; continue; }   // sparse/compressed accessor
            ++posPrims;

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

            // Gather this primitive's index sequence (or 0..count-1 if non-indexed).
            std::vector<unsigned int> seq;
            if (prim.indices >= 0) {
                const tinygltf::Accessor&   ia = model.accessors[prim.indices];
                const tinygltf::BufferView& iv = model.bufferViews[ia.bufferView];
                const tinygltf::Buffer&     ib = model.buffers[iv.buffer];
                int istride = ia.ByteStride(iv);
                const unsigned char* idp = ib.data.data() + iv.byteOffset + ia.byteOffset;
                seq.reserve(ia.count);
                for (size_t e = 0; e < ia.count; ++e) {
                    const unsigned char* p = idp + e * (size_t)(istride > 0 ? istride : 1);
                    unsigned int v = 0;
                    switch (ia.componentType) {
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  v = *p; break;
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: v = *reinterpret_cast<const uint16_t*>(p); break;
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   v = *reinterpret_cast<const uint32_t*>(p); break;
                        default: break;
                    }
                    seq.push_back(v);
                }
            } else {
                seq.reserve(pa.count);
                for (size_t v = 0; v < pa.count; ++v) seq.push_back((unsigned int)v);
            }

            // Expand the sequence into triangle indices according to the mode.
            if (mode == TINYGLTF_MODE_TRIANGLES) {
                for (unsigned int v : seq) idx.push_back(base + v);
            } else if (mode == TINYGLTF_MODE_TRIANGLE_STRIP) {
                for (size_t i = 2; i < seq.size(); ++i) {
                    if (i % 2 == 0) { idx.push_back(base+seq[i-2]); idx.push_back(base+seq[i-1]); idx.push_back(base+seq[i]); }
                    else            { idx.push_back(base+seq[i-1]); idx.push_back(base+seq[i-2]); idx.push_back(base+seq[i]); }
                }
            } else {  // TRIANGLE_FAN
                for (size_t i = 2; i < seq.size(); ++i) {
                    idx.push_back(base+seq[0]); idx.push_back(base+seq[i-1]); idx.push_back(base+seq[i]);
                }
            }
        }
    }

    if (!pos.empty() && !idx.empty()) return true;
    if (compressedPrims > 0)   err = "glTF mesh uses compressed geometry (Draco/sparse) -- not supported";
    else if (model.meshes.empty()) err = "glTF contains no meshes";
    else if (primCount == 0)   err = "glTF meshes have no primitives";
    else if (posPrims == 0)    err = "glTF has no triangle/strip/fan primitives with POSITION";
    else                       err = "glTF triangle data was empty";
    return false;
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

MeshData loadMeshData(const std::string& path, float scale) {
    MeshData data;
    std::vector<float> pos;
    std::vector<unsigned int> idx;
    std::string err;
    bool parsed;
    if (endsWith(path, ".obj"))
        parsed = loadObj(path, pos, idx, err);
    else if (endsWith(path, ".gltf") || endsWith(path, ".glb"))
        parsed = loadGltf(path, pos, idx, err);
    else { data.error = "unsupported file type (use .obj, .gltf or .glb)"; return data; }

    if (!parsed) { data.error = err.empty() ? "could not read file" : err; return data; }
    normalize(pos, scale);
    data.lines = trianglesToLineList(pos, idx);
    data.tris  = trianglesToShadedList(pos, idx);
    data.ok = !data.lines.empty() || !data.tris.empty();
    if (!data.ok) data.error = "no triangle geometry";
    return data;
}

} // namespace oss
