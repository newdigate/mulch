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

// out[0] = left mono, out[1] = right mono.
TEST_CASE("centred mono inputs sum equally into both outputs") {
    AudioMixerNode mix;
    std::vector<float> a = {0.1f, 0.2f, 0.3f};
    std::vector<float> b = {0.4f, 0.4f, 0.4f};
    auto in = makeInputs();
    in[0] = AudioRef{a.data(), a.size(), 48000};
    in[3] = AudioRef{b.data(), b.size(), 48000};
    std::vector<Value> out(2);
    EvalContext ctx{in, out, 1.0f / 60.0f};
    mix.evaluate(ctx);

    AudioRef L = std::get<AudioRef>(out[0]), R = std::get<AudioRef>(out[1]);
    REQUIRE(L.count == 3); REQUIRE(R.count == 3);
    CHECK(L.samples[0] == doctest::Approx(0.5f));
    CHECK(R.samples[0] == doctest::Approx(0.5f));
    CHECK(L.samples[2] == doctest::Approx(0.7f));
    CHECK(R.samples[2] == doctest::Approx(0.7f));
}

TEST_CASE("pan places a mono input in the stereo field") {
    AudioMixerNode mix;
    std::vector<float> s = {0.5f};
    SUBCASE("hard left") {
        auto in = makeInputs();
        in[0] = AudioRef{s.data(), 1, 48000};
        in[2] = -1.0f;
        std::vector<Value> out(2);
        EvalContext ctx{in, out, 0.016f};
        mix.evaluate(ctx);
        CHECK(std::get<AudioRef>(out[0]).samples[0] == doctest::Approx(0.5f));   // L
        CHECK(std::get<AudioRef>(out[1]).samples[0] == doctest::Approx(0.0f));   // R muted
    }
    SUBCASE("hard right") {
        auto in = makeInputs();
        in[0] = AudioRef{s.data(), 1, 48000};
        in[2] = 1.0f;
        std::vector<Value> out(2);
        EvalContext ctx{in, out, 0.016f};
        mix.evaluate(ctx);
        CHECK(std::get<AudioRef>(out[0]).samples[0] == doctest::Approx(0.0f));   // L muted
        CHECK(std::get<AudioRef>(out[1]).samples[0] == doctest::Approx(0.5f));   // R
    }
}

TEST_CASE("mixer applies per-channel gain") {
    AudioMixerNode mix;
    std::vector<float> a = {0.5f}, b = {0.5f};
    auto in = makeInputs();
    in[0] = AudioRef{a.data(), 1, 48000}; in[1] = 0.5f;
    in[3] = AudioRef{b.data(), 1, 48000}; in[4] = 0.25f;
    std::vector<Value> out(2);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    CHECK(std::get<AudioRef>(out[0]).samples[0] == doctest::Approx(0.5f * 0.5f + 0.5f * 0.25f));
}

TEST_CASE("mixer clamps each output to [-1, 1]") {
    AudioMixerNode mix;
    std::vector<float> s = {0.9f};
    auto in = makeInputs();
    for (int c = 0; c < 4; ++c) in[c * 3] = AudioRef{s.data(), 1, 48000};
    std::vector<Value> out(2);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    CHECK(std::get<AudioRef>(out[0]).samples[0] == doctest::Approx(1.0f));
    CHECK(std::get<AudioRef>(out[1]).samples[0] == doctest::Approx(1.0f));
}

TEST_CASE("mixer output length is the longest input (in samples)") {
    AudioMixerNode mix;
    std::vector<float> a = {0.1f, 0.1f, 0.1f, 0.1f};
    std::vector<float> b = {0.2f, 0.2f};
    auto in = makeInputs();
    in[0] = AudioRef{a.data(), 4, 48000};
    in[3] = AudioRef{b.data(), 2, 48000};
    std::vector<Value> out(2);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    AudioRef L = std::get<AudioRef>(out[0]);
    REQUIRE(L.count == 4);
    CHECK(L.samples[0] == doctest::Approx(0.3f));   // a+b
    CHECK(L.samples[2] == doctest::Approx(0.1f));   // a only (b exhausted)
}

TEST_CASE("mixer with nothing connected outputs empty blocks") {
    AudioMixerNode mix;
    auto in = makeInputs();
    std::vector<Value> out(2);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    CHECK(std::get<AudioRef>(out[0]).count == 0);
    CHECK(std::get<AudioRef>(out[1]).count == 0);
}
