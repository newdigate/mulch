#include <doctest/doctest.h>
#include "modules/MidiMergeNode.h"
#include "core/Node.h"
#include "core/Value.h"
#include <vector>

using namespace oss;

static std::vector<Value> emptyInputs() {
    std::vector<Value> in(4);
    for (int i = 0; i < 4; ++i) in[i] = MidiRef{};
    return in;
}

TEST_CASE("merge concatenates events from all inputs in input order") {
    MidiMergeNode m;
    std::vector<MidiEvent> a = {midiNoteOn(36, 100, 9)};
    std::vector<MidiEvent> b = {midiNoteOn(38, 100, 9), midiNoteOff(38, 9)};
    auto in = emptyInputs();
    in[0] = MidiRef{a.data(), a.size()};
    in[2] = MidiRef{b.data(), b.size()};
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.016f};
    m.evaluate(ctx);

    MidiRef o = std::get<MidiRef>(out[0]);
    REQUIRE(o.count == 3);
    CHECK(o.events[0].data1 == 36);            // input 0 first
    CHECK(o.events[1].data1 == 38);            // then input 2
    CHECK(midiIsNoteOff(o.events[2]));
}

TEST_CASE("merge keeps all four inputs") {
    MidiMergeNode m;
    std::vector<MidiEvent> e0 = {midiNoteOn(36, 100, 9)}, e1 = {midiNoteOn(38, 100, 9)},
                           e2 = {midiNoteOn(42, 100, 9)}, e3 = {midiNoteOn(46, 100, 9)};
    auto in = emptyInputs();
    in[0] = MidiRef{e0.data(), 1}; in[1] = MidiRef{e1.data(), 1};
    in[2] = MidiRef{e2.data(), 1}; in[3] = MidiRef{e3.data(), 1};
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.016f};
    m.evaluate(ctx);

    MidiRef o = std::get<MidiRef>(out[0]);
    REQUIRE(o.count == 4);
    CHECK(o.events[0].data1 == 36);
    CHECK(o.events[1].data1 == 38);
    CHECK(o.events[2].data1 == 42);
    CHECK(o.events[3].data1 == 46);
}

TEST_CASE("merge with all inputs empty outputs nothing") {
    MidiMergeNode m;
    auto in = emptyInputs();
    std::vector<Value> out(1);
    EvalContext ctx{in, out, 0.016f};
    m.evaluate(ctx);
    CHECK(std::get<MidiRef>(out[0]).count == 0);
}
