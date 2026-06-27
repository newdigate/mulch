#include <doctest/doctest.h>
#include "core/DrumPattern.h"
#include <string>

using namespace oss;

TEST_CASE("DrumPatterns cells default off and cycle off->on->accent->off") {
    DrumPatterns p;
    CHECK(p.cell(0, 0) == 0);
    p.cycleCell(0, 0); CHECK(p.cell(0, 0) == 1);   // on
    p.cycleCell(0, 0); CHECK(p.cell(0, 0) == 2);   // accent
    p.cycleCell(0, 0); CHECK(p.cell(0, 0) == 0);   // back to off
    // out-of-range is a no-op (no crash)
    p.cycleCell(-1, 0); p.cycleCell(0, 99);
    CHECK(p.cell(0, 0) == 0);
}

TEST_CASE("DrumPatterns active slot selects an independent grid") {
    DrumPatterns p;
    p.setActive(0); p.cycleCell(1, 2);            // slot 0, row 1, col 2 -> on
    p.setActive(3); CHECK(p.cell(1, 2) == 0);     // slot 3 is empty
    p.cycleCell(1, 2); p.cycleCell(1, 2);         // slot 3 -> accent
    CHECK(p.cell(3, 1, 2) == 2);
    CHECK(p.cell(0, 1, 2) == 1);                  // slot 0 unchanged
    p.setActive(99); CHECK(p.active() == DrumPatterns::kSlots - 1);   // clamped
}

TEST_CASE("DrumPatterns encode/decode round-trips all slots + active index") {
    DrumPatterns a;
    a.setActive(5);
    a.setCell(0, 0, 0, 1); a.setCell(0, 1, 4, 2);
    a.setCell(7, 3, 15, 2);
    std::string s = a.encode();
    DrumPatterns b; b.decode(s);
    CHECK(b.active() == 5);
    CHECK(b.cell(0, 0, 0) == 1);
    CHECK(b.cell(0, 1, 4) == 2);
    CHECK(b.cell(7, 3, 15) == 2);
    CHECK(b.cell(2, 2, 2) == 0);
    CHECK(b.encode() == s);
}

TEST_CASE("DrumPatterns decode tolerates empty + malformed input") {
    DrumPatterns p; p.setCell(0, 0, 0, 2);
    p.decode("");                       // empty: no change, no throw
    CHECK(p.cell(0, 0, 0) == 2);
    DrumPatterns q;
    q.decode("notanumber;012");         // bad active -> 0; short grid -> rest off
    CHECK(q.active() == 0);
    CHECK(q.cell(0, 0, 0) == 0);
    CHECK(q.cell(0, 0, 1) == 1);
    CHECK(q.cell(0, 0, 2) == 2);
}
