#pragma once
#include <imgui.h>

namespace oss {

// Submit the invisible full-viewport host dockspace (sits below the main menu bar and
// behind all panels) and return its dockspace id. Call once per frame, after the menu
// bar and before the dockable panels' Begin() calls.
ImGuiID beginDockHost();

// (Re)build the crafted default layout for `dockspaceId`, docking the panels by their
// window titles: Node Graph in the centre, Automation across the bottom, Assets +
// Preferences tabbed on the right. Overrides any existing layout for this id.
void buildDefaultDockLayout(ImGuiID dockspaceId);

} // namespace oss
