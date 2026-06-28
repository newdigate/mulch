#pragma once
#include <functional>
#include <string>

namespace oss {

struct Transport;

// Project Save / Save As / Load controls drawn at the right of the transport bar. When null,
// the bar shows only transport controls. Each callback is invoked when its button is clicked.
struct ProjectBarIO {
    std::function<void()> onSave;       // write the current file (or prompt if untitled)
    std::function<void()> onSaveAs;     // always prompt for a destination
    std::function<void()> onLoad;       // prompt for a file to open
    std::function<void()> onLibOpen;    // Asset Library > Open
    std::function<void()> onLibSave;    // Asset Library > Save
    std::function<void()> onLibSaveAs;  // Asset Library > Save As
    std::function<void()> onLibRemap;   // Asset Library > Remap Directory
    std::string status;                 // shown after the buttons
    bool*       showPreferences = nullptr;   // toggled by the View > Preferences item (if non-null)
    bool*       showAssets      = nullptr;   // toggled by the View > Assets item (if non-null)
};

// Draws the transport toolbar (Play/Pause, Stop, Rewind, Fast-forward, tempo, loop,
// position read-out) as a top main-menu bar; when `io` is non-null also draws
// Save / Save As / Load buttons + status. Mutates `t` in place. Call inside an
// active ImGui frame.
void drawTransportBar(Transport& t, ProjectBarIO* io = nullptr);

} // namespace oss
