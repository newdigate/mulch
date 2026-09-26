#include <doctest/doctest.h>
#include <vector>
#include "core/TimedAudio.h"

using namespace oss;

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

TEST_CASE("TimedAudio: where chunks overlap, the later-starting chunk wins (a loop seam)") {
    TimedAudio a(1000);
    std::vector<float> ones(2000, 1.0f), twos(2000, 2.0f);
    a.beginChunk(0.0); a.append(ones.data(), ones.size());       // [0, 2)
    a.beginChunk(1.5); a.append(twos.data(), twos.size());       // [1.5, 3.5)
    float v = 0.0f;
    a.sample(1.0, 1.0, &v, 1); CHECK(v == doctest::Approx(1.0f));
    a.sample(1.6, 1.6, &v, 1); CHECK(v == doctest::Approx(2.0f));
    a.sample(3.0, 3.0, &v, 1); CHECK(v == doctest::Approx(2.0f));
}

TEST_CASE("TimedAudio: append keeps only samples before clipHi") {
    TimedAudio a(1000);
    std::vector<float> s(100, 1.0f);
    a.beginChunk(0.0);
    a.append(s.data(), s.size(), 0.010);                          // 10 samples fit before 10 ms
    CHECK(a.size() == 10);
    a.append(s.data(), s.size(), 0.010);                          // already at the clip: nothing more
    CHECK(a.size() == 10);
    TimedAudio none;
    none.append(s.data(), s.size());                              // no chunk begun: no-op
    CHECK(none.size() == 0);
}

TEST_CASE("TimedAudio: retain drops chunks outside the window and trims fronts a second at a time") {
    TimedAudio a(1000);
    std::vector<float> s(5000, 1.0f);
    a.beginChunk(0.0);  a.append(s.data(), s.size());            // [0, 5)
    a.beginChunk(10.0); a.append(s.data(), s.size());            // [10, 15): current
    a.retain(6.0, 100.0);                                         // the first chunk ends before 6
    CHECK(a.size() == 5000);
    a.retain(10.5, 100.0);                                        // under a second into the chunk: kept
    CHECK(a.size() == 5000);
    a.retain(12.5, 100.0);                                        // 2.5 s in: trimmed
    CHECK(a.size() == 2500);
    float v = 0.0f;
    a.sample(13.0, 13.0, &v, 1); CHECK(v == doctest::Approx(1.0f));
}

TEST_CASE("TimedAudio: holds at most kMaxSeconds, dropping the oldest chunks first") {
    TimedAudio a(100);
    std::vector<float> s(2000, 1.0f);                             // 20 s per chunk at 100 Hz
    a.beginChunk(0.0);  a.append(s.data(), s.size());
    a.beginChunk(20.0); a.append(s.data(), s.size());             // 40 s held: the first chunk goes
    CHECK(a.size() == 2000);
    float v = 0.0f;
    a.sample(10.0, 10.0, &v, 1); CHECK(v == 0.0f);
    a.sample(30.0, 30.0, &v, 1); CHECK(v == doctest::Approx(1.0f));
}
