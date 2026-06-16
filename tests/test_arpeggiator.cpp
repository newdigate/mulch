#include <doctest/doctest.h>
#include "modules/ArpeggiatorNode.h"
#include "core/Node.h"
#include "core/Value.h"
#include <string>
#include <vector>

using namespace oss;

// Drive one evaluate() with the given input events + dt; return generated events.
static std::vector<MidiEvent> step(ArpeggiatorNode& arp,
                                   const std::vector<MidiEvent>& inEvents, float dt,
                                   float rate = 10.0f, float gate = 0.5f,
                                   float octaves = 1.0f, float mode = 0.0f) {
    std::vector<Value> in(5);
    in[0] = MidiRef{inEvents.data(), inEvents.size()};
    in[1] = rate; in[2] = gate; in[3] = octaves; in[4] = mode;
    std::vector<Value> out(1);
    EvalContext ctx{in, out, dt};
    arp.evaluate(ctx);
    MidiRef o = std::get<MidiRef>(out[0]);
    return std::vector<MidiEvent>(o.events, o.events + o.count);
}

static std::vector<int> noteOns(const std::vector<MidiEvent>& evs) {
    std::vector<int> ns;
    for (auto& e : evs) if (midiIsNoteOn(e)) ns.push_back(e.data1);
    return ns;
}

TEST_CASE("MidiRef maps to PortType::Midi") {
    Value v = MidiRef{};
    CHECK(typeOf(v) == PortType::Midi);
    CHECK(std::string(portTypeName(PortType::Midi)) == "Midi");
}

TEST_CASE("arpeggiator steps up through held notes and cycles") {
    ArpeggiatorNode arp;
    std::vector<MidiEvent> chord = {midiNoteOn(60, 100), midiNoteOn(64, 100)};
    std::vector<int> ons = noteOns(step(arp, chord, 0.001f));   // first step fires
    for (int i = 0; i < 40; ++i)                                // ~0.4s -> several steps
        for (int n : noteOns(step(arp, {}, 0.01f))) ons.push_back(n);
    REQUIRE(ons.size() >= 4);
    CHECK(ons[0] == 60);
    CHECK(ons[1] == 64);
    CHECK(ons[2] == 60);
    CHECK(ons[3] == 64);
}

TEST_CASE("arpeggiator releases a note after its gate time") {
    ArpeggiatorNode arp;
    step(arp, {midiNoteOn(60, 100)}, 0.001f, 10.0f, 0.5f);     // note-on 60; off due ~0.05s
    auto e = step(arp, {}, 0.06f, 10.0f, 0.5f);                 // advance past the gate
    bool sawOff60 = false;
    for (auto& ev : e) if (midiIsNoteOff(ev) && ev.data1 == 60) sawOff60 = true;
    CHECK(sawOff60);
}

TEST_CASE("arpeggiator spans octaves") {
    ArpeggiatorNode arp;
    std::vector<int> seq = noteOns(step(arp, {midiNoteOn(60, 100)}, 0.001f, 10, 0.5f, 2.0f));
    for (int i = 0; i < 30; ++i)
        for (int n : noteOns(step(arp, {}, 0.01f, 10, 0.5f, 2.0f))) seq.push_back(n);
    REQUIRE(seq.size() >= 4);
    CHECK(seq[0] == 60);
    CHECK(seq[1] == 72);   // +1 octave
    CHECK(seq[2] == 60);
    CHECK(seq[3] == 72);
}

TEST_CASE("arpeggiator down mode descends") {
    ArpeggiatorNode arp;
    std::vector<MidiEvent> chord = {midiNoteOn(60, 100), midiNoteOn(64, 100)};
    std::vector<int> seq = noteOns(step(arp, chord, 0.001f, 10, 0.5f, 1.0f, 1.0f /*down*/));
    for (int i = 0; i < 20; ++i)
        for (int n : noteOns(step(arp, {}, 0.01f, 10, 0.5f, 1.0f, 1.0f))) seq.push_back(n);
    REQUIRE(seq.size() >= 3);
    CHECK(seq[0] == 64);   // highest first
    CHECK(seq[1] == 60);
    CHECK(seq[2] == 64);
}

TEST_CASE("arpeggiator emits nothing with no held notes") {
    ArpeggiatorNode arp;
    for (int i = 0; i < 10; ++i) CHECK(step(arp, {}, 0.02f).empty());
}
