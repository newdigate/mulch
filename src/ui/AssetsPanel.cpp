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

// Push the 3 button-state colors tinted with `c` (alpha distinguishes selected/unselected).
void pushTagColors(const glm::vec4& c, float baseAlpha) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(c.x, c.y, c.z, baseAlpha));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x, c.y, c.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(c.x, c.y, c.z, 1.0f));
}

} // namespace

void AssetsPanel::drawTab(AssetLibrary& lib, AssetType type, const char* noun,
                          const std::vector<std::string>& filters) {
    std::set<std::string>& filterSet = filter_[(int)type];
    std::set<int>&         selSet    = selected_[(int)type];
    int&                   anchor    = anchor_[(int)type];
    const ImGuiIO&         io        = ImGui::GetIO();
    const bool             selecting = io.KeyCtrl || io.KeySuper || io.KeyShift;

    // --- Tag-filter toolbar: this tab's tags as colored toggle buttons (left-click filter,
    //     right-click recolor). ---
    std::vector<std::string> tabTags = lib.tagsForType(type);
    for (std::size_t i = 0; i < tabTags.size(); ++i) {
        const std::string& tag = tabTags[i];
        if (i) ImGui::SameLine();
        ImGui::PushID(tag.c_str());
        const bool selected = filterSet.count(tag) > 0;
        pushTagColors(lib.tagColor(tag), selected ? 1.0f : 0.45f);
        if (ImGui::SmallButton(tag.c_str())) {
            if (selected) filterSet.erase(tag); else filterSet.insert(tag);
        }
        ImGui::PopStyleColor(3);
        if (ImGui::BeginPopupContextItem("##recolor")) {     // right-click a tag -> color picker
            glm::vec4 c = lib.tagColor(tag);
            if (ImGui::ColorPicker4("##pick", &c.x, ImGuiColorEditFlags_NoAlpha))
                lib.setTagColor(tag, c);   // tag colors are RGB-only; alpha is unused
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (!filterSet.empty()) {
        if (!tabTags.empty()) ImGui::SameLine();
        if (ImGui::SmallButton("clear filter")) filterSet.clear();
    }

    // --- Selection status line: count + clear + a hint. ---
    if (!selSet.empty()) {
        ImGui::Text("%d selected", (int)selSet.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear selection")) { selSet.clear(); anchor = -1; }
        ImGui::SameLine();
        ImGui::TextDisabled("(Ctrl/Cmd-click toggles, Shift-click ranges; type a tag to apply to all)");
    }

    // Filtered, ordered visible rows -- drives both the Shift-range and rendering.
    std::vector<const Asset*> rows;
    for (const Asset* a : lib.byType(type)) {
        if (!filterSet.empty()) {
            bool match = false;
            for (const std::string& t : a->tags) if (filterSet.count(t)) { match = true; break; }
            if (!match) continue;
        }
        rows.push_back(a);
    }
    auto indexOf = [&rows](int id) -> int {
        for (std::size_t k = 0; k < rows.size(); ++k) if (rows[k]->id == id) return (int)k;
        return -1;
    };

    int toRemove = -1;
    if (ImGui::BeginTable("##assettbl", 4,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Path");
        ImGui::TableSetupColumn("Tags",  ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();

        for (const Asset* a : rows) {
            int id = a->id;
            ImGui::PushID(id);
            ImGui::TableNextRow();
            const bool isSel = selSet.count(id) > 0;
            if (isSel)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       ImGui::GetColorU32(ImVec4(0.26f, 0.45f, 0.62f, 0.45f)));

            // --- Selection mode (a modifier is held): the whole row is a read-only click target,
            //     so a click selects instead of editing a field. ---
            if (selecting) {
                ImGui::TableSetColumnIndex(0);
                std::string rl = !a->label.empty() ? a->label
                               : (!a->path.empty() ? a->path : std::string("(unnamed)"));
                if (ImGui::Selectable((rl + "##rowsel").c_str(), isSel,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    if (io.KeyShift && anchor != -1 && indexOf(anchor) >= 0) {
                        int ai = indexOf(anchor), yi = indexOf(id);
                        int lo = ai < yi ? ai : yi, hi = ai < yi ? yi : ai;
                        for (int k = lo; k <= hi; ++k) selSet.insert(rows[(std::size_t)k]->id);  // add range
                    } else if (isSel) {
                        selSet.erase(id);                                                        // toggle off
                    } else {
                        selSet.insert(id);                                                       // toggle on
                    }
                    anchor = id;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(a->path.c_str());
                ImGui::TableSetColumnIndex(2);
                for (std::size_t ti = 0; ti < a->tags.size(); ++ti) {
                    if (ti) ImGui::SameLine();
                    glm::vec4 c = lib.tagColor(a->tags[ti]);
                    ImGui::TextColored(ImVec4(c.x, c.y, c.z, 1.0f), "%s", a->tags[ti].c_str());
                }
                ImGui::PopID();
                continue;   // skip the editable rendering this frame
            }

            // --- Edit mode: the normal editable row, with tag add/remove broadcasting to the
            //     selection when this row is part of it. ---
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
                std::string picked = openFileDialog(noun, "Media", filters);
                if (!picked.empty()) {
                    lib.setPath(id, picked);
                    const Asset* cur = lib.find(id);
                    if (cur && cur->label.empty()) lib.setLabel(id, baseLabel(picked));
                }
            }

            ImGui::TableSetColumnIndex(2);
            int tagToRemove = -1;
            for (std::size_t ti = 0; ti < a->tags.size(); ++ti) {
                const std::string& tag = a->tags[ti];
                ImGui::PushID((int)ti);
                pushTagColors(lib.tagColor(tag), 0.8f);
                if (ImGui::SmallButton((tag + " x").c_str())) tagToRemove = (int)ti;
                ImGui::PopStyleColor(3);
                ImGui::PopID();
                ImGui::SameLine();
            }
            std::string& s = addText_[id];
            char addbuf[64];
            std::snprintf(addbuf, sizeof(addbuf), "%s", s.c_str());
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::InputText("##addtag", addbuf, sizeof(addbuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (selSet.count(id)) { for (int sid : selSet) lib.addTag(sid, addbuf); }  // broadcast to selection
                else                    lib.addTag(id, addbuf);
                s.clear();
            } else {
                s = addbuf;                 // persist in-progress typing across frames
            }
            if (tagToRemove >= 0) {
                std::string tg = a->tags[(std::size_t)tagToRemove];   // copy before any mutation
                if (selSet.count(id)) { for (int sid : selSet) lib.removeTag(sid, tg); }   // broadcast
                else                    lib.removeTag(id, tg);
            }

            ImGui::TableSetColumnIndex(3);
            if (ImGui::Button("x")) toRemove = id;

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    std::string addLabel = std::string("+ Add ") + noun;
    if (ImGui::Button(addLabel.c_str())) lib.add(type, "", "");
    ImGui::SameLine();
    if (ImGui::Button("Add files...")) {
        for (const std::string& path : openMultipleFileDialog(noun, "Media", filters))
            lib.add(type, baseLabel(path), path);
    }
    if (toRemove >= 0) {
        lib.remove(toRemove);
        addText_.erase(toRemove);
        selSet.erase(toRemove);
        if (anchor == toRemove) anchor = -1;   // anchor's row is gone
    }
}

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
