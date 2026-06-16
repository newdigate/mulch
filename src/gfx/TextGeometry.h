#pragma once
#include <string>
#include <vector>

namespace oss {

// CPU-side geometry built from a text string and a TrueType font. GL-free: it
// produces float buffers the Text nodes upload to VBOs (mirroring MeshLoader's
// output shape), consumed by the Wireframe and Shaded Render nodes.
//
//   lines  - GL_LINES vertex list, 3 floats/vertex (Pos3): the glyph outlines.
//   tris   - GL_TRIANGLES vertex list, 6 floats/vertex (Pos3 + Normal3): the
//            filled letters; flat (front only, +Z normals) when depth == 0, or a
//            solid extruded slab (front +Z, back -Z, side walls) when depth > 0.
//
// The result is centred on the origin and scaled so the em height maps to `size`
// world units, which frames it for the Wireframe/Shaded cameras (~unit scale).
struct TextGeometry {
    bool               ok = false;
    std::vector<float> lines;
    std::vector<float> tris;
    std::string        error;
};

// Build geometry for `text` using the TTF at `fontPath`. `size` is the em height
// in world units; `depth` is the extrusion thickness in world units (0 = flat 2D).
TextGeometry buildTextGeometry(const std::string& text, const std::string& fontPath,
                               float size, float depth);

} // namespace oss
