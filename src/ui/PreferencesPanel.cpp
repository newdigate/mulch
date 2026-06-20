#include "ui/PreferencesPanel.h"
#include "core/Preferences.h"
#include <imgui.h>
#include <soundio/soundio.h>
#include <RtMidi.h>

namespace oss {

void PreferencesPanel::refresh() {
    outDevices_.clear(); inDevices_.clear(); midiIns_.clear(); midiOuts_.clear();

    SoundIo* sio = soundio_create();
    if (sio && soundio_connect(sio) == 0) {
        soundio_flush_events(sio);
        int no = soundio_output_device_count(sio);
        for (int i = 0; i < no; ++i) {
            SoundIoDevice* d = soundio_get_output_device(sio, i);
            if (d && !d->is_raw) outDevices_.push_back({d->id ? d->id : "", d->name ? d->name : "?"});
            if (d) soundio_device_unref(d);
        }
        int ni = soundio_input_device_count(sio);
        for (int i = 0; i < ni; ++i) {
            SoundIoDevice* d = soundio_get_input_device(sio, i);
            if (d && !d->is_raw) inDevices_.push_back({d->id ? d->id : "", d->name ? d->name : "?"});
            if (d) soundio_device_unref(d);
        }
    }
    if (sio) soundio_destroy(sio);

    try { RtMidiIn  mi; unsigned int n = mi.getPortCount(); for (unsigned int i = 0; i < n; ++i) midiIns_.push_back(mi.getPortName(i)); } catch (...) {}
    try { RtMidiOut mo; unsigned int n = mo.getPortCount(); for (unsigned int i = 0; i < n; ++i) midiOuts_.push_back(mo.getPortName(i)); } catch (...) {}
}

void PreferencesPanel::draw(Preferences& prefs, const std::function<void()>& onChange, bool* open) {
    if (!open || !*open) return;
    if (!loaded_) { refresh(); loaded_ = true; }

    if (!ImGui::Begin("Preferences", open)) { ImGui::End(); return; }

    if (ImGui::Button("Refresh devices")) refresh();

    auto deviceCombo = [&](const char* label, std::vector<Dev>& devs, std::string& curId) {
        std::string cur = "System default";
        if (!curId.empty()) for (const Dev& d : devs) if (d.id == curId) cur = d.name;
        if (ImGui::BeginCombo(label, cur.c_str())) {
            if (ImGui::Selectable("System default", curId.empty())) { curId.clear(); onChange(); }
            for (const Dev& d : devs) {
                bool sel = (d.id == curId);
                if (ImGui::Selectable((d.name + "##" + d.id).c_str(), sel)) { curId = d.id; onChange(); }
            }
            ImGui::EndCombo();
        }
    };

    if (ImGui::BeginTabBar("prefs_tabs")) {
        if (ImGui::BeginTabItem("Audio Output")) {
            deviceCombo("Output device", outDevices_, prefs.audioOutputDeviceId);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio Input")) {
            deviceCombo("Input device", inDevices_, prefs.audioInputDeviceId);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("MIDI")) {
            ImGui::TextUnformatted("Inputs");
            for (const std::string& name : midiIns_) {
                bool on = prefs.midiInputEnabled(name);
                if (ImGui::Checkbox((name + "##in").c_str(), &on)) { prefs.setMidiInputEnabled(name, on); onChange(); }
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Outputs");
            for (const std::string& name : midiOuts_) {
                bool on = prefs.midiOutputEnabled(name);
                if (ImGui::Checkbox((name + "##out").c_str(), &on)) { prefs.setMidiOutputEnabled(name, on); onChange(); }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace oss
