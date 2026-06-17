#include <doctest/doctest.h>
#include "core/StepSync.h"

using namespace oss;

TEST_CASE("step division table has 8 entries with a 1/16 default") {
    CHECK(stepDivisionLabels().size() == 8);
    CHECK(kStepDivisionDefault == 4);
    CHECK(stepDivisionLabels()[kStepDivisionDefault] == "1/16");
}

TEST_CASE("step division bars are the right musical lengths (4/4)") {
    CHECK(stepDivisionBars(0) == doctest::Approx(0.25));        // 1/4 (a beat)
    CHECK(stepDivisionBars(1) == doctest::Approx(0.125));       // 1/8
    CHECK(stepDivisionBars(2) == doctest::Approx(0.1875));      // 1/8. dotted
    CHECK(stepDivisionBars(3) == doctest::Approx(0.0833333));   // 1/8T triplet
    CHECK(stepDivisionBars(4) == doctest::Approx(0.0625));      // 1/16
    CHECK(stepDivisionBars(5) == doctest::Approx(0.09375));     // 1/16. dotted
    CHECK(stepDivisionBars(6) == doctest::Approx(0.0416667));   // 1/16T triplet
    CHECK(stepDivisionBars(7) == doctest::Approx(0.03125));     // 1/32
}

TEST_CASE("step division index is clamped to a valid range") {
    CHECK(stepDivisionBars(-5) == doctest::Approx(0.25));       // clamps to 0
    CHECK(stepDivisionBars(99) == doctest::Approx(0.03125));    // clamps to 7
}
