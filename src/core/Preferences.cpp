#include "core/Preferences.h"
#include <algorithm>
#include <sstream>

namespace oss {

namespace {
bool listed(const std::vector<std::string>& v, const std::string& n) {
    return std::find(v.begin(), v.end(), n) != v.end();
}
void setListed(std::vector<std::string>& v, const std::string& n, bool on) {
    auto it = std::find(v.begin(), v.end(), n);
    if (on && it == v.end()) v.push_back(n);
    else if (!on && it != v.end()) v.erase(it);
}
} // namespace

void clampTextureSize(int& w, int& h) {
    if (w < 320)  w = 320;
    if (w > 1920) w = 1920;
    if (h < 240)  h = 240;
    if (h > 1080) h = 1080;
}

bool Preferences::midiInputEnabled(const std::string& n) const  { return listed(enabledMidiInputs, n); }
void Preferences::setMidiInputEnabled(const std::string& n, bool on)  { setListed(enabledMidiInputs, n, on); }
bool Preferences::midiOutputEnabled(const std::string& n) const { return listed(enabledMidiOutputs, n); }
void Preferences::setMidiOutputEnabled(const std::string& n, bool on) { setListed(enabledMidiOutputs, n, on); }

std::string serializePreferences(const Preferences& p) {
    std::string out = "oss-prefs 1\n";
    if (!p.audioOutputDeviceId.empty()) out += "audio-out " + p.audioOutputDeviceId + "\n";
    if (!p.audioInputDeviceId.empty())  out += "audio-in "  + p.audioInputDeviceId  + "\n";
    for (const std::string& n : p.enabledMidiInputs)  out += "midi-in "  + n + "\n";
    for (const std::string& n : p.enabledMidiOutputs) out += "midi-out " + n + "\n";
    out += "texture-size " + std::to_string(p.textureWidth) + " " + std::to_string(p.textureHeight) + "\n";
    out += "sync-in "  + std::to_string(p.syncInMode)  + " " + p.syncInSource + "\n";
    out += "sync-out " + std::to_string(p.syncOutMode) + " " + p.syncOutDest  + "\n";
    return out;
}

bool parsePreferences(const std::string& text, Preferences& out) {
    out = Preferences{};
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("oss-prefs", 0) != 0) return false;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string kw; ls >> kw;
        std::string rest; std::getline(ls >> std::ws, rest);
        if      (kw == "audio-out") out.audioOutputDeviceId = rest;
        else if (kw == "audio-in")  out.audioInputDeviceId  = rest;
        else if (kw == "midi-in"  && !rest.empty()) out.enabledMidiInputs.push_back(rest);
        else if (kw == "midi-out" && !rest.empty()) out.enabledMidiOutputs.push_back(rest);
        else if (kw == "texture-size") {
            std::istringstream rs(rest);
            int w = 0, h = 0;
            rs >> w >> h;
            if (!rs.fail()) { clampTextureSize(w, h); out.textureWidth = w; out.textureHeight = h; }
        }
        else if (kw == "sync-in") {
            std::istringstream rs(rest); int mode = 0; rs >> mode;
            std::string name; std::getline(rs >> std::ws, name);
            out.syncInMode   = (mode < 0 || mode > 1) ? 0 : mode;
            out.syncInSource = name;
        }
        else if (kw == "sync-out") {
            std::istringstream rs(rest); int mode = 0; rs >> mode;
            std::string name; std::getline(rs >> std::ws, name);
            out.syncOutMode = (mode < 0 || mode > 1) ? 0 : mode;
            out.syncOutDest = name;
        }
    }
    return true;
}

} // namespace oss
