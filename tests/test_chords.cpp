#include <doctest/doctest.h>
#include "core/Chords.h"
#include <vector>

using namespace oss;

TEST_CASE("buildChordNotes builds a C major triad at octave 4") {
    std::vector<int> n;
    buildChordNotes(0, 4, 0, n);              // C, oct 4, maj
    REQUIRE(n.size() == 3);
    CHECK(n[0] == 60); CHECK(n[1] == 64); CHECK(n[2] == 67);
}

TEST_CASE("buildChordNotes builds an A minor triad") {
    std::vector<int> n;
    buildChordNotes(9, 4, 1, n);              // A, oct 4, min
    REQUIRE(n.size() == 3);
    CHECK(n[0] == 69); CHECK(n[1] == 72); CHECK(n[2] == 76);
}

TEST_CASE("seventh chords have four notes") {
    std::vector<int> n;
    buildChordNotes(0, 4, 6, n);              // C maj7
    REQUIRE(n.size() == 4);
    CHECK(n[3] == 71);                        // 60 + 11
}

TEST_CASE("raising the octave by one adds 12 to every note") {
    std::vector<int> a, b;
    buildChordNotes(2, 3, 8, a);              // D dom7, oct 3
    buildChordNotes(2, 4, 8, b);              // D dom7, oct 4
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(b[i] == a[i] + 12);
}

TEST_CASE("notes above 127 are dropped but the root remains") {
    std::vector<int> n;
    buildChordNotes(11, 8, 13, n);            // B add9, oct 8: base 119, +{0,4,7,14}; 133 dropped
    REQUIRE(n.size() == 3);
    CHECK(n[0] == 119);
    for (int x : n) CHECK(x <= 127);
}

TEST_CASE("buildChordNotes appends without clearing") {
    std::vector<int> n = {1, 2};
    buildChordNotes(0, 4, 0, n);
    REQUIRE(n.size() == 5);
    CHECK(n[0] == 1); CHECK(n[2] == 60);
}

TEST_CASE("root and chord label counts match the spec") {
    CHECK(rootNoteLabels().size() == 12);
    CHECK(chordNames().size() == 14);
}

TEST_CASE("out-of-range root and chord indices are clamped to a valid chord") {
    std::vector<int> hi;
    buildChordNotes(99, 4, 99, hi);   // chord clamps to add9 (last); root clamps to B(11)
    REQUIRE(hi.size() == 4);          // add9 has 4 intervals
    CHECK(hi[0] == 71);               // B at oct 4 = (4+1)*12 + 11
    std::vector<int> lo;
    buildChordNotes(-1, 4, -1, lo);   // both clamp to 0 -> C maj
    REQUIRE(lo.size() == 3);
    CHECK(lo[0] == 60); CHECK(lo[1] == 64); CHECK(lo[2] == 67);
}
