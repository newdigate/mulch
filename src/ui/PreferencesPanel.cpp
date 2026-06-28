#include "ui/PreferencesPanel.h"
#include "core/Preferences.h"
#include "ui/FileDialog.h"
#include <imgui.h>
#include <soundio/soundio.h>
#include <RtMidi.h>
#include <string>

namespace oss {

namespace {
struct Res { int w, h; const char* label; };
const Res kResolutions[] = {
    {320, 240, "320 x 240"}, {640, 480, "640 x 480"},
    {1280, 720, "1280 x 720"}, {1920, 1080, "1920 x 1080"},
};
}

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
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderInt("Audio buffer (ms)", &prefs.audioBufferMs, 20, 500)) onChange();
            ImGui::TextDisabled("Higher = more under-run headroom + latency; lower = tighter latency.");
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
        if (ImGui::BeginTabItem("Video")) {
            int curW = prefs.textureWidth, curH = prefs.textureHeight;
            std::string cur = std::to_string(curW) + " x " + std::to_string(curH);
            for (const Res& r : kResolutions) if (r.w == curW && r.h == curH) cur = r.label;
            if (ImGui::BeginCombo("Streaming texture size", cur.c_str())) {
                for (const Res& r : kResolutions) {
                    bool sel = (r.w == curW && r.h == curH);
                    if (ImGui::Selectable(r.label, sel)) { prefs.textureWidth = r.w; prefs.textureHeight = r.h; onChange(); }
                }
                ImGui::EndCombo();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sync")) {
            const char* modes[] = { "Off", "Beat Clock", "MTC" };
            auto portCombo = [&](const char* label, std::vector<std::string>& ports, std::string& cur) {
                if (ImGui::BeginCombo(label, cur.empty() ? "None" : cur.c_str())) {
                    if (ImGui::Selectable("None", cur.empty())) { cur.clear(); onChange(); }
                    for (const std::string& nm : ports) {
                        bool sel = (nm == cur);
                        if (ImGui::Selectable((nm + std::string("##") + label).c_str(), sel)) { cur = nm; onChange(); }
                    }
                    ImGui::EndCombo();
                }
            };
            ImGui::TextUnformatted("Receive (slave)");
            if (ImGui::Combo("In mode", &prefs.syncInMode, modes, 3)) onChange();
            portCombo("Sync source", midiIns_, prefs.syncInSource);
            ImGui::Separator();
            ImGui::TextUnformatted("Send (master)");
            if (ImGui::Combo("Out mode", &prefs.syncOutMode, modes, 3)) onChange();
            portCombo("Sync destination", midiOuts_, prefs.syncOutDest);
            if (prefs.syncInMode == 2 || prefs.syncOutMode == 2) {
                ImGui::Separator();
                const char* rates[] = { "24", "25", "29.97 df", "30" };
                if (ImGui::Combo("Frame rate", &prefs.syncFrameRate, rates, 4)) onChange();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Locations")) {
            auto folderRow = [&](const char* label, std::string& dir) {
                ImGui::TextUnformatted(label);
                ImGui::SameLine(160.0f);
                ImGui::TextUnformatted(dir.empty() ? "(OS default)" : dir.c_str());
                ImGui::SameLine();
                ImGui::PushID(label);
                if (ImGui::SmallButton("Browse...")) {
                    std::string picked = pickFolderDialog(label, dir);
                    if (!picked.empty()) { dir = picked; if (onChange) onChange(); }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear")) { dir.clear(); if (onChange) onChange(); }
                ImGui::PopID();
            };
            folderRow("Projects folder",      prefs.projectsDir);
            folderRow("Asset library folder", prefs.assetLibraryDir);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace oss
