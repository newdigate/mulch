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

TEST_CASE("a uniform LineStrip trail is kept compact and drawn as one strip") {
    VertexTrail t;
    float strip[9] = { 0.0f,0.0f,0.0f,  1.0f,1.0f,0.0f,  2.0f,2.0f,0.0f };
    t.push(strip, 3, VertexFormat::Pos3, Primitive::LineStrip);
    std::vector<float> out;
    int n = t.build(0.5f, 0.0f, out);
    CHECK(n == 3);                               // compact: 1 vertex per point (not 4 expanded)
    CHECK(t.primitive() == Primitive::LineStrip);
    CHECK(t.stripCount() == 1);                  // one snapshot -> one strip
    REQUIRE(out.size() == 18);
    CHECK(out[0]  == doctest::Approx(0.0f)); CHECK(out[1]  == doctest::Approx(0.0f));   // v0
    CHECK(out[6]  == doctest::Approx(1.0f)); CHECK(out[7]  == doctest::Approx(1.0f));   // v1
    CHECK(out[12] == doctest::Approx(2.0f)); CHECK(out[13] == doctest::Approx(2.0f));   // v2
}

TEST_CASE("stacked uniform LineStrip snapshots are drawn as separate strips") {
    VertexTrail t;
    float strip[9] = { 0.0f,0.0f,0.0f,  1.0f,1.0f,0.0f,  2.0f,2.0f,0.0f };
    t.push(strip, 3, VertexFormat::Pos3, Primitive::LineStrip);   // age 1
    t.push(strip, 3, VertexFormat::Pos3, Primitive::LineStrip);   // age 0 (newest)
    std::vector<float> out;
    int n = t.build(0.5f, 0.0f, out);
    CHECK(n == 6);                               // 2 snapshots * 3 compact verts (not 8 expanded)
    CHECK(t.primitive() == Primitive::LineStrip);
    CHECK(t.stripCount() == 2);                  // each snapshot is its own strip (Wireframe multi-draws)
    CHECK(out[2]  == doctest::Approx(0.0f));     // age0 v0.z
    CHECK(out[14] == doctest::Approx(0.0f));     // age0 v2.z
    CHECK(out[20] == doctest::Approx(0.5f));     // age1 v0.z (offset)
    CHECK(out[32] == doctest::Approx(0.5f));     // age1 v2.z (offset)
}

TEST_CASE("changing the input primitive restarts the trail (no mixed-primitive snapshots)") {
    VertexTrail t;
    float a[3] = { 0.0f, 0.0f, 0.0f };
    t.push(a, 1, VertexFormat::Pos3, Primitive::Lines);
    t.push(a, 1, VertexFormat::Pos3, Primitive::Lines);
    REQUIRE(t.frameCount() == 2);
    float strip[6] = { 0.0f,0.0f,0.0f,  1.0f,1.0f,0.0f };
    t.push(strip, 2, VertexFormat::Pos3, Primitive::LineStrip);   // primitive changed -> flush old snapshots
    CHECK(t.frameCount() == 1);                                   // only the new snapshot remains
    CHECK(t.primitive() == Primitive::LineStrip);
    CHECK(t.stripCount() == 1);
}

TEST_CASE("a LineStrip trail with varying vertex counts falls back to independent segments") {
    VertexTrail t;
    float strip3[9] = { 0.0f,0.0f,0.0f,  1.0f,1.0f,0.0f,  2.0f,2.0f,0.0f };
    float strip2[6] = { 0.0f,0.0f,0.0f,  1.0f,1.0f,0.0f };
    t.push(strip3, 3, VertexFormat::Pos3, Primitive::LineStrip);   // age 1, N=3
    t.push(strip2, 2, VertexFormat::Pos3, Primitive::LineStrip);   // age 0, N=2
    std::vector<float> out;
    int n = t.build(0.5f, 0.0f, out);
    // mixed counts -> expand each strip: 2(N-1) verts -> age0 2(2-1)=2 + age1 2(3-1)=4 = 6
    CHECK(n == 6);
    CHECK(t.primitive() == Primitive::Lines);    // fallback to independent segments
    CHECK(t.stripCount() == 1);
}
