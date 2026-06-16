#include "ui/TransportBar.h"
#include "core/Transport.h"
#include <imgui.h>
#include <algorithm>

namespace oss {

void drawTransportBar(Transport& t) {
    // A menu bar lays its items out horizontally, so the buttons, tempo field,
    // and read-out all sit on one row without explicit SameLine calls.
    if (!ImGui::BeginMainMenuBar()) return;

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
    ImGui::Text("   Bar %d.%d   |   %.2f beats   |   %d:%06.3f   (%.0f ms)",
                t.barNumber(), t.beatInBar(), t.beats(), mins, rem, t.millis());

    ImGui::EndMainMenuBar();
}

} // namespace oss
