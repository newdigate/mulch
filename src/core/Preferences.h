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
    int textureWidth  = 1280;    // streaming-texture (render FBO) resolution
    int textureHeight = 720;
    int audioBufferMs = 150;     // output ring length (ms); trades latency for under-run headroom [20,500]
    int         syncInMode  = 0;   // 0 = Off, 1 = Beat Clock, 2 = MTC
    std::string syncInSource;      // MIDI input port name
    int         syncOutMode = 0;   // 0 = Off, 1 = Beat Clock, 2 = MTC
    std::string syncOutDest;       // MIDI output port name
    int         syncFrameRate = 3; // MTC send rate: 0=24 1=25 2=29.97df 3=30 (receive auto-detects)
    std::string projectsDir;       // default dir for project Open/Save dialogs ("" = OS default)
    std::string assetLibraryDir;   // default dir for asset-library + media dialogs ("" = OS default)

    bool midiInputEnabled(const std::string& name) const;
    void setMidiInputEnabled(const std::string& name, bool on);   // idempotent add/remove
    bool midiOutputEnabled(const std::string& name) const;
    void setMidiOutputEnabled(const std::string& name, bool on);
};

std::string serializePreferences(const Preferences& p);
bool        parsePreferences(const std::string& text, Preferences& out);   // false on bad header

// Clamp a requested streaming-texture size to sane bounds: [320,1920] x [240,1080].
void clampTextureSize(int& w, int& h);

} // namespace oss
