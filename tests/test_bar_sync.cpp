#include <doctest/doctest.h>
#include "audio/BarSync.h"

using namespace oss;

TEST_CASE("barSyncPlayhead: aligns to bar 1 and repeats every length bars") {
    double dur = 8.0;
    CHECK(barSyncPlayhead(0.0, 4, dur) == doctest::Approx(0.0));       // start of window
    CHECK(barSyncPlayhead(4.0, 4, dur) == doctest::Approx(0.0));       // exactly one window -> wrapped to 0
    CHECK(barSyncPlayhead(2.0, 4, dur) == doctest::Approx(4.0));       // halfway -> half the clip
}

TEST_CASE("barSyncPlayhead: linear stretch so a full pass spans length bars") {
    double dur = 8.0;
    // length = 4: slope is dur/4 = 2.0 s per bar within the window
    CHECK(barSyncPlayhead(1.0, 4, dur) == doctest::Approx(2.0));       // dur/4
    CHECK(barSyncPlayhead(3.0, 4, dur) == doctest::Approx(6.0));       // 3*dur/4
    // length = 1: the whole clip fits in one bar
    CHECK(barSyncPlayhead(0.25, 1, dur) == doctest::Approx(2.0));      // dur/4
    CHECK(barSyncPlayhead(0.5,  1, dur) == doctest::Approx(4.0));      // dur/2
}

TEST_CASE("barSyncPlayhead: repeats across windows") {
    double dur = 8.0;
    CHECK(barSyncPlayhead(5.0, 4, dur) == doctest::Approx(barSyncPlayhead(1.0, 4, dur)));
    CHECK(barSyncPlayhead(9.0, 4, dur) == doctest::Approx(barSyncPlayhead(1.0, 4, dur)));
}

TEST_CASE("barSyncPlayhead: guards non-positive length and duration") {
    CHECK(barSyncPlayhead(2.5, 0,  8.0) == doctest::Approx(0.0));      // length < 1
    CHECK(barSyncPlayhead(2.5, -3, 8.0) == doctest::Approx(0.0));      // length < 1
    CHECK(barSyncPlayhead(2.5, 4,  0.0) == doctest::Approx(0.0));      // duration <= 0
    CHECK(barSyncPlayhead(2.5, 4, -1.0) == doctest::Approx(0.0));      // duration <= 0
}
