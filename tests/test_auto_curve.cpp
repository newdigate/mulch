#include <doctest/doctest.h>
#include "core/AutoCurve.h"

using namespace oss;

TEST_CASE("an empty curve samples to zero") {
    AutoCurve c;
    CHECK(c.sample(0.0f) == doctest::Approx(0.0f));
    CHECK(c.sample(3.5f) == doctest::Approx(0.0f));
}

TEST_CASE("a curve interpolates and holds at the ends") {
    AutoCurve c;
    c.points = { {0.0f, 0.0f}, {2.0f, 1.0f}, {4.0f, 0.0f} };   // a triangle
    CHECK(c.sample(0.0f) == doctest::Approx(0.0f));
    CHECK(c.sample(1.0f) == doctest::Approx(0.5f));    // halfway up
    CHECK(c.sample(2.0f) == doctest::Approx(1.0f));    // peak
    CHECK(c.sample(3.0f) == doctest::Approx(0.5f));    // halfway down
    CHECK(c.sample(4.0f) == doctest::Approx(0.0f));
    CHECK(c.sample(5.0f) == doctest::Approx(0.0f));    // past the end -> hold last
    CHECK(c.sample(-1.0f) == doctest::Approx(0.0f));   // before start -> hold first
}
