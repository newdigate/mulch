#include <doctest/doctest.h>
#include "core/MidiTimecode.h"
#include <vector>
#include <initializer_list>

using namespace oss;

TEST_CASE("SMPTE <-> seconds for the integer frame rates") {
    CHECK(smpteToSeconds({1, 0, 0, 0, MtcRate::Fps30}) == doctest::Approx(3600.0));
    CHECK(smpteToSeconds({0, 0, 1, 15, MtcRate::Fps30}) == doctest::Approx(1.5));
    CHECK(smpteToSeconds({0, 1, 0, 0, MtcRate::Fps25}) == doctest::Approx(60.0));
    CHECK(smpteToSeconds({0, 0, 10, 0, MtcRate::Fps24}) == doctest::Approx(10.0));
    for (MtcRate r : {MtcRate::Fps24, MtcRate::Fps25, MtcRate::Fps30}) {
        SmpteTime a{0, 12, 34, 5, r};
        SmpteTime b = secondsToSmpte(smpteToSeconds(a), r);
        CHECK(b.h == a.h); CHECK(b.m == a.m); CHECK(b.s == a.s); CHECK(b.f == a.f);
    }
}

TEST_CASE("29.97 drop-frame conversion + round-trip") {
    CHECK(smpteToSeconds({1, 0, 0, 0, MtcRate::Fps2997df}) == doctest::Approx(3599.9964));
    CHECK(smpteToSeconds({0, 1, 0, 2, MtcRate::Fps2997df}) == doctest::Approx(60.06));
    for (SmpteTime a : { SmpteTime{0, 1, 0, 2, MtcRate::Fps2997df},
                         SmpteTime{0, 10, 0, 0, MtcRate::Fps2997df},
                         SmpteTime{1, 0, 0, 0, MtcRate::Fps2997df} }) {
        SmpteTime b = secondsToSmpte(smpteToSeconds(a), MtcRate::Fps2997df);
        CHECK(b.h == a.h); CHECK(b.m == a.m); CHECK(b.s == a.s); CHECK(b.f == a.f);
    }
}

TEST_CASE("quarter-frame bytes carry the right piece + nibble") {
    SmpteTime tc{0, 0, 1, 0, MtcRate::Fps30};
    CHECK(quarterFrameByte(0, tc) == 0x00);
    CHECK(quarterFrameByte(2, tc) == 0x21);
    CHECK(quarterFrameByte(7, tc) == 0x76);
}

TEST_CASE("full-frame SysEx round-trips and rejects non-MTC") {
    SmpteTime tc{2, 13, 47, 9, MtcRate::Fps25};
    std::vector<unsigned char> m;
    fullFrameMessage(tc, m);
    REQUIRE(m.size() == 10);
    CHECK(m.front() == 0xF0); CHECK(m.back() == 0xF7);
    SmpteTime got;
    REQUIRE(parseFullFrame(m.data(), m.size(), got));
    CHECK(got.h == 2); CHECK(got.m == 13); CHECK(got.s == 47); CHECK(got.f == 9);
    CHECK(got.rate == MtcRate::Fps25);
    std::vector<unsigned char> bad = {0xF0, 0x7E, 0x7F, 0x01, 0x01, 0, 0, 0, 0, 0xF7};
    CHECK_FALSE(parseFullFrame(bad.data(), bad.size(), got));
}

TEST_CASE("MtcReader reassembles quarter-frames + handles full-frame + idle") {
    SmpteTime tc{0, 0, 1, 0, MtcRate::Fps30};
    MtcReader r;
    for (int p = 0; p < 8; ++p) r.onQuarterFrame(quarterFrameByte(p, tc));
    CHECK(r.playing());
    CHECK(r.rate() == MtcRate::Fps30);
    CHECK(r.seconds() == doctest::Approx(smpteToSeconds(tc) + 2.0 * frameDuration(MtcRate::Fps30)));
    r.onFullFrame({0, 5, 0, 0, MtcRate::Fps30});
    CHECK(r.seconds() == doctest::Approx(300.0));
    r.onIdle(0.2);
    CHECK_FALSE(r.playing());
}
