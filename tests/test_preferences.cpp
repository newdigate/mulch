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

TEST_CASE("texture-size round-trips, defaults, and clamps on parse") {
    Preferences p; p.textureWidth = 640; p.textureHeight = 480;
    Preferences r;
    REQUIRE(parsePreferences(serializePreferences(p), r));
    CHECK(r.textureWidth == 640);
    CHECK(r.textureHeight == 480);

    Preferences d;                                   // no texture-size line -> 1280x720 default
    REQUIRE(parsePreferences("oss-prefs 1\n", d));
    CHECK(d.textureWidth == 1280);
    CHECK(d.textureHeight == 720);

    Preferences big;                                 // clamp high
    REQUIRE(parsePreferences("oss-prefs 1\ntexture-size 5000 5000\n", big));
    CHECK(big.textureWidth == 1920);
    CHECK(big.textureHeight == 1080);

    Preferences small;                               // clamp low
    REQUIRE(parsePreferences("oss-prefs 1\ntexture-size 0 0\n", small));
    CHECK(small.textureWidth == 320);
    CHECK(small.textureHeight == 240);
}

TEST_CASE("clampTextureSize bounds") {
    int w = 0, h = 0; clampTextureSize(w, h); CHECK(w == 320); CHECK(h == 240);
    w = 5000; h = 5000; clampTextureSize(w, h); CHECK(w == 1920); CHECK(h == 1080);
    w = 640;  h = 480;  clampTextureSize(w, h); CHECK(w == 640);  CHECK(h == 480);
}
