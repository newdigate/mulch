#pragma once
#include <string>
#include <vector>

namespace oss {

// App-global settings (not project state). Audio devices are keyed by libsoundio's
// stable device id; MIDI interfaces by port name. GL-free.
struct Preferences {
    std::string audioOutputDeviceId;                 // "" = system default
    std::string audioInputDeviceId;                  // "" = system default
    std::vector<std::string> enabledMidiInputs;      // hardware port names
    std::vector<std::string> enabledMidiOutputs;

    bool midiInputEnabled(const std::string& name) const;
    void setMidiInputEnabled(const std::string& name, bool on);   // idempotent add/remove
    bool midiOutputEnabled(const std::string& name) const;
    void setMidiOutputEnabled(const std::string& name, bool on);
};

std::string serializePreferences(const Preferences& p);
bool        parsePreferences(const std::string& text, Preferences& out);   // false on bad header

} // namespace oss
