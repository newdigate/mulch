#include "ui/DockLayout.h"
#include <imgui_internal.h>   // DockBuilder* API

namespace oss {

ImGuiID beginDockHost() {
    // PassthruCentralNode keeps the host transparent (no visible chrome behind the panels).
    return ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                        ImGuiDockNodeFlags_PassthruCentralNode);
}

void buildDefaultDockLayout(ImGuiID dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID mainId = dockspaceId, bottomId = 0, rightId = 0;
    bottomId = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Down,  0.30f, nullptr, &mainId);
    rightId  = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Right, 0.25f, nullptr, &mainId);

    ImGui::DockBuilderDockWindow("Node Graph",  mainId);
    ImGui::DockBuilderDockWindow("Automation",  bottomId);
    ImGui::DockBuilderDockWindow("Assets",      rightId);
    ImGui::DockBuilderDockWindow("Preferences", rightId);   // tabbed with Assets
    ImGui::DockBuilderFinish(dockspaceId);
}

} // namespace oss
