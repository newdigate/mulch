#include <doctest/doctest.h>
#include "core/AudioPan.h"

using namespace oss;

TEST_CASE("panGains: centre keeps both, hard pan mutes the far side") {
    CHECK(panGains(0.0f).l == doctest::Approx(1.0f));
    CHECK(panGains(0.0f).r == doctest::Approx(1.0f));
    CHECK(panGains(-1.0f).l == doctest::Approx(1.0f));   // hard left
    CHECK(panGains(-1.0f).r == doctest::Approx(0.0f));
    CHECK(panGains(1.0f).l == doctest::Approx(0.0f));     // hard right
    CHECK(panGains(1.0f).r == doctest::Approx(1.0f));
}

TEST_CASE("downmixGains: centre averages, hard balance keeps one side") {
    CHECK(downmixGains(0.0f).l == doctest::Approx(0.5f));
    CHECK(downmixGains(0.0f).r == doctest::Approx(0.5f));
    CHECK(downmixGains(-1.0f).l == doctest::Approx(1.0f));  // all left
    CHECK(downmixGains(-1.0f).r == doctest::Approx(0.0f));
    CHECK(downmixGains(1.0f).l == doctest::Approx(0.0f));   // all right
    CHECK(downmixGains(1.0f).r == doctest::Approx(1.0f));
}
