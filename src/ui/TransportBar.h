#pragma once
#include <cstddef>
#include <functional>
#include <string>

namespace oss {

struct Transport;

// Optional Save/Load controls drawn at the right of the transport bar. When null,
// the bar shows only transport controls.
struct ProjectBarIO {
    char*       filename = nullptr;     // editable filename buffer
    std::size_t filenameLen = 0;
    std::function<void()> onSave;
    std::function<void()> onLoad;
    std::string status;                 // shown after the buttons
    bool*       showPreferences = nullptr;   // toggled by the Prefs button (if non-null)
    bool*       showAssets = nullptr;        // toggled by the Assets button (if non-null)
};

// Draws the transport toolbar (Play/Pause, Stop, Rewind, Fast-forward, tempo, loop,
// position read-out) as a top main-menu bar; when `io` is non-null also draws a
// filename field + Save/Load buttons + status. Mutates `t` in place. Call inside an
// active ImGui frame.
void drawTransportBar(Transport& t, ProjectBarIO* io = nullptr);

} // namespace oss
