#include <doctest/doctest.h>
#include "core/PitchGraph.h"
#include "core/Value.h"
#include <glm/vec3.hpp>
#include <vector>
#include <cmath>

using namespace oss;

static bool approx(const glm::vec3& a, const glm::vec3& b, float e = 1e-3f) {
    return std::fabs(a.x-b.x) < e && std::fabs(a.y-b.y) < e && std::fabs(a.z-b.z) < e;
}

TEST_CASE("hsvToRgb maps the primary hues and wraps") {
    CHECK(approx(hsvToRgb(0.0f, 1, 1),       glm::vec3(1, 0, 0)));   // red
    CHECK(approx(hsvToRgb(1.0f/3.0f, 1, 1),  glm::vec3(0, 1, 0)));   // green
    CHECK(approx(hsvToRgb(2.0f/3.0f, 1, 1),  glm::vec3(0, 0, 1)));   // blue
    CHECK(approx(hsvToRgb(1.0f/6.0f, 1, 1),  glm::vec3(1, 1, 0)));   // yellow  (sector 1)
    CHECK(approx(hsvToRgb(0.5f, 1, 1),       glm::vec3(0, 1, 1)));   // cyan    (sector 3)
    CHECK(approx(hsvToRgb(5.0f/6.0f, 1, 1),  glm::vec3(1, 0, 1)));   // magenta (sector 5)
    CHECK(approx(hsvToRgb(1.0f, 1, 1),       hsvToRgb(0.0f, 1, 1))); // hue wraps
}

TEST_CASE("a note-on then note-off makes one coloured segment at the right pitch") {
    PitchGraph pg;
    std::vector<MidiEvent> on = { midiNoteOn(60, 100) };
    pg.ingest(MidiRef{on.data(), on.size()}, 0.0f, 8.0f);   // note-on at t=0
    std::vector<MidiEvent> off = { midiNoteOff(60) };
    pg.ingest(MidiRef{off.data(), off.size()}, 1.0f, 8.0f); // note-off at t=1
    std::vector<float> v;
    int n = pg.build(8.0f, v);
    REQUIRE(n == 2);              // one segment = 2 vertices
    REQUIRE(v.size() == 12);     // 2 * 6 floats
    CHECK(v[1] == doctest::Approx(0.0f));   // y of note 60 = range centre
    CHECK(v[7] == doctest::Approx(0.0f));
    float val = 0.25f + 0.75f * 100.0f / 127.0f;
    CHECK(v[3] == doctest::Approx(val));    // r (pitch class 0 -> hue 0 -> red = value)
    CHECK(v[4] == doctest::Approx(0.0f));   // g
    CHECK(v[5] == doctest::Approx(0.0f));   // b
}

TEST_CASE("the same pitch class shares a hue across octaves; other notes differ") {
    PitchGraph pg;
    std::vector<MidiEvent> on = { midiNoteOn(60, 100), midiNoteOn(72, 100), midiNoteOn(61, 100) };
    pg.ingest(MidiRef{on.data(), on.size()}, 0.0f, 8.0f);
    std::vector<float> v; pg.build(8.0f, v);
    REQUIRE(v.size() == 3 * 2 * 6);                 // 3 held notes
    glm::vec3 c60(v[3],  v[4],  v[5]);              // note 60, first vertex colour
    glm::vec3 c72(v[15], v[16], v[17]);             // note 72
    glm::vec3 c61(v[27], v[28], v[29]);             // note 61
    CHECK(approx(c60, c72));                         // pitch class 0 == pitch class 0
    CHECK_FALSE(approx(c60, c61));                   // pitch class 0 != pitch class 1
}

TEST_CASE("a held note's right end stays pinned at the right edge") {
    PitchGraph pg;
    std::vector<MidiEvent> on = { midiNoteOn(60, 100) };
    pg.ingest(MidiRef{on.data(), on.size()}, 0.0f, 8.0f);
    std::vector<MidiEvent> none;
    pg.ingest(MidiRef{none.data(), none.size()}, 2.0f, 8.0f);   // 2s later, still held
    std::vector<float> v; pg.build(8.0f, v);
    REQUIRE(v.size() == 12);
    CHECK(v[6] == doctest::Approx(1.0f));   // second vertex x (xe) pinned at +1
    CHECK(v[0] == doctest::Approx(0.5f));   // first vertex x (xs) scrolled left: 2*(-2)/8+1
}

TEST_CASE("a closed note scrolled past the window is pruned") {
    PitchGraph pg;
    std::vector<MidiEvent> on = { midiNoteOn(60, 100) };
    pg.ingest(MidiRef{on.data(), on.size()}, 0.0f, 4.0f);
    std::vector<MidiEvent> off = { midiNoteOff(60) };
    pg.ingest(MidiRef{off.data(), off.size()}, 0.5f, 4.0f);     // closed at t=0.5
    std::vector<MidiEvent> none;
    pg.ingest(MidiRef{none.data(), none.size()}, 5.0f, 4.0f);   // t=5.5, cutoff=1.5 > 0.5
    std::vector<float> v; int n = pg.build(4.0f, v);
    CHECK(n == 0);
    CHECK(pg.activeCount() == 0);
}

TEST_CASE("activeCount counts only held notes") {
    PitchGraph pg;
    std::vector<MidiEvent> on = { midiNoteOn(60, 100), midiNoteOn(64, 100) };
    pg.ingest(MidiRef{on.data(), on.size()}, 0.0f, 8.0f);
    CHECK(pg.activeCount() == 2);
    std::vector<MidiEvent> off = { midiNoteOff(60) };
    pg.ingest(MidiRef{off.data(), off.size()}, 0.1f, 8.0f);
    CHECK(pg.activeCount() == 1);
}

TEST_CASE("retriggering a held note closes the prior segment and opens a new one") {
    PitchGraph pg;
    std::vector<MidiEvent> on1 = { midiNoteOn(60, 100) };
    pg.ingest(MidiRef{on1.data(), on1.size()}, 0.0f, 8.0f);   // on at t=0
    std::vector<MidiEvent> on2 = { midiNoteOn(60, 100) };
    pg.ingest(MidiRef{on2.data(), on2.size()}, 1.0f, 8.0f);   // retrigger at t=1: close first, open second
    CHECK(pg.activeCount() == 1);                              // only the second is held
    std::vector<MidiEvent> off = { midiNoteOff(60) };
    pg.ingest(MidiRef{off.data(), off.size()}, 1.0f, 8.0f);   // off at t=2: close second
    CHECK(pg.activeCount() == 0);
    std::vector<float> v; int n = pg.build(8.0f, v);
    CHECK(n == 4);                                             // two segments = 4 vertices
}

TEST_CASE("a note below the pitch range clamps to the bottom edge") {
    PitchGraph pg;
    std::vector<MidiEvent> on = { midiNoteOn(0, 100) };        // pitch 0, far below kLoNote 24
    pg.ingest(MidiRef{on.data(), on.size()}, 0.0f, 8.0f);
    std::vector<float> v; pg.build(8.0f, v);
    REQUIRE(v.size() == 12);
    CHECK(v[1] == doctest::Approx(-1.0f));                     // y clamped to the bottom
}

TEST_CASE("the note history stays bounded under a flood of note-ons") {
    PitchGraph pg;
    std::vector<MidiEvent> many;
    for (int i = 0; i < 600; ++i) many.push_back(midiNoteOn(20 + (i % 80), 100));
    pg.ingest(MidiRef{many.data(), many.size()}, 0.0f, 8.0f);  // no crash, capped
    CHECK(pg.activeCount() <= 512);
    std::vector<float> v; int n = pg.build(8.0f, v);
    CHECK(n <= 512 * 2);                                        // bounded vertex count
}
