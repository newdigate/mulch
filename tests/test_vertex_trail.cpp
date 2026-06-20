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
