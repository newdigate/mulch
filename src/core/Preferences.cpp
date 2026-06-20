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
    }
    return true;
}

} // namespace oss
