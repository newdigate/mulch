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

TEST_CASE("retracted handles keep a segment exactly linear") {
    AutoCurve c;
    c.points = { {0.0f, 0.0f}, {2.0f, 1.0f} };
    CHECK(c.sample(1.0f) == doctest::Approx(0.5f));      // identical to the old linear curve
    CHECK(c.sample(0.5f) == doctest::Approx(0.25f));
}

TEST_CASE("an out-handle bends a segment into an ease") {
    AutoCurve c;
    AutoPoint a{0.0f, 0.0f}, b{4.0f, 1.0f};
    a.outDBar = 2.0f; a.outDValue = 0.0f;                // flat start handle -> slow start (ease-in)
    c.points = { a, b };
    float mid = c.sample(2.0f);
    CHECK(mid < 0.5f);                                   // ease-in dips below the linear midpoint
    CHECK(mid > 0.0f);
    CHECK(c.sample(0.0f) == doctest::Approx(0.0f));      // endpoints stay exact
    CHECK(c.sample(4.0f) == doctest::Approx(1.0f));
}

TEST_CASE("an extreme handle still samples monotonic, single-valued, no NaN") {
    AutoCurve c;
    AutoPoint a{0.0f, 0.0f}, b{2.0f, 1.0f};
    a.outDBar = 100.0f; a.outDValue = 1.0f;              // way past the next point; clamp keeps x monotonic
    c.points = { a, b };
    float prev = -1.0f;
    for (float bar = 0.0f; bar <= 2.0f + 1e-4f; bar += 0.25f) {
        float v = c.sample(bar);
        CHECK(v == v);                                   // not NaN
        CHECK(v >= prev - 1e-4f);                        // non-decreasing (this curve only rises)
        prev = v;
    }
    CHECK(c.sample(0.0f) == doctest::Approx(0.0f));
    CHECK(c.sample(2.0f) == doctest::Approx(1.0f));
}

TEST_CASE("curve codec round-trips handles and the broken flag") {
    AutoCurve c;
    AutoPoint a{0.0f, 0.2f}; a.outDBar = 0.5f; a.outDValue = 0.1f; a.broken = true;
    AutoPoint b{2.0f, 0.8f}; b.inDBar = -0.3f; b.inDValue = -0.05f;
    c.points = { a, b };
    AutoCurve r = decodeCurve(encodeCurve(c));
    REQUIRE(r.points.size() == 2);
    CHECK(r.points[0].outDBar   == doctest::Approx(0.5f));
    CHECK(r.points[0].outDValue == doctest::Approx(0.1f));
    CHECK(r.points[0].broken);
    CHECK(r.points[1].inDBar    == doctest::Approx(-0.3f));
    CHECK(r.points[1].inDValue  == doctest::Approx(-0.05f));
    CHECK_FALSE(r.points[1].broken);
}

TEST_CASE("a legacy bar,value curve still decodes with retracted handles") {
    AutoCurve r = decodeCurve("0.000000,0.250000,2.000000,0.750000");
    REQUIRE(r.points.size() == 2);
    CHECK(r.points[0].bar    == doctest::Approx(0.0f));
    CHECK(r.points[0].value  == doctest::Approx(0.25f));
    CHECK(r.points[1].value  == doctest::Approx(0.75f));
    CHECK(r.points[0].outDBar == doctest::Approx(0.0f));   // handles default to retracted
    CHECK_FALSE(r.points[0].broken);
}

TEST_CASE("the empty curve still round-trips to empty") {
    CHECK(encodeCurve(AutoCurve{}).empty());
    CHECK(decodeCurve(encodeCurve(AutoCurve{})).points.empty());
    CHECK(decodeCurve("").points.empty());
}
