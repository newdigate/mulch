#include <doctest/doctest.h>
#include "core/VertexTrail.h"
#include "core/ColorHsv.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

TEST_CASE("a single Pos3 snapshot builds to a red vertex at its position") {
    VertexTrail t;
    float v[3] = {1.0f, 2.0f, 3.0f};
    t.push(v, 1, VertexFormat::Pos3, Primitive::Lines);
    std::vector<float> out;
    int n = t.build(0.5f, 0.1f, out);
    REQUIRE(n == 1);
    REQUIRE(out.size() == 6);
    CHECK(out[0] == doctest::Approx(1.0f));
    CHECK(out[1] == doctest::Approx(2.0f));
    CHECK(out[2] == doctest::Approx(3.0f));     // age 0, no z offset
    CHECK(out[3] == doctest::Approx(1.0f));     // red base
    CHECK(out[4] == doctest::Approx(0.0f));
    CHECK(out[5] == doctest::Approx(0.0f));
    CHECK(t.frameCount() == 1);
    CHECK(t.primitive() == Primitive::Lines);
}

TEST_CASE("older snapshots get z-offset and hue rotation by age") {
    VertexTrail t;
    float a[3] = {0.0f, 0.0f, 0.0f};   // pushed first  -> age 1
    float b[3] = {0.0f, 0.0f, 0.0f};   // pushed second -> age 0 (newest)
    t.push(a, 1, VertexFormat::Pos3, Primitive::Lines);
    t.push(b, 1, VertexFormat::Pos3, Primitive::Lines);
    std::vector<float> out;
    int n = t.build(0.5f, 0.1f, out);
    REQUIRE(n == 2);
    CHECK(out[2] == doctest::Approx(0.0f));     // age 0: z 0
    CHECK(out[3] == doctest::Approx(1.0f));     // age 0: red
    glm::vec3 c = hsvToRgb(0.1f, 1.0f, 1.0f);
    CHECK(out[8]  == doctest::Approx(0.5f));    // age 1: z += 0.5
    CHECK(out[9]  == doctest::Approx(c.x));     // age 1: hue rotated 0.1
    CHECK(out[10] == doctest::Approx(c.y));
    CHECK(out[11] == doctest::Approx(c.z));
}

TEST_CASE("setMaxFrames prunes the oldest snapshots") {
    VertexTrail t;
    float v[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 5; ++i) t.push(v, 1, VertexFormat::Pos3, Primitive::Lines);
    t.setMaxFrames(2);
    CHECK(t.frameCount() == 2);
    std::vector<float> out;
    CHECK(t.build(0.5f, 0.0f, out) == 2);
}

TEST_CASE("a Pos3Color3 snapshot keeps its colour at age 0") {
    VertexTrail t;
    float v[6] = {1.0f, 2.0f, 3.0f, 0.0f, 1.0f, 0.0f};   // green
    t.push(v, 1, VertexFormat::Pos3Color3, Primitive::Lines);
    std::vector<float> out;
    t.build(0.5f, 0.1f, out);
    CHECK(out[3] == doctest::Approx(0.0f));
    CHECK(out[4] == doctest::Approx(1.0f));
    CHECK(out[5] == doctest::Approx(0.0f));   // green preserved (age 0, no hue shift)
}

TEST_CASE("a LineStrip snapshot is emitted as independent segments, not a joined strip") {
    VertexTrail t;
    // A 3-point strip: v0,v1,v2. As a strip it is 2 segments (v0-v1, v1-v2).
    float strip[9] = { 0.0f,0.0f,0.0f,  1.0f,1.0f,0.0f,  2.0f,2.0f,0.0f };
    t.push(strip, 3, VertexFormat::Pos3, Primitive::LineStrip);
    std::vector<float> out;
    int n = t.build(0.5f, 0.0f, out);

    CHECK(n == 4);                               // 2 segments * 2 verts = 4 (expanded), not 3 (old strip)
    CHECK(t.primitive() == Primitive::Lines);    // strip input -> independent segments on output
    REQUIRE(out.size() == 24);                   // 4 verts * 6 floats
    // segment 0 = (v0, v1); segment 1 = (v1, v2) -- the shared middle vertex v1 appears twice
    CHECK(out[0]  == doctest::Approx(0.0f)); CHECK(out[1]  == doctest::Approx(0.0f));   // v0
    CHECK(out[6]  == doctest::Approx(1.0f)); CHECK(out[7]  == doctest::Approx(1.0f));   // v1
    CHECK(out[12] == doctest::Approx(1.0f)); CHECK(out[13] == doctest::Approx(1.0f));   // v1 (shared)
    CHECK(out[18] == doctest::Approx(2.0f)); CHECK(out[19] == doctest::Approx(2.0f));   // v2
}

TEST_CASE("stacked LineStrip snapshots never connect across copies") {
    VertexTrail t;
    float strip[9] = { 0.0f,0.0f,0.0f,  1.0f,1.0f,0.0f,  2.0f,2.0f,0.0f };
    t.push(strip, 3, VertexFormat::Pos3, Primitive::LineStrip);   // becomes age 1
    t.push(strip, 3, VertexFormat::Pos3, Primitive::LineStrip);   // age 0 (newest)
    std::vector<float> out;
    int n = t.build(0.5f, 0.0f, out);
    REQUIRE(n == 8);                             // 2 snapshots * 4 expanded verts
    // age 0 (verts 0..3): z == 0; age 1 (verts 4..7): z == 0.5. Each segment is a consecutive pair
    // WITHIN one snapshot, so no segment ever spans the z=0 -> z=0.5 boundary (no join).
    CHECK(out[2]  == doctest::Approx(0.0f));     // age0 seg0 v0.z
    CHECK(out[8]  == doctest::Approx(0.0f));     // age0 seg0 v1.z
    CHECK(out[14] == doctest::Approx(0.0f));     // age0 seg1 v1.z
    CHECK(out[20] == doctest::Approx(0.0f));     // age0 seg1 v2.z
    CHECK(out[26] == doctest::Approx(0.5f));     // age1 seg0 v0.z (offset)
    CHECK(out[44] == doctest::Approx(0.5f));     // age1 seg1 v2.z (offset)
}
