#include <doctest/doctest.h>
#include "modules/MonoToStereoNode.h"
#include "modules/StereoToMonoNode.h"
#include "core/Node.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

TEST_CASE("Mono to Stereo: centre duplicates, hard left mutes right") {
    MonoToStereoNode n;
    std::vector<float> m = {0.2f, -0.4f, 0.6f};
    SUBCASE("centre") {
        std::vector<Value> in = { AudioRef{m.data(), m.size(), 48000}, 0.0f };
        std::vector<Value> out(2);
        EvalContext ctx{in, out, 0.016f};
        n.evaluate(ctx);
        AudioRef L = std::get<AudioRef>(out[0]), R = std::get<AudioRef>(out[1]);
        REQUIRE(L.count == 3); REQUIRE(R.count == 3);
        CHECK(L.samples[0] == doctest::Approx(0.2f));
        CHECK(R.samples[0] == doctest::Approx(0.2f));
        CHECK(L.samples[2] == doctest::Approx(0.6f));
        CHECK(R.samples[2] == doctest::Approx(0.6f));
    }
    SUBCASE("hard left") {
        std::vector<Value> in = { AudioRef{m.data(), m.size(), 48000}, -1.0f };
        std::vector<Value> out(2);
        EvalContext ctx{in, out, 0.016f};
        n.evaluate(ctx);
        AudioRef L = std::get<AudioRef>(out[0]), R = std::get<AudioRef>(out[1]);
        CHECK(L.samples[1] == doctest::Approx(-0.4f));
        CHECK(R.samples[1] == doctest::Approx(0.0f));
    }
}

TEST_CASE("Stereo to Mono: centre averages, balance picks a side, output clamps") {
    StereoToMonoNode n;
    std::vector<float> l = {0.2f, 0.8f}, r = {0.6f, 0.4f};
    SUBCASE("centre averages") {
        std::vector<Value> in = { AudioRef{l.data(), 2, 48000}, AudioRef{r.data(), 2, 48000}, 0.0f };
        std::vector<Value> out(1);
        EvalContext ctx{in, out, 0.016f};
        n.evaluate(ctx);
        AudioRef o = std::get<AudioRef>(out[0]);
        REQUIRE(o.count == 2);
        CHECK(o.samples[0] == doctest::Approx(0.4f));   // 0.5*(0.2+0.6)
        CHECK(o.samples[1] == doctest::Approx(0.6f));   // 0.5*(0.8+0.4)
    }
    SUBCASE("balance -1 keeps left only") {
        std::vector<Value> in = { AudioRef{l.data(), 2, 48000}, AudioRef{r.data(), 2, 48000}, -1.0f };
        std::vector<Value> out(1);
        EvalContext ctx{in, out, 0.016f};
        n.evaluate(ctx);
        AudioRef o = std::get<AudioRef>(out[0]);
        CHECK(o.samples[0] == doctest::Approx(0.2f));
        CHECK(o.samples[1] == doctest::Approx(0.8f));
    }
    SUBCASE("output clamps to [-1,1]") {
        std::vector<float> big = {2.0f, 2.0f};
        std::vector<Value> in = { AudioRef{big.data(), 2, 48000}, AudioRef{big.data(), 2, 48000}, 0.0f };
        std::vector<Value> out(1);
        EvalContext ctx{in, out, 0.016f};
        n.evaluate(ctx);
        AudioRef o = std::get<AudioRef>(out[0]);
        CHECK(o.samples[0] == doctest::Approx(1.0f));
    }
}
