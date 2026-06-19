#include <doctest/doctest.h>
#include "core/BlendModes.h"
#include <glm/vec3.hpp>
#include <cmath>

using namespace oss;

static bool approx(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return std::fabs(a.x-b.x) < eps && std::fabs(a.y-b.y) < eps && std::fabs(a.z-b.z) < eps;
}
static float lumOf(const glm::vec3& c) { return 0.3f*c.x + 0.59f*c.y + 0.11f*c.z; }

TEST_CASE("blendModeLabels has 23 modes") { CHECK(blendModeLabels().size() == 23); }

TEST_CASE("Normal returns the blend layer") {
    CHECK(approx(blendPixel(0, glm::vec3(0.1f,0.2f,0.3f), glm::vec3(0.7f,0.6f,0.5f)),
                 glm::vec3(0.7f,0.6f,0.5f)));
}

TEST_CASE("Add clamps at 1, Subtract clamps at 0") {
    CHECK(approx(blendPixel(1, glm::vec3(0.6f), glm::vec3(0.6f)), glm::vec3(1.0f)));   // 1.2 -> 1
    CHECK(approx(blendPixel(2, glm::vec3(0.3f), glm::vec3(0.6f)), glm::vec3(0.0f)));   // -0.3 -> 0
}

TEST_CASE("Multiply darkens, Screen lightens") {
    CHECK(approx(blendPixel(5, glm::vec3(0.5f), glm::vec3(0.5f)), glm::vec3(0.25f)));
    CHECK(approx(blendPixel(6, glm::vec3(0.5f), glm::vec3(0.5f)), glm::vec3(0.75f)));
}

TEST_CASE("Difference is symmetric; Darken/Lighten are min/max") {
    glm::vec3 a(0.2f,0.8f,0.5f), b(0.6f,0.1f,0.9f);
    CHECK(approx(blendPixel(3, a, b), blendPixel(3, b, a)));
    CHECK(approx(blendPixel(8, a, b), glm::vec3(0.2f,0.1f,0.5f)));
    CHECK(approx(blendPixel(9, a, b), glm::vec3(0.6f,0.8f,0.9f)));
}

TEST_CASE("Average is the midpoint") {
    CHECK(approx(blendPixel(15, glm::vec3(0.2f), glm::vec3(0.8f)), glm::vec3(0.5f)));
}

TEST_CASE("Divide and Color Dodge guard division by zero (finite, clamped)") {
    glm::vec3 r1 = blendPixel(14, glm::vec3(0.5f), glm::vec3(0.0f));   // divide by 0 -> 1
    glm::vec3 r2 = blendPixel(10, glm::vec3(0.5f), glm::vec3(1.0f));   // dodge b>=1 -> 1
    CHECK(std::isfinite(r1.x)); CHECK(r1.x == doctest::Approx(1.0f));
    CHECK(std::isfinite(r2.x)); CHECK(r2.x == doctest::Approx(1.0f));
}

TEST_CASE("bitwise AND/OR/XOR operate on 8-bit channels") {
    glm::vec3 a(240.0f/255.0f);   // 0xF0
    glm::vec3 b(15.0f/255.0f);    // 0x0F
    CHECK(approx(blendPixel(20, a, b), glm::vec3(0.0f)));            // AND -> 0x00
    CHECK(approx(blendPixel(21, a, b), glm::vec3(255.0f/255.0f)));  // OR  -> 0xFF
    CHECK(approx(blendPixel(22, a, b), glm::vec3(255.0f/255.0f)));  // XOR -> 0xFF
}

TEST_CASE("Color imposes the blend's chroma but keeps the base luminance") {
    glm::vec3 base(0.4f);                 // grey, lum 0.4
    glm::vec3 blend(0.9f, 0.2f, 0.1f);
    glm::vec3 r = blendPixel(18, base, blend);          // Color = setLum(blend, lum(base))
    CHECK(lumOf(r) == doctest::Approx(0.4f).epsilon(0.02));
}

TEST_CASE("Saturation keeps the base luminance (setSat path)") {
    glm::vec3 base(0.2f, 0.5f, 0.8f), blend(0.9f, 0.5f, 0.1f);
    glm::vec3 r = blendPixel(17, base, blend);
    CHECK(lumOf(r) == doctest::Approx(lumOf(base)).epsilon(0.02));
}

TEST_CASE("Color preserves base luma and clamps to [0,1] even when the result clips") {
    glm::vec3 base(0.5f);                 // grey, lum 0.5
    glm::vec3 blend(1.0f, 0.0f, 0.0f);    // pure red -> setLum overshoots, forcing clipColor's x>1 branch
    glm::vec3 r = blendPixel(18, base, blend);
    CHECK(lumOf(r) == doctest::Approx(0.5f).epsilon(0.02));
    for (int i = 0; i < 3; ++i) { CHECK(r[i] >= 0.0f); CHECK(r[i] <= 1.0f); }
}

TEST_CASE("Saturation with an achromatic base hits the all-equal setSat path safely") {
    glm::vec3 base(0.4f);                 // grey -> setSat input has all-equal channels (mdi=3 unused)
    glm::vec3 blend(0.9f, 0.3f, 0.1f);
    glm::vec3 r = blendPixel(17, base, blend);   // setSat(grey,*) -> {0,0,0}; setLum -> grey(lum(base))
    CHECK(std::isfinite(r.x));
    CHECK(r.x == doctest::Approx(0.4f).epsilon(0.02));
    CHECK(r.x == doctest::Approx(r.y));  CHECK(r.y == doctest::Approx(r.z));   // neutral grey
}
