#include "ui/AssetsPanel.h"
#include "ui/FileDialog.h"
#include "core/AssetLibrary.h"
#include <imgui.h>
#include <cfloat>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace oss {

namespace {

// Filename basename, without directory or extension -> a default label seed.
std::string baseLabel(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) name = name.substr(0, dot);  // keep dotfiles like ".vimrc" intact
    return name;
}

void drawTab(AssetLibrary& lib, AssetType type, const char* noun,
             const std::vector<std::string>& filters) {
    int toRemove = -1;
    if (ImGui::BeginTable("##assettbl", 3,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Path");
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();

        // byType() pointers stay valid for the loop: editing a label/path mutates a string
        // in place (no vector realloc); add/remove are deferred until after EndTable.
        for (const Asset* a : lib.byType(type)) {
            int id = a->id;
            ImGui::PushID(id);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            char lbl[512];
            std::snprintf(lbl, sizeof(lbl), "%s", a->label.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##label", lbl, sizeof(lbl))) lib.setLabel(id, lbl);

            ImGui::TableSetColumnIndex(1);
            char pth[512];
            std::snprintf(pth, sizeof(pth), "%s", a->path.c_str());
            ImGui::SetNextItemWidth(-30.0f);
            if (ImGui::InputText("##path", pth, sizeof(pth))) lib.setPath(id, pth);
            ImGui::SameLine();
            if (ImGui::Button("...")) {
                std::string picked = openFileDialog(noun, filters);
                if (!picked.empty()) {
                    lib.setPath(id, picked);
                    const Asset* cur = lib.find(id);
                    if (cur && cur->label.empty()) lib.setLabel(id, baseLabel(picked));
                }
            }

            ImGui::TableSetColumnIndex(2);
            if (ImGui::Button("x")) toRemove = id;

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    std::string addLabel = std::string("+ Add ") + noun;
    if (ImGui::Button(addLabel.c_str())) lib.add(type, "", "");
    if (toRemove >= 0) lib.remove(toRemove);
}

} // namespace

void AssetsPanel::draw(AssetLibrary& lib, bool* open) {
    if (!open || !*open) return;
    if (!ImGui::Begin("Assets", open)) { ImGui::End(); return; }

    if (ImGui::BeginTabBar("assets_tabs")) {
        if (ImGui::BeginTabItem("Audio")) {
            drawTab(lib, AssetType::Audio, "audio file",
                    {"mp3", "wav", "flac", "m4a", "ogg", "aac", "aiff"});
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Video")) {
            drawTab(lib, AssetType::Video, "video file",
                    {"mp4", "mov", "mkv", "avi", "webm"});
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("MIDI")) {
            drawTab(lib, AssetType::Midi, "MIDI file", {"mid", "midi"});
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("3D")) {
            drawTab(lib, AssetType::Mesh, "3D model", {"obj", "gltf", "glb"});
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace oss
