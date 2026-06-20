#pragma once
#include <functional>
#include <string>
#include <vector>

namespace oss {

struct Preferences;

// Preferences window: Audio Output / Audio Input / MIDI tabs. Enumerates devices and
// MIDI ports on open + a Refresh button. Edits the Preferences in place and calls
// onChange() whenever a setting changes (the app persists on change).
class PreferencesPanel {
public:
    void draw(Preferences& prefs, const std::function<void()>& onChange, bool* open);
private:
    void refresh();
    struct Dev { std::string id, name; };
    std::vector<Dev>         outDevices_, inDevices_;
    std::vector<std::string> midiIns_, midiOuts_;
    bool loaded_ = false;
};

} // namespace oss
