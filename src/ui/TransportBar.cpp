#include "ui/TransportBar.h"
#include "core/Transport.h"
#include <imgui.h>
#include <algorithm>

namespace oss {

void drawTransportBar(Transport& t, ProjectBarIO* io) {
    // A menu bar lays its items out horizontally, so the buttons, tempo field,
    // and read-out all sit on one row without explicit SameLine calls.
    if (!ImGui::BeginMainMenuBar()) return;

    // Project actions in a left-anchored "File" menu (drawn first, so it sits to the left of
    // the "View" menu); a left menu never clips off a narrow window.
    if (io && (io->onLoad || io->onSave || io->onSaveAs)) {
        if (ImGui::BeginMenu("File")) {
            if (io->onLoad   && ImGui::MenuItem("Load..."))    io->onLoad();
            ImGui::Separator();
            if (io->onSave   && ImGui::MenuItem("Save"))       io->onSave();
            if (io->onSaveAs && ImGui::MenuItem("Save As...")) io->onSaveAs();
            ImGui::EndMenu();
        }
    }

    // Asset Library actions in their own left-anchored menu (between File and View).
    if (io && (io->onLibOpen || io->onLibSave || io->onLibSaveAs || io->onLibRemap)) {
        if (ImGui::BeginMenu("Asset Library")) {
            if (io->onLibOpen   && ImGui::MenuItem("Open Asset Library...")) io->onLibOpen();
            ImGui::Separator();
            if (io->onLibSave   && ImGui::MenuItem("Save"))                  io->onLibSave();
            if (io->onLibSaveAs && ImGui::MenuItem("Save As..."))            io->onLibSaveAs();
            ImGui::Separator();
            if (io->onLibRemap  && ImGui::MenuItem("Remap Directory..."))    io->onLibRemap();
            ImGui::EndMenu();
        }
    }

    // Window toggles live in a left-anchored "View" menu so they stay reachable at any
    // window width: the menu bar lays everything on one non-wrapping row, so buttons at
    // the far right clip off-screen on a narrow window (the Assets/Prefs toggles did).
    // A left menu never clips. The bool* MenuItem overload shows a checkmark for the
    // current state and flips it on click.
    if (io && (io->showPreferences || io->showAssets || io->onResetLayout)) {
        if (ImGui::BeginMenu("View")) {
            if (io->showPreferences) ImGui::MenuItem("Preferences", nullptr, io->showPreferences);
            if (io->showAssets)      ImGui::MenuItem("Assets",      nullptr, io->showAssets);
            if (io->onResetLayout) {
                ImGui::Separator();
                if (ImGui::MenuItem("Reset Layout")) io->onResetLayout();
            }
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

    // Project Save/Load actions moved to the left-anchored "File" menu above; the status
    // (e.g. "saved foo.oss") stays on the right.
    if (io && !io->status.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted(io->status.c_str());
    }

    ImGui::EndMainMenuBar();
}

} // namespace oss
