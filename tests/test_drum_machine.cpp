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

#include "audio/SampleVoice.h"

// A clip of `frames` stereo frames; L = R = the frame index (a 0,1,2,... ramp).
static AudioClip rampClip(int frames) {
    AudioClip c; c.ok = true; c.sampleRate = 48000; c.channels = 2;
    c.samples.resize((std::size_t)frames * 2);
    for (int i = 0; i < frames; ++i) { c.samples[(std::size_t)i*2] = (float)i; c.samples[(std::size_t)i*2+1] = (float)i; }
    return c;
}

TEST_CASE("SampleVoice plays a clip forward at rate 1 then deactivates") {
    AudioClip c = rampClip(5);              // values 0,1,2,3,4
    SampleVoice v; v.trigger(c, false);
    CHECK(v.active());
    float L[8] = {0}, R[8] = {0};
    v.render(c, 1.0, 1.0f, 1.0f, L, R, 8);  // 8 out frames, clip only 5
    CHECK(L[0] == doctest::Approx(0.0f));
    CHECK(L[1] == doctest::Approx(1.0f));
    CHECK(L[3] == doctest::Approx(3.0f));
    CHECK(L[4] == doctest::Approx(0.0f));   // ran off the end (pos hit frames-1) -> silence
    CHECK(!v.active());
}

TEST_CASE("SampleVoice rate 2 reads every other sample (interpolation at 0.5)") {
    AudioClip c = rampClip(9);
    SampleVoice v; v.trigger(c, false);
    float L[4] = {0}, R[4] = {0};
    v.render(c, 2.0, 1.0f, 1.0f, L, R, 4);
    CHECK(L[0] == doctest::Approx(0.0f));
    CHECK(L[1] == doctest::Approx(2.0f));
    CHECK(L[2] == doctest::Approx(4.0f));
}

TEST_CASE("SampleVoice reverse starts at the end and plays backward") {
    AudioClip c = rampClip(5);              // 0..4
    SampleVoice v; v.trigger(c, true);      // pos = 4
    float L[3] = {0}, R[3] = {0};
    v.render(c, -1.0, 1.0f, 1.0f, L, R, 3);
    CHECK(L[0] == doctest::Approx(4.0f));
    CHECK(L[1] == doctest::Approx(3.0f));
    CHECK(L[2] == doctest::Approx(2.0f));
}

TEST_CASE("SampleVoice render adds (mixes) and applies per-channel gains") {
    AudioClip c = rampClip(4);              // 0,1,2,3
    SampleVoice v; v.trigger(c, false);
    float L[3] = {10.0f, 10.0f, 10.0f}, R[3] = {0};
    v.render(c, 1.0, 0.5f, 0.25f, L, R, 3);
    CHECK(L[1] == doctest::Approx(10.0f + 1.0f * 0.5f));   // added, not overwritten
    CHECK(R[1] == doctest::Approx(1.0f * 0.25f));
}

TEST_CASE("SampleVoice retrigger restarts; rate 0 and empty clip are no-ops") {
    AudioClip c = rampClip(4);
    SampleVoice v; v.trigger(c, false);
    float L[2] = {0}, R[2] = {0};
    v.render(c, 1.0, 1.0f, 1.0f, L, R, 2);  // consumes 0,1
    v.trigger(c, false);                    // restart at 0
    float L2[2] = {0}, R2[2] = {0};
    v.render(c, 1.0, 1.0f, 1.0f, L2, R2, 2);
    CHECK(L2[0] == doctest::Approx(0.0f));
    // rate 0 -> no-op (no advance, no infinite hold expectation), empty clip -> inactive
    AudioClip empty; SampleVoice e; e.trigger(empty, false); CHECK(!e.active());
}
