#include <doctest/doctest.h>
#include <limits>
#include <vector>
#include "core/TimedAudio.h"

using namespace oss;

static const double kInfT = std::numeric_limits<double>::infinity();

// A chunk whose sample i holds the value i, starting at `start`.
static void addRamp(TimedAudio& a, double start, int n) {
    std::vector<float> s((std::size_t)n);
    for (int i = 0; i < n; ++i) s[(std::size_t)i] = (float)i;
    a.beginChunk(start);
    a.append(s.data(), s.size());
}

TEST_CASE("TimedAudio: sampling a span inside a chunk matches the old emitAudio mapping") {
    TimedAudio a(48000);
    addRamp(a, 1.0, 1000);
    std::vector<float> out(100);
    a.sample(1.0, 1.0 + 100.0 / 48000.0, out.data(), 100);      // one source sample per output sample
    for (int j = 0; j < 100; ++j) CHECK(out[(std::size_t)j] == doctest::Approx((float)j));
    a.sample(1.0, 1.0 + 1.0 / 48000.0, out.data(), 2);           // half-sample steps interpolate
    CHECK(out[1] == doctest::Approx(0.5f));
}

TEST_CASE("TimedAudio: a reversed span reads backwards") {
    TimedAudio a(48000);
    addRamp(a, 0.0, 1000);
    std::vector<float> out(10);
    a.sample(500.0 / 48000.0, 490.0 / 48000.0, out.data(), 10);
    for (int j = 0; j < 10; ++j) CHECK(out[(std::size_t)j] == doctest::Approx((float)(500 - j)));
}

TEST_CASE("TimedAudio: times no chunk covers are silence") {
    TimedAudio a(48000);
    addRamp(a, 1.0, 480);                                         // [1.0, 1.01)
    float v = -1.0f;
    a.sample(0.5, 0.5, &v, 1);    CHECK(v == 0.0f);                // before
    a.sample(1.02, 1.02, &v, 1);  CHECK(v == 0.0f);                // after
    TimedAudio empty;
    empty.sample(0.0, 1.0, &v, 1); CHECK(v == 0.0f);
}

TEST_CASE("TimedAudio: a chunk covers its last interval, so adjacent chunks tile (a loop seam)") {
    TimedAudio a(1000);
    std::vector<float> ones(1000, 1.0f), twos(1000, 2.0f);
    a.beginChunk(0.0); a.append(ones.data(), ones.size());       // lap 0's audio ends exactly at D = 1
    a.beginChunk(1.0); a.append(twos.data(), twos.size());       // lap 1 starts at D
    std::vector<float> out(40);
    a.sample(0.98, 1.02, out.data(), 40);                         // one output sample per source sample
    for (int j = 0; j < 40; ++j) CHECK(out[(std::size_t)j] == doctest::Approx(j < 20 ? 1.0f : 2.0f));
    float v = 0.0f;
    a.sample(0.9995, 0.9995, &v, 1); CHECK(v == doctest::Approx(1.0f));   // the last interval holds
}

TEST_CASE("TimedAudio: where chunks overlap, the most recently begun wins") {
    TimedAudio a(1000);
    std::vector<float> ones(2000, 1.0f), twos(2000, 2.0f);
    a.beginChunk(0.0); a.append(ones.data(), ones.size());       // [0, 2)
    a.beginChunk(1.5); a.append(twos.data(), twos.size());       // [1.5, 3.5): a loop seam, the new lap
    float v = 0.0f;
    a.sample(1.0, 1.0, &v, 1); CHECK(v == doctest::Approx(1.0f));
    a.sample(1.6, 1.6, &v, 1); CHECK(v == doctest::Approx(2.0f));
    a.sample(3.0, 3.0, &v, 1); CHECK(v == doctest::Approx(2.0f));
    // Reverse: the stretch above was decoded first, and its audio resumes a little before its keyframe
    // (1.0) out of a just-flushed decoder. The stretch below, decoded next and clipped at that keyframe,
    // runs on unbroken there -- and wins, though it starts earlier.
    TimedAudio r(1000);
    std::vector<float> above(2000, 2.0f), below(3000, 1.0f);
    r.beginChunk(0.9);  r.append(above.data(), above.size());
    r.beginChunk(-1.0); r.append(below.data(), below.size(), 1.0);
    r.sample(0.95, 0.95, &v, 1); CHECK(v == doctest::Approx(1.0f));
    r.sample(1.50, 1.50, &v, 1); CHECK(v == doctest::Approx(2.0f));
}

TEST_CASE("TimedAudio: append keeps the samples that start before clipHi") {
    TimedAudio a(1000);
    std::vector<float> s(100, 1.0f);
    a.beginChunk(0.0);
    a.append(s.data(), s.size(), 0.0105);                         // samples at 0..10 ms start before 10.5 ms
    CHECK(a.size() == 11);
    a.append(s.data(), s.size(), 0.0105);                         // the next would start at 11 ms: none
    CHECK(a.size() == 11);
    TimedAudio g(48000);                                          // a clip on the sample grid, computed
    std::vector<float> big(120000, 1.0f);                         // (2.19 - 0.01) * 48000 = 104640.000...015:
    g.beginChunk(0.01);                                           // the sample starting AT 2.19 stays out
    g.append(big.data(), big.size(), 2.19);
    CHECK(g.size() == 104640);
    TimedAudio none;
    none.append(s.data(), s.size());                              // no chunk begun: no-op
    CHECK(none.size() == 0);
}

TEST_CASE("TimedAudio: retain drops chunks outside the window and trims fronts once a second is stale") {
    TimedAudio a(1000);
    std::vector<float> s(5000, 1.0f);
    a.beginChunk(0.0);  a.append(s.data(), s.size());            // [0, 5)
    a.beginChunk(10.0); a.append(s.data(), s.size());            // [10, 15): current
    a.retain(6.0, kInfT, 8.0);                                    // the first chunk ends before 6
    CHECK(a.size() == 5000);
    a.retain(10.5, kInfT, 12.5);                                  // under a second into the chunk: kept
    CHECK(a.size() == 5000);
    a.retain(12.5, kInfT, 14.5);                                  // 2.5 s in: trimmed
    CHECK(a.size() == 2500);
    float v = 0.0f;
    a.sample(13.0, 13.0, &v, 1); CHECK(v == doctest::Approx(1.0f));
}

TEST_CASE("TimedAudio: in reverse, retain trims what the playhead has passed from the backs of chunks") {
    TimedAudio a(1000);
    std::vector<float> s(10000, 1.0f);
    a.beginChunk(10.0); a.append(s.data(), s.size());            // [10, 20): the stretch being played
    a.beginChunk(0.0);  a.append(s.data(), s.size(), 10.0);      // [0, 10): the stretch below, current
    a.retain(-kInfT, 12.0 + 0.5, 12.0);                           // the playhead at 12, keeping 0.5 s above
    CHECK(a.size() == 10000 + 2500);                              // [10, 12.5) and [0, 10)
    float v = 0.0f;
    a.sample(12.4, 12.4, &v, 1); CHECK(v == doctest::Approx(1.0f));
    a.sample(12.6, 12.6, &v, 1); CHECK(v == 0.0f);
    a.retain(-kInfT, 12.0, 11.5);                                 // under a second more: left alone
    CHECK(a.size() == 10000 + 2500);
}

TEST_CASE("TimedAudio: the cap drops the audio farthest from the playhead, never what plays there") {
    const int rate = 100;
    const double cap = TimedAudio::kMaxSeconds;
    float v = 0.0f;
    auto chunk = [&](TimedAudio& a, double start, double seconds, float value, double clipHi = kInfT) {
        std::vector<float> s((std::size_t)(seconds * rate), value);
        a.beginChunk(start);
        a.append(s.data(), s.size(), clipHi);
    };
    {                                                             // before any retain(): the oldest goes
        TimedAudio a(rate);
        chunk(a, 0.0, 0.5 * cap + 1.0, 1.0f);
        chunk(a, 0.5 * cap + 1.0, 0.5 * cap + 1.0, 2.0f);
        CHECK(a.size() == (std::size_t)((0.5 * cap + 1.0) * rate));
        a.sample(1.0, 1.0, &v, 1); CHECK(v == 0.0f);
        a.sample(cap, cap, &v, 1); CHECK(v == doctest::Approx(2.0f));
    }
    {                                                             // reverse: the stretch playing is the oldest
        TimedAudio a(rate);
        const double third = cap / 3.0 + 1.0;
        chunk(a, 2.0 * third, third, 3.0f);                       // the stretch playing
        a.retain(-kInfT, 2.5 * third + 2.0, 2.5 * third);
        chunk(a, third, third, 2.0f);                             // the stretch below
        chunk(a, 0.0, third, 1.0f);                               // and the one below that: over the cap
        CHECK(a.size() == (std::size_t)(2.0 * third * rate));
        a.sample(2.5 * third, 2.5 * third, &v, 1); CHECK(v == doctest::Approx(3.0f));   // still playing
        a.sample(1.5 * third, 1.5 * third, &v, 1); CHECK(v == 0.0f);                    // the farthest went
        a.sample(0.5 * third, 0.5 * third, &v, 1); CHECK(v == doctest::Approx(1.0f));   // the one being filled stays
    }
    {                                                             // measured from the playhead, not the window
        TimedAudio a(rate);                                       // (keyframes 89.5 s apart): the stretch playing
        chunk(a, 99.9, 90.0, 3.0f);                               // reaches down to the playhead's lap...
        chunk(a, 10.4, 90.0, 2.0f, 100.0);                        // ...where the one below now plays
        a.retain(-kInfT, 99.5 + 2.0, 99.5);
        chunk(a, -79.1, 90.0, 1.0f, 10.4);                        // over the cap
        a.sample(99.5, 99.5, &v, 1); CHECK(v == doctest::Approx(2.0f));
        CHECK(a.size() <= (std::size_t)(cap * rate));
    }
    {                                                             // only what plays and what is being filled
        TimedAudio a(rate);                                       // are left: the latter's far end goes
        chunk(a, 0.0, 95.0, 2.0f);                                // the fresh stretch, played at 94
        a.retain(-kInfT, 94.0 + 2.0, 94.0);
        chunk(a, -95.0, 95.0, 1.0f, 0.0);                         // the lap before, over the cap by 10 s
        CHECK(a.size() == (std::size_t)(cap * rate));
        a.sample(94.0, 94.0, &v, 1); CHECK(v == doctest::Approx(2.0f));
        a.sample(-90.0, -90.0, &v, 1); CHECK(v == 0.0f);          // the bottom 10 s went
        a.sample(-80.0, -80.0, &v, 1); CHECK(v == doctest::Approx(1.0f));
    }
    {                                                             // one stretch alone over the cap keeps the
        TimedAudio a(rate);                                       // cap's worth nearest the playhead
        a.retain(-kInfT, 600.0 + 2.0, 600.0);
        chunk(a, 0.0, 600.0, 1.0f);                               // a keyframe 10 minutes below
        CHECK(a.size() == (std::size_t)(cap * rate));
        a.sample(599.0, 599.0, &v, 1); CHECK(v == doctest::Approx(1.0f));
        a.sample(600.0 - cap - 1.0, 600.0 - cap - 1.0, &v, 1); CHECK(v == 0.0f);
    }
}
