#include <doctest/doctest.h>
#include "modules/CrossoverFilterNode.h"
#include "core/Node.h"
#include "core/Value.h"
#include <cmath>
#include <vector>

using namespace oss;

static const float kPi = 3.14159265358979323846f;

// Drive the node with a continuous sine of `hz` for several blocks (default cutoffs
// 200/2000 Hz, resonance 0) and return the summed squared energy of each band over
// the final settled block.
static void bandEnergies(float hz, double& bass, double& mid, double& treble) {
    CrossoverFilterNode n;
    int sr = 48000, block = 2048, phase = 0;
    std::vector<float> sig(block);
    bass = mid = treble = 0.0;
    for (int b = 0; b < 12; ++b) {
        for (int i = 0; i < block; ++i, ++phase)
            sig[i] = std::sin(2.0f * kPi * hz * (float)phase / (float)sr);
        std::vector<Value> in = { AudioRef{sig.data(), (std::size_t)block, sr},
                                  200.0f, 0.0f, 2000.0f, 0.0f };
        std::vector<Value> out(3);
        EvalContext ctx{in, out, 0.0426667f};   // ~2048/48000 s
        n.evaluate(ctx);
        if (b == 11) {
            AudioRef B = std::get<AudioRef>(out[0]);
            AudioRef M = std::get<AudioRef>(out[1]);
            AudioRef T = std::get<AudioRef>(out[2]);
            for (std::size_t i = 0; i < B.count; ++i) bass   += (double)B.samples[i] * B.samples[i];
            for (std::size_t i = 0; i < M.count; ++i) mid    += (double)M.samples[i] * M.samples[i];
            for (std::size_t i = 0; i < T.count; ++i) treble += (double)T.samples[i] * T.samples[i];
        }
    }
}

TEST_CASE("Crossover Filter: a low tone lands mostly in the bass band") {
    double bass, mid, treble;
    bandEnergies(80.0f, bass, mid, treble);        // 80 Hz, below the 200 Hz low cutoff
    CHECK(bass > mid);
    CHECK(bass > treble);
    CHECK(treble < 0.1 * bass);
}

TEST_CASE("Crossover Filter: a high tone lands mostly in the treble band") {
    double bass, mid, treble;
    bandEnergies(8000.0f, bass, mid, treble);      // 8 kHz, above the 2 kHz high cutoff
    CHECK(treble > mid);
    CHECK(treble > bass);
    CHECK(bass < 0.1 * treble);
}

TEST_CASE("Crossover Filter: outputs are mono AudioRefs matching the input block") {
    CrossoverFilterNode n;
    std::vector<float> sig = {0.1f, -0.2f, 0.3f, -0.4f};
    std::vector<Value> in = { AudioRef{sig.data(), sig.size(), 44100},
                              200.0f, 0.0f, 2000.0f, 0.0f };
    std::vector<Value> out(3);
    EvalContext ctx{in, out, 0.016f};
    n.evaluate(ctx);
    for (int k = 0; k < 3; ++k) {
        AudioRef o = std::get<AudioRef>(out[k]);
        CHECK(o.count == sig.size());
        CHECK(o.sampleRate == 44100);
        REQUIRE(o.samples != nullptr);
    }
}

TEST_CASE("Crossover Filter: an unconnected input yields three empty blocks") {
    CrossoverFilterNode n;
    std::vector<Value> in = { AudioRef{}, 200.0f, 0.0f, 2000.0f, 0.0f };
    std::vector<Value> out(3);
    EvalContext ctx{in, out, 0.016f};
    n.evaluate(ctx);
    for (int k = 0; k < 3; ++k) {
        AudioRef o = std::get<AudioRef>(out[k]);
        CHECK(o.count == 0);
    }
}
