#include <doctest/doctest.h>
#include "modules/AudioMixerNode.h"
#include "core/Node.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

// Input ports: 0:in1 1:gain1 2:in2 3:gain2 4:in3 5:gain3 6:in4 7:gain4
static std::vector<Value> makeInputs() {
    std::vector<Value> in(8);
    for (int c = 0; c < 4; ++c) {
        in[c * 2]     = AudioRef{};   // unconnected audio
        in[c * 2 + 1] = 1.0f;         // unity gain
    }
    return in;
}

TEST_CASE("mixer sums two unit-gain inputs sample-by-sample") {
    AudioMixerNode mix;
    std::vector<float> a = {0.1f, 0.2f, 0.3f};
    std::vector<float> b = {0.4f, 0.4f, 0.4f};
    auto in = makeInputs();
    in[0] = AudioRef{a.data(), a.size(), 48000};
    in[2] = AudioRef{b.data(), b.size(), 48000};
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 1.0f / 60.0f};
    mix.evaluate(ctx);

    AudioRef o = std::get<AudioRef>(out[0]);
    REQUIRE(o.count == 3);
    CHECK(o.sampleRate == 48000);
    CHECK(o.samples[0] == doctest::Approx(0.5f));
    CHECK(o.samples[1] == doctest::Approx(0.6f));
    CHECK(o.samples[2] == doctest::Approx(0.7f));
}

TEST_CASE("mixer applies per-channel gain") {
    AudioMixerNode mix;
    std::vector<float> a = {0.5f, 0.5f};
    std::vector<float> b = {0.5f, 0.5f};
    auto in = makeInputs();
    in[0] = AudioRef{a.data(), 2, 48000}; in[1] = 0.5f;
    in[2] = AudioRef{b.data(), 2, 48000}; in[3] = 0.25f;
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);

    AudioRef o = std::get<AudioRef>(out[0]);
    REQUIRE(o.count == 2);
    CHECK(o.samples[0] == doctest::Approx(0.5f * 0.5f + 0.5f * 0.25f));   // 0.375
}

TEST_CASE("mixer clamps the summed signal to [-1, 1]") {
    AudioMixerNode mix;
    std::vector<float> s = {0.9f};
    auto in = makeInputs();
    for (int c = 0; c < 4; ++c) in[c * 2] = AudioRef{s.data(), 1, 48000};  // 4 * 0.9 = 3.6
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);

    AudioRef o = std::get<AudioRef>(out[0]);
    REQUIRE(o.count == 1);
    CHECK(o.samples[0] == doctest::Approx(1.0f));   // clamped
}

TEST_CASE("mixer output length is the longest input; shorter inputs zero-pad") {
    AudioMixerNode mix;
    std::vector<float> a = {0.1f, 0.1f, 0.1f, 0.1f};   // 4 samples
    std::vector<float> b = {0.2f, 0.2f};               // 2 samples
    auto in = makeInputs();
    in[0] = AudioRef{a.data(), 4, 48000};
    in[2] = AudioRef{b.data(), 2, 48000};
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);

    AudioRef o = std::get<AudioRef>(out[0]);
    REQUIRE(o.count == 4);
    CHECK(o.samples[0] == doctest::Approx(0.3f));   // a + b
    CHECK(o.samples[2] == doctest::Approx(0.1f));   // a only (b exhausted)
    CHECK(o.samples[3] == doctest::Approx(0.1f));
}

TEST_CASE("mixer with nothing connected outputs an empty block") {
    AudioMixerNode mix;
    auto in = makeInputs();
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.016f};
    mix.evaluate(ctx);
    CHECK(std::get<AudioRef>(out[0]).count == 0);
}
