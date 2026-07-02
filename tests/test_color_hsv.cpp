#include <doctest/doctest.h>
#include "core/ColorHsv.h"
#include <initializer_list>

using namespace oss;

TEST_CASE("hsvToRgb hits the primary colours") {
    glm::vec3 r = hsvToRgb(0.0f, 1.0f, 1.0f);
    CHECK(r.x == doctest::Approx(1.0f)); CHECK(r.y == doctest::Approx(0.0f)); CHECK(r.z == doctest::Approx(0.0f));
    glm::vec3 g = hsvToRgb(1.0f/3.0f, 1.0f, 1.0f);
    CHECK(g.x == doctest::Approx(0.0f)); CHECK(g.y == doctest::Approx(1.0f)); CHECK(g.z == doctest::Approx(0.0f));
    glm::vec3 b = hsvToRgb(2.0f/3.0f, 1.0f, 1.0f);
    CHECK(b.x == doctest::Approx(0.0f)); CHECK(b.y == doctest::Approx(0.0f)); CHECK(b.z == doctest::Approx(1.0f));
}

TEST_CASE("rgbToHsv inverts hsvToRgb") {
    glm::vec3 hsv = rgbToHsv(1.0f, 0.0f, 0.0f);
    CHECK(hsv.x == doctest::Approx(0.0f));
    CHECK(hsv.y == doctest::Approx(1.0f));
    CHECK(hsv.z == doctest::Approx(1.0f));
    for (glm::vec3 c : { glm::vec3(0.8f,0.2f,0.4f), glm::vec3(0.1f,0.6f,0.9f), glm::vec3(0.5f,0.5f,0.2f) }) {
        glm::vec3 h  = rgbToHsv(c.x, c.y, c.z);
        glm::vec3 rt = hsvToRgb(h.x, h.y, h.z);
        CHECK(rt.x == doctest::Approx(c.x)); CHECK(rt.y == doctest::Approx(c.y)); CHECK(rt.z == doctest::Approx(c.z));
    }
}

TEST_CASE("rgbToHsv: grey has zero saturation") {
    glm::vec3 hsv = rgbToHsv(0.5f, 0.5f, 0.5f);
    CHECK(hsv.y == doctest::Approx(0.0f));
    CHECK(hsv.z == doctest::Approx(0.5f));
}
