#include <doctest/doctest.h>
#include "core/Preferences.h"

using namespace oss;

TEST_CASE("Preferences serialize/parse round-trip") {
    Preferences p;
    p.audioOutputDeviceId = "Built-in Output id";
    p.audioInputDeviceId  = "USB Mic id";
    p.enabledMidiInputs   = {"Keystation 49", "IAC Bus 1"};
    p.enabledMidiOutputs  = {"IAC Bus 1"};
    Preferences r;
    REQUIRE(parsePreferences(serializePreferences(p), r));
    CHECK(r.audioOutputDeviceId == "Built-in Output id");
    CHECK(r.audioInputDeviceId == "USB Mic id");
    REQUIRE(r.enabledMidiInputs.size() == 2);
    CHECK(r.enabledMidiInputs[1] == "IAC Bus 1");
    REQUIRE(r.enabledMidiOutputs.size() == 1);
    CHECK(r.enabledMidiOutputs[0] == "IAC Bus 1");
}

TEST_CASE("empty Preferences round-trips and a bad header is rejected") {
    Preferences r;
    REQUIRE(parsePreferences(serializePreferences(Preferences{}), r));
    CHECK(r.audioOutputDeviceId.empty());
    CHECK(r.enabledMidiInputs.empty());
    CHECK_FALSE(parsePreferences("garbage\n", r));
    CHECK_FALSE(parsePreferences("", r));
}

TEST_CASE("MIDI enable helpers are idempotent add/remove") {
    Preferences p;
    p.setMidiInputEnabled("port A", true);
    p.setMidiInputEnabled("port A", true);          // idempotent: no duplicate
    CHECK(p.enabledMidiInputs.size() == 1);
    CHECK(p.midiInputEnabled("port A"));
    p.setMidiInputEnabled("port A", false);
    CHECK_FALSE(p.midiInputEnabled("port A"));
    CHECK(p.enabledMidiInputs.empty());
    p.setMidiOutputEnabled("out B", true);
    CHECK(p.midiOutputEnabled("out B"));
    CHECK_FALSE(p.midiOutputEnabled("missing"));
}
