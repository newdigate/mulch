#include <doctest/doctest.h>
#include "modules/AudioMixerNode.h"
#include "core/Node.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

// Input ports per channel: in / gain / pan -> 12 ports for 4 channels.
static std::vector<Value> makeInputs() {
    std::vector<Value> in(12);
    for (int c = 0; c < 4; ++c) {
        in[c * 3]     = AudioRef{};   // unconnected audio
        in[c * 3 + 1] = 1.0f;         // unity gain
        in[c * 3 + 2] = 0.0f;         // centre pan
    }
    return in;
}

TEST_CASE("centred mono inputs sum equally into both stereo channels") {
    AudioMixerNode mix;
    std::vector<float> a = {0.1f, 0.2f, 0.3f};
    std::vector<float> b = {0.4f, 0.4f, 0.4f};
    auto in = makeInputs();
    in[0] = AudioRef{a.data(), a.size(), 48000};
    in[3] = AudioRef{b.data(), b.size(), 48000};
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 1.0f / 60.0f};
    mix.evaluate(ctx);

    AudioRef o = std::get<AudioRef>(out[0]);
    REQUIRE(o.channels == 2);
    REQUIRE(o.count == 6);                 // 3 frames * 2 channels
    REQUIRE(o.frames() == 3);
    CHECK(o.samples[0] == doctest::Approx(0.5f));   // L0
    CHECK(o.samples[1] == doctest::Approx(0.5f));   // R0 == L0 (centred)
    CHECK(o.samples[4] == doctest::Approx(0.7f));   // L2
    CHECK(o.samples[5] == doctest::Approx(0.7f));   // R2
}

TEST_CASE("pan places a mono input in the stereo field") {
    AudioMixerNode mix;
    std::vector<float> s = {0.5f};
    SUBCASE("hard left") {
        auto in = makeInputs();
        in[0] = AudioRef{s.data(), 1, 48000};
        in[2] = -1.0f;                      // pan 1 = full left
        std::vector<Value> out(1);
        EvalContext ctx{in, out, 0.016f};
        mix.evaluate(ctx);
        AudioRef o = std::get<AudioRef>(out[0]);
        CHECK(o.samples[0] == doctest::Approx(0.5f));   // L
        CHECK(o.samples[1] == doctest::Approx(0.0f));   // R muted
    }
    SUBCASE("hard right") {
        auto in = makeInputs();
        in[0] = AudioRef{s.data(), 1, 48000};
        in[2] = 1.0f;                       // pan 1 = full right
        std::vector<Value> out(1);
        EvalContext ctx{in, out, 0.016f};
        mix.evaluate(ctx);
        AudioRef o = std::get<AudioRef>(out[0]);
        CHECK(o.samples[0] == doctest::Approx(0.0f));   // L muted
        CHECK(o.samples[1] == doctest::Approx(0.5f));   // R
    }
}

TEST_CASE("mixer applies per-channel gain") {
    AudioMixerNode mix;
    std::vector<float> a = {0.5f}, b = {0.5f};
    auto in = makeInputs();
    in[0] = AudioRef{a.data(), 1, 48000}; in[1] = 0.5f;
    in[3] = AudioRef{b.data(), 1, 48000}; in[4] = 0.25f;
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    AudioRef o = std::get<AudioRef>(out[0]);
    CHECK(o.samples[0] == doctest::Approx(0.5f * 0.5f + 0.5f * 0.25f));   // 0.375 (L, centred)
}

TEST_CASE("mixer clamps each channel to [-1, 1]") {
    AudioMixerNode mix;
    std::vector<float> s = {0.9f};
    auto in = makeInputs();
    for (int c = 0; c < 4; ++c) in[c * 3] = AudioRef{s.data(), 1, 48000};  // 4 * 0.9 = 3.6
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    AudioRef o = std::get<AudioRef>(out[0]);
    CHECK(o.samples[0] == doctest::Approx(1.0f));   // L clamped
    CHECK(o.samples[1] == doctest::Approx(1.0f));   // R clamped
}

TEST_CASE("a stereo input keeps its own L/R") {
    AudioMixerNode mix;
    std::vector<float> st = {0.2f, 0.8f, 0.3f, 0.7f};   // 2 frames: (L,R)
    auto in = makeInputs();
    in[0] = AudioRef{st.data(), st.size(), 48000, 2};
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    AudioRef o = std::get<AudioRef>(out[0]);
    REQUIRE(o.channels == 2);
    REQUIRE(o.frames() == 2);
    CHECK(o.samples[0] == doctest::Approx(0.2f));   // L0
    CHECK(o.samples[1] == doctest::Approx(0.8f));   // R0
    CHECK(o.samples[3] == doctest::Approx(0.7f));   // R1
}

TEST_CASE("mixer output length is the longest input (in frames)") {
    AudioMixerNode mix;
    std::vector<float> a = {0.1f, 0.1f, 0.1f, 0.1f};   // 4 frames mono
    std::vector<float> b = {0.2f, 0.2f};               // 2 frames mono
    auto in = makeInputs();
    in[0] = AudioRef{a.data(), 4, 48000};
    in[3] = AudioRef{b.data(), 2, 48000};
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    AudioRef o = std::get<AudioRef>(out[0]);
    REQUIRE(o.frames() == 4);
    CHECK(o.samples[0] == doctest::Approx(0.3f));   // L0: a+b
    CHECK(o.samples[4] == doctest::Approx(0.1f));   // L2: a only (b exhausted)
}

TEST_CASE("mixer with nothing connected outputs an empty block") {
    AudioMixerNode mix;
    auto in = makeInputs();
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    CHECK(std::get<AudioRef>(out[0]).count == 0);
}
