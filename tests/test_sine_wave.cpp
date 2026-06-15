#include <doctest/doctest.h>
#include "modules/SineWaveNode.h"
#include "audio/FFT.h"
#include "core/Node.h"
#include "core/Value.h"
#include <algorithm>
#include <cmath>
#include <vector>

using namespace oss;

TEST_CASE("SineWaveNode emits round(sampleRate*dt) samples within [-amp, amp]") {
    SineWaveNode node;
    std::vector<Value> in  = { Value(440.0f), Value(0.5f) };   // freq, amp
    std::vector<Value> out(1);
    EvalContext ctx{ in, out, 1.0f / 60.0f };
    node.evaluate(ctx);

    AudioRef a = std::get<AudioRef>(out[0]);
    CHECK(a.sampleRate == 48000);
    CHECK(a.count == (std::size_t)std::lround(48000.0 / 60.0));  // 800
    for (std::size_t i = 0; i < a.count; ++i) {
        CHECK(a.samples[i] >= -0.5001f);
        CHECK(a.samples[i] <=  0.5001f);
    }
}

TEST_CASE("SineWaveNode produces a tone at the requested frequency") {
    // bin = freq * N / sampleRate. With N=1024, sr=48000, choosing
    // freq = 10 * 48000 / 1024 = 468.75 Hz lands exactly on bin 10, so a
    // leakage-free FFT peak there confirms the generated pitch.
    const int N = 1024;
    SineWaveNode node;
    std::vector<Value> in  = { Value(468.75f), Value(1.0f) };
    std::vector<Value> out(1);
    EvalContext ctx{ in, out, (float)N / 48000.0f };           // dt -> exactly N samples
    node.evaluate(ctx);

    AudioRef a = std::get<AudioRef>(out[0]);
    REQUIRE(a.count == (std::size_t)N);
    std::vector<float> samples(a.samples, a.samples + a.count);
    std::vector<float> mag = magnitudeSpectrum(samples);        // N/2 bins
    int peak = (int)(std::max_element(mag.begin() + 1, mag.end()) - mag.begin());
    CHECK(peak == 10);
}

TEST_CASE("SineWaveNode phase is continuous across frames") {
    // Two back-to-back half-frames must equal one whole frame sample-for-sample.
    SineWaveNode split;
    std::vector<Value> in  = { Value(440.0f), Value(1.0f) };
    std::vector<Value> o1(1), o2(1);
    EvalContext c1{ in, o1, 200.0f / 48000.0f };
    EvalContext c2{ in, o2, 200.0f / 48000.0f };
    split.evaluate(c1);
    std::vector<float> first(std::get<AudioRef>(o1[0]).samples,
                             std::get<AudioRef>(o1[0]).samples + 200);
    split.evaluate(c2);
    std::vector<float> second(std::get<AudioRef>(o2[0]).samples,
                              std::get<AudioRef>(o2[0]).samples + 200);

    SineWaveNode whole;
    std::vector<Value> ow(1);
    EvalContext cw{ in, ow, 400.0f / 48000.0f };
    whole.evaluate(cw);
    const float* w = std::get<AudioRef>(ow[0]).samples;
    for (int i = 0; i < 200; ++i) {
        CHECK(w[i]       == doctest::Approx(first[i]).epsilon(1e-4));
        CHECK(w[200 + i] == doctest::Approx(second[i]).epsilon(1e-4));
    }
}
