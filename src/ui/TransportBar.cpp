#include "ui/TransportBar.h"
#include "core/Transport.h"
#include <imgui.h>
#include <algorithm>

namespace oss {

void drawTransportBar(Transport& t, ProjectBarIO* io) {
    // A menu bar lays its items out horizontally, so the buttons, tempo field,
    // and read-out all sit on one row without explicit SameLine calls.
    if (!ImGui::BeginMainMenuBar()) return;

    // Window toggles live in a left-anchored "View" menu so they stay reachable at any
    // window width: the menu bar lays everything on one non-wrapping row, so buttons at
    // the far right clip off-screen on a narrow window (the Assets/Prefs toggles did).
    // A left menu never clips. The bool* MenuItem overload shows a checkmark for the
    // current state and flips it on click.
    if (io && (io->showPreferences || io->showAssets)) {
        if (ImGui::BeginMenu("View")) {
            if (io->showPreferences) ImGui::MenuItem("Preferences", nullptr, io->showPreferences);
            if (io->showAssets)      ImGui::MenuItem("Assets",      nullptr, io->showAssets);
            ImGui::EndMenu();
        }
    }

    if (t.playing) { if (ImGui::Button("Pause")) t.pause(); }
    else           { if (ImGui::Button("Play"))  t.play();  }
    if (ImGui::Button("Stop")) t.stop();          // pause + return to the start
    if (ImGui::Button("<<"))   t.rewindBar();      // back one bar (clamps at 0)
    if (ImGui::Button(">>"))   t.forwardBar();     // forward one bar

    ImGui::TextUnformatted("  Tempo");
    ImGui::SetNextItemWidth(70.0f);
    float bpm = (float)t.bpm;
    // Decimal tempo; commit on edit and clamp to a sane musical range.
    if (ImGui::InputFloat("##bpm", &bpm, 0.0f, 0.0f, "%.2f"))
        t.bpm = std::clamp((double)bpm, 20.0, 999.0);
    ImGui::TextUnformatted("BPM");

    // Position: bars.beats (musical), beats, and clock time with milliseconds.
    int    mins = (int)(t.seconds / 60.0);
    double rem  = t.seconds - mins * 60.0;
    ImGui::Text("   Bar %d.%d   |   %.2f beats   |   %d:%06.3f   (%.0f ms)   |  ",
                t.barNumber(), t.beatInBar(), t.beats(), mins, rem, t.millis());

    // Loop: a toggle button (highlighted when on) and editable start/end in bars.
    // Capture `looping` BEFORE the button: clicking it calls toggleLoop(), which
    // flips t.looping mid-widget. Gating the push on the pre-click value and the pop
    // on the post-click value would push/pop unequal counts and trip ImGui's
    // "PopStyleColor() too many times" assert. One captured flag keeps them balanced.
    const bool loopHighlighted = t.looping;
    if (loopHighlighted) ImGui::PushStyleColor(ImGuiCol_Button, (ImU32)IM_COL32(170, 110, 40, 255));
    if (ImGui::Button("Loop")) t.toggleLoop();
    if (loopHighlighted) ImGui::PopStyleColor();

    ImGui::SetNextItemWidth(46.0f);
    float ls = (float)t.loopStartBar;
    if (ImGui::InputFloat("##loopStart", &ls, 0.0f, 0.0f, "%.2f")) t.loopStartBar = std::max(0.0f, ls);
    ImGui::TextUnformatted("-");
    ImGui::SetNextItemWidth(46.0f);
    float le = (float)t.loopEndBar;
    if (ImGui::InputFloat("##loopEnd", &le, 0.0f, 0.0f, "%.2f")) t.loopEndBar = le;
    ImGui::TextUnformatted("bars");

    if (io) {
        ImGui::Separator();
        ImGui::SetNextItemWidth(160.0f);
        if (io->filename) ImGui::InputText("##projfile", io->filename, io->filenameLen);
        if (ImGui::Button("Save") && io->onSave) io->onSave();
        ImGui::SameLine();
        if (ImGui::Button("Load") && io->onLoad) io->onLoad();
        // (Preferences/Assets toggles moved to the left-anchored "View" menu above.)
        if (!io->status.empty()) { ImGui::SameLine(); ImGui::TextUnformatted(io->status.c_str()); }
    }

    ImGui::EndMainMenuBar();
}

} // namespace oss
