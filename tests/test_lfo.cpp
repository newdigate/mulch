#include <doctest/doctest.h>
#include "modules/LfoNode.h"
#include "core/Node.h"
#include "core/Transport.h"
#include "core/Value.h"
#include <algorithm>
#include <variant>
#include <vector>

using namespace oss;

// The 7 input Values in port order: waveform, sync, rate Hz, rate sync, min, max, amplify.
static std::vector<Value> lfoInputs(float waveform, bool sync, float rateHz,
                                    float rateSync, float lo, float hi, float amplify = 1.0f) {
    return { Value(waveform), Value(sync), Value(rateHz),
             Value(rateSync), Value(lo), Value(hi), Value(amplify) };
}

TEST_CASE("lfoSample produces the expected deterministic shapes") {
    CHECK(lfoSample(0, 0.0)  == doctest::Approx(0.5));   // Sine
    CHECK(lfoSample(0, 0.25) == doctest::Approx(1.0));
    CHECK(lfoSample(0, 0.5)  == doctest::Approx(0.5));
    CHECK(lfoSample(0, 0.75) == doctest::Approx(0.0));
    CHECK(lfoSample(1, 0.0)  == doctest::Approx(0.0));   // Triangle
    CHECK(lfoSample(1, 0.25) == doctest::Approx(0.5));
    CHECK(lfoSample(1, 0.5)  == doctest::Approx(1.0));
    CHECK(lfoSample(2, 0.25) == doctest::Approx(1.0));   // Square
    CHECK(lfoSample(2, 0.75) == doctest::Approx(0.0));
    CHECK(lfoSample(3, 0.3)  == doctest::Approx(0.3));   // Ramp Up
    CHECK(lfoSample(4, 0.3)  == doctest::Approx(0.7));   // Ramp Down
}

TEST_CASE("free-run LFO advances phase by rate*dt") {
    LfoNode lfo;
    std::vector<Value> out(2);
    std::vector<Value> in = lfoInputs(0.0f, false, 1.0f, 8.0f, 0.0f, 1.0f);
    EvalContext c0{in, out, 0.0f};   lfo.evaluate(c0);   // phase 0 -> sine 0.5
    CHECK(std::get<float>(out[0]) == doctest::Approx(0.5f));
    EvalContext c1{in, out, 0.25f};  lfo.evaluate(c1);   // +0.25 -> sine peak 1.0
    CHECK(std::get<float>(out[0]) == doctest::Approx(1.0f));
}

TEST_CASE("output range maps the normalised waveform into [min,max]") {
    LfoNode lfo;
    std::vector<Value> out(2);
    std::vector<Value> in = lfoInputs(0.0f, false, 1.0f, 8.0f, 0.0f, 2.0f);
    EvalContext c0{in, out, 0.0f};   lfo.evaluate(c0);   // mid 0.5 -> 1.0
    CHECK(std::get<float>(out[0]) == doctest::Approx(1.0f));
    EvalContext c1{in, out, 0.25f};  lfo.evaluate(c1);   // peak 1.0 -> 2.0
    CHECK(std::get<float>(out[0]) == doctest::Approx(2.0f));
}

TEST_CASE("synced LFO derives phase from the transport bar position") {
    LfoNode lfo;
    Transport t; t.bpm = 120.0; t.seconds = 0.5;   // 2 s/bar -> bars = 0.25
    std::vector<Value> in = lfoInputs(0.0f, true, 1.0f, 8.0f, 0.0f, 1.0f);  // "1 bar"
    std::vector<Value> out(2);
    EvalContext ctx{in, out, 0.0f, &t};
    lfo.evaluate(ctx);
    CHECK(std::get<float>(out[0]) == doctest::Approx(1.0f));   // sine(0.25) = 1.0
}

TEST_CASE("waveform input rounds to the nearest index and clamps") {
    LfoNode lfo;
    std::vector<Value> out(2);
    {   // 1.4 -> Triangle (index 1); triangle at phase 0 = 0
        std::vector<Value> in = lfoInputs(1.4f, false, 1.0f, 8.0f, 0.0f, 1.0f);
        EvalContext ctx{in, out, 0.0f};
        lfo.evaluate(ctx);
        CHECK(std::get<float>(out[0]) == doctest::Approx(0.0f));
    }
    {   // 9.0 clamps to Sample & Hold (index 5) -> a value in [0,1], no out-of-bounds
        std::vector<Value> in = lfoInputs(9.0f, false, 1.0f, 8.0f, 0.0f, 1.0f);
        EvalContext ctx{in, out, 0.0f};
        lfo.evaluate(ctx);
        float v = std::get<float>(out[0]);
        CHECK(v >= 0.0f);
        CHECK(v <= 1.0f);
    }
}

TEST_CASE("Sample & Hold holds within a cycle and changes across cycles") {
    LfoNode lfo;
    std::vector<Value> out(2);
    auto evalSH = [&](float dt) {
        std::vector<Value> in = lfoInputs(5.0f, false, 1.0f, 8.0f, 0.0f, 1.0f);
        EvalContext ctx{in, out, dt};
        lfo.evaluate(ctx);
        return std::get<float>(out[0]);
    };
    float a = evalSH(0.0f);    // phase 0, initial held value
    float b = evalSH(0.4f);    // same cycle -> unchanged
    CHECK(a == doctest::Approx(b));
    std::vector<float> seen{a};
    for (int i = 0; i < 8; ++i) {
        float v = evalSH(1.0f);   // wrap a whole cycle -> re-latch
        bool known = std::any_of(seen.begin(), seen.end(),
            [&](float s){ return s == doctest::Approx(v); });
        if (!known) seen.push_back(v);
    }
    CHECK(seen.size() >= 2);   // not stuck on one value
}

TEST_CASE("the LFO exposes choice ports for waveform and sync rate") {
    LfoNode lfo;
    REQUIRE(lfo.inputs().size() == 7);
    CHECK(lfo.outputs().size() == 2);
    CHECK(lfo.inputs()[0].choices.size() == 6);    // waveform
    CHECK(std::get<float>(lfo.inputs()[0].defaultValue) == doctest::Approx(0.0f));
    CHECK(lfo.inputs()[3].choices.size() == 15);   // rate sync
    CHECK(std::get<float>(lfo.inputs()[3].defaultValue) == doctest::Approx(8.0f));
}

TEST_CASE("amplified output is the normal output times the amplify input") {
    LfoNode lfo;
    std::vector<Value> out(2);
    // Square wave (index 2) at phase 0 -> w01 = 1; min 0 max 2 -> out0 = 2.0.
    {   // amplify = 3 -> amplified = 6.0
        std::vector<Value> in = lfoInputs(2.0f, false, 1.0f, 8.0f, 0.0f, 2.0f, 3.0f);
        EvalContext ctx{in, out, 0.0f};
        lfo.evaluate(ctx);
        CHECK(std::get<float>(out[0]) == doctest::Approx(2.0f));   // "out": normal, unchanged
        CHECK(std::get<float>(out[1]) == doctest::Approx(6.0f));   // "amplified" = 2.0 * 3.0
    }
    {   // default amplify (1.0) -> amplified == out
        std::vector<Value> in = lfoInputs(2.0f, false, 1.0f, 8.0f, 0.0f, 2.0f);
        EvalContext ctx{in, out, 0.0f};
        lfo.evaluate(ctx);
        CHECK(std::get<float>(out[1]) == doctest::Approx(std::get<float>(out[0])));
    }
    {   // negative amplify inverts the amplified output
        std::vector<Value> in = lfoInputs(2.0f, false, 1.0f, 8.0f, 0.0f, 2.0f, -1.0f);
        EvalContext ctx{in, out, 0.0f};
        lfo.evaluate(ctx);
        CHECK(std::get<float>(out[1]) == doctest::Approx(-2.0f));   // -1 * 2.0
    }
}
