#include <doctest/doctest.h>
#include "core/CubeMap.h"
#include <glm/vec3.hpp>
#include <glm/geometric.hpp>
#include <cmath>
#include <initializer_list>

using namespace oss;

TEST_CASE("cubeFaceUV selects the right face for each axis, centred at u=v=0.5") {
    CHECK(cubeFaceUV(glm::vec3( 1, 0, 0)).face == 0);   // +X
    CHECK(cubeFaceUV(glm::vec3(-1, 0, 0)).face == 1);   // -X
    CHECK(cubeFaceUV(glm::vec3( 0, 1, 0)).face == 2);   // +Y
    CHECK(cubeFaceUV(glm::vec3( 0,-1, 0)).face == 3);   // -Y
    CHECK(cubeFaceUV(glm::vec3( 0, 0, 1)).face == 4);   // +Z
    CHECK(cubeFaceUV(glm::vec3( 0, 0,-1)).face == 5);   // -Z
    for (glm::vec3 d : { glm::vec3(1,0,0), glm::vec3(0,1,0), glm::vec3(0,0,-1) }) {
        CubeSample c = cubeFaceUV(d);
        CHECK(c.u == doctest::Approx(0.5f));
        CHECK(c.v == doctest::Approx(0.5f));
    }
}

TEST_CASE("cubeFaceUV maps an off-axis direction to the right UV") {
    CubeSample c = cubeFaceUV(glm::vec3(1.0f, 0.5f, 0.0f));   // +X dominant
    CHECK(c.face == 0);
    CHECK(c.u == doctest::Approx(0.5f));    // sc = -d.z = 0 -> 0.5
    CHECK(c.v == doctest::Approx(0.25f));   // tc = -d.y = -0.5, /ma=1 -> (-0.5+1)/2
}

TEST_CASE("cubeFaceUV maps off-axis UV on the +Y and -Z faces") {
    CubeSample y = cubeFaceUV(glm::vec3(0.3f, 1.0f, 0.7f));    // +Y dominant
    CHECK(y.face == 2);
    CHECK(y.u == doctest::Approx(0.65f));   // sc=d.x=0.3 -> 0.3*0.5+0.5
    CHECK(y.v == doctest::Approx(0.85f));   // tc=d.z=0.7 -> 0.7*0.5+0.5
    CubeSample z = cubeFaceUV(glm::vec3(0.4f, -0.2f, -1.0f));  // -Z dominant
    CHECK(z.face == 5);
    CHECK(z.u == doctest::Approx(0.3f));    // sc=-d.x=-0.4 -> -0.4*0.5+0.5
    CHECK(z.v == doctest::Approx(0.6f));    // tc=-d.y=0.2  ->  0.2*0.5+0.5
}

TEST_CASE("cubeFaceUV keeps u,v in [0,1], including normalized directions") {
    for (glm::vec3 d : { glm::vec3(0.7f,0.2f,-0.6f), glm::vec3(-0.3f,0.9f,0.2f),
                         glm::vec3(0.1f,-0.2f,0.95f), glm::vec3(-0.8f,-0.5f,-0.1f),
                         glm::normalize(glm::vec3(0.7f,0.2f,-0.6f)) }) {
        CubeSample c = cubeFaceUV(d);
        CHECK(c.u >= 0.0f); CHECK(c.u <= 1.0f);
        CHECK(c.v >= 0.0f); CHECK(c.v <= 1.0f);
    }
}

TEST_CASE("cubeFaceUV handles a degenerate zero direction without NaN") {
    CubeSample c = cubeFaceUV(glm::vec3(0.0f));   // guard: ma==0 -> u=v=0.5, finite
    CHECK(std::isfinite(c.u)); CHECK(std::isfinite(c.v));
    CHECK(c.u == doctest::Approx(0.5f)); CHECK(c.v == doctest::Approx(0.5f));
}
