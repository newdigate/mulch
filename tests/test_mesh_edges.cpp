#include <doctest/doctest.h>
#include "gfx/MeshEdges.h"
#include <vector>

using namespace oss;

TEST_CASE("a single triangle expands to three edges (6 vertices)") {
    std::vector<float> pos = {0,0,0,  1,0,0,  0,1,0};
    std::vector<unsigned int> idx = {0, 1, 2};
    auto lines = trianglesToLineList(pos, idx);
    REQUIRE(lines.size() == 6 * 3);                  // 6 vertices x 3 floats

    // edge a-b: (0,0,0) -> (1,0,0)
    CHECK(lines[0] == 0); CHECK(lines[3] == 1);
    // edge b-c: (1,0,0) -> (0,1,0)
    CHECK(lines[6] == 1); CHECK(lines[10] == 1);
    // edge c-a: (0,1,0) -> (0,0,0)
    CHECK(lines[13] == 1); CHECK(lines[15] == 0);
}

TEST_CASE("two triangles expand to twelve line vertices") {
    std::vector<float> pos = {0,0,0, 1,0,0, 0,1,0, 1,1,0};
    std::vector<unsigned int> idx = {0,1,2, 1,3,2};
    CHECK(trianglesToLineList(pos, idx).size() == 12 * 3);
}

TEST_CASE("triangles with out-of-range indices are skipped") {
    std::vector<float> pos = {0,0,0, 1,0,0, 0,1,0};
    std::vector<unsigned int> idx = {0,1,2, 0,1,99};   // 2nd triangle invalid
    CHECK(trianglesToLineList(pos, idx).size() == 6 * 3);
}

TEST_CASE("a trailing partial triangle is ignored") {
    std::vector<float> pos = {0,0,0, 1,0,0, 0,1,0};
    std::vector<unsigned int> idx = {0,1,2, 0};        // dangling index
    CHECK(trianglesToLineList(pos, idx).size() == 6 * 3);
}

TEST_CASE("shaded expansion yields 3 vertices each with the face normal") {
    std::vector<float> pos = {0,0,0, 1,0,0, 0,1,0};    // CCW in the XY plane
    std::vector<unsigned int> idx = {0, 1, 2};
    auto s = trianglesToShadedList(pos, idx);
    REQUIRE(s.size() == 3 * 6);                         // 3 verts x (pos3 + normal3)
    // vertex 0: position (0,0,0), normal (0,0,1)
    CHECK(s[0] == 0); CHECK(s[1] == 0); CHECK(s[2] == 0);
    CHECK(s[3] == doctest::Approx(0));
    CHECK(s[4] == doctest::Approx(0));
    CHECK(s[5] == doctest::Approx(1));
    // vertex 1's normal matches the same face
    CHECK(s[11] == doctest::Approx(1));
}

TEST_CASE("shaded expansion skips degenerate/out-of-range triangles") {
    std::vector<float> pos = {0,0,0, 1,0,0, 0,1,0};
    std::vector<unsigned int> idx = {0,1,2, 0,1,99};
    CHECK(trianglesToShadedList(pos, idx).size() == 3 * 6);
}

TEST_CASE("shaded expansion uses supplied per-vertex normals (smooth shading)") {
    std::vector<float> pos = {0,0,0, 1,0,0, 0,1,0};   // face normal is +Z
    std::vector<unsigned int> idx = {0, 1, 2};
    std::vector<float> nrm = {0,1,0,  0,1,0,  0,1,0};  // supplied normals are +Y
    auto s = trianglesToShadedList(pos, idx, nrm);
    REQUIRE(s.size() == 3 * 6);
    CHECK(s[3] == doctest::Approx(0));   // vertex 0 normal.x
    CHECK(s[4] == doctest::Approx(1));   // vertex 0 normal.y (supplied, not the face's)
    CHECK(s[5] == doctest::Approx(0));   // vertex 0 normal.z
}

TEST_CASE("shaded expansion falls back to the face normal for a degenerate supplied normal") {
    std::vector<float> pos = {0,0,0, 1,0,0, 0,1,0};
    std::vector<unsigned int> idx = {0, 1, 2};
    std::vector<float> nrm = {0,0,0,  0,0,0,  0,0,0};  // all zero -> use face normal (+Z)
    auto s = trianglesToShadedList(pos, idx, nrm);
    REQUIRE(s.size() == 3 * 6);
    CHECK(s[5] == doctest::Approx(1));   // fell back to face normal.z
}
