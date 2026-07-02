#include "ui/AssetsPanel.h"
#include "ui/FileDialog.h"
#include "core/AssetLibrary.h"
#include "core/AssetTree.h"
#include "core/PathPrefix.h"
#include "core/PathUtil.h"
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

// Depth-first leaf ids in render order (folders before files at each level) -> drives Shift-range.
void flattenLeaves(const AssetTreeNode& node, std::vector<int>& out) {
    for (const AssetTreeNode& f : node.folders) flattenLeaves(f, out);
    for (const Asset* a : node.files) out.push_back(a->id);
}

// Index of `id` in the flattened leaf order, or -1 if absent.
int indexOfLeaf(const std::vector<int>& order, int id) {
    for (std::size_t k = 0; k < order.size(); ++k) if (order[k] == id) return (int)k;
    return -1;
}

} // namespace

void AssetsPanel::drawTab(AssetLibrary& lib, AssetType type, const char* noun,
                          const std::vector<std::string>& filters) {
    std::set<std::string>& filterSet = filter_[(int)type];
    std::set<int>&         selSet    = selected_[(int)type];
    int&                   anchor    = anchor_[(int)type];

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
        ImGui::TextDisabled("(click=select, Ctrl/Cmd=toggle, Shift=range; double-click a name to rename)");
    }

    // Filtered rows -> folder tree -> flattened leaf order (drives Shift-range).
    std::vector<const Asset*> rows;
    for (const Asset* a : lib.byType(type)) {
        if (!filterSet.empty()) {
            bool match = false;
            for (const std::string& t : a->tags) if (filterSet.count(t)) { match = true; break; }
            if (!match) continue;
        }
        rows.push_back(a);
    }
    AssetTreeNode tree = buildAssetTree(rows);
    std::vector<int> leafOrder;
    flattenLeaves(tree, leafOrder);
    const bool singleRoot = tree.folders.size() == 1;

    int toRemove = -1;
    if (ImGui::BeginTable("##assettbl", 4,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Tags", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();

        drawTreeNode(lib, tree, /*rootLevel*/true, singleRoot, selSet, anchor, leafOrder,
                     noun, filters, toRemove);

        ImGui::EndTable();
    }

    std::string addLabel = std::string("+ Add ") + noun;
    if (ImGui::Button(addLabel.c_str())) lib.add(type, "", "");
    ImGui::SameLine();
    if (ImGui::Button("Add files...")) {
        for (const std::string& path : openMultipleFileDialog(noun, "Media", filters, mediaDir_))
            lib.add(type, baseLabel(path), path);
    }
    if (toRemove >= 0) {
        lib.remove(toRemove);
        addText_.erase(toRemove);
        selSet.erase(toRemove);
        if (anchor == toRemove)       anchor = -1;        // anchor's row is gone
        if (renamingId_ == toRemove)  renamingId_ = -1;   // stop renaming a removed row
    }
}

void AssetsPanel::drawTreeNode(AssetLibrary& lib, const AssetTreeNode& node, bool rootLevel,
                               bool singleRoot, std::set<int>& selSet, int& anchor,
                               const std::vector<int>& leafOrder, const char* noun,
                               const std::vector<std::string>& filters, int& toRemove) {
    for (const AssetTreeNode& f : node.folders) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth;
        if (rootLevel && singleRoot) flags |= ImGuiTreeNodeFlags_DefaultOpen;   // open the lone root
        // The label IS the node id: the joined path is unique per tree position and stable across
        // the per-frame rebuild, so ImGui remembers each folder's open/closed state.
        bool open = ImGui::TreeNodeEx(f.name.c_str(), flags);                   // cols 1-3 stay empty
        if (open) {
            drawTreeNode(lib, f, /*rootLevel*/false, singleRoot, selSet, anchor, leafOrder,
                         noun, filters, toRemove);
            ImGui::TreePop();
        }
    }
    for (const Asset* a : node.files)
        drawAssetLeaf(lib, a, selSet, anchor, leafOrder, noun, filters, toRemove);
}

void AssetsPanel::drawAssetLeaf(AssetLibrary& lib, const Asset* a, std::set<int>& selSet,
                                int& anchor, const std::vector<int>& leafOrder, const char* noun,
                                const std::vector<std::string>& filters, int& toRemove) {
    const ImGuiIO& io = ImGui::GetIO();
    int id = a->id;
    ImGui::PushID(id);
    ImGui::TableNextRow();
    const bool isSel = selSet.count(id) > 0;
    if (isSel)
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                               ImGui::GetColorU32(ImVec4(0.26f, 0.45f, 0.62f, 0.45f)));

    // --- Column 0: the leaf node (selection + double-click rename), or the inline rename field. ---
    ImGui::TableSetColumnIndex(0);
    if (renamingId_ == id) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (renameFocus_) { ImGui::SetKeyboardFocusHere(); renameFocus_ = false; }
        bool enter = ImGui::InputText("##rename", renameBuf_, sizeof(renameBuf_),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter) {                                          // Enter commits
            lib.setLabel(id, renameBuf_); renamingId_ = -1;
        } else if (ImGui::IsItemDeactivated()) {              // focus left the field
            if (!ImGui::IsKeyPressed(ImGuiKey_Escape)) lib.setLabel(id, renameBuf_);  // click-away commits; Esc cancels
            renamingId_ = -1;
        }
    } else {
        std::string base = fileBaseName(a->path);
        std::string disp = !a->label.empty() ? a->label
                         : (!base.empty() ? base : std::string("(unnamed)"));
        ImGui::TreeNodeEx(disp.c_str(),
            ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
            (isSel ? ImGuiTreeNodeFlags_Selected : 0));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            renamingId_ = id;
            std::snprintf(renameBuf_, sizeof(renameBuf_), "%s", a->label.c_str());
            renameFocus_ = true;
        } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (io.KeyShift && anchor != -1) {                // range over the visible leaves
                int ai = indexOfLeaf(leafOrder, anchor), yi = indexOfLeaf(leafOrder, id);
                if (ai >= 0 && yi >= 0) {
                    int lo = ai < yi ? ai : yi, hi = ai < yi ? yi : ai;
                    for (int k = lo; k <= hi; ++k) selSet.insert(leafOrder[(std::size_t)k]);
                } else { selSet.clear(); selSet.insert(id); }                       // stale anchor -> single
            } else if (io.KeyCtrl || io.KeySuper) {           // toggle one
                if (isSel) selSet.erase(id); else selSet.insert(id);
            } else {                                          // plain click -> select only this
                selSet.clear(); selSet.insert(id);
            }
            anchor = id;
        }
    }

    // --- Column 1: path + Browse. ---
    ImGui::TableSetColumnIndex(1);
    char pth[512];
    std::snprintf(pth, sizeof(pth), "%s", a->path.c_str());
    ImGui::SetNextItemWidth(-30.0f);
    if (ImGui::InputText("##path", pth, sizeof(pth))) lib.setPath(id, pth);
    ImGui::SameLine();
    if (ImGui::Button("...")) {
        std::string picked = openFileDialog(noun, "Media", filters, mediaDir_);
        if (!picked.empty()) {
            lib.setPath(id, picked);
            const Asset* cur = lib.find(id);
            if (cur && cur->label.empty()) lib.setLabel(id, baseLabel(picked));
        }
    }

    // --- Column 2: tag chips (+ remove) and an add box; both broadcast to a selection. ---
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

    // --- Column 3: remove. ---
    ImGui::TableSetColumnIndex(3);
    if (ImGui::Button("x")) toRemove = id;

    ImGui::PopID();
}

void AssetsPanel::drawRemapModal(AssetLibrary& lib) {
    if (showRemap_) { ImGui::OpenPopup("Remap Directory"); showRemap_ = false; }
    if (ImGui::BeginPopupModal("Remap Directory", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!remapPrimed_) {                              // pre-fill From with the common dir prefix
            std::vector<std::string> paths;
            for (const Asset& a : lib.all()) if (!a.path.empty()) paths.push_back(a.path);
            std::string common = commonDirPrefix(paths);
            std::snprintf(remapFrom_, sizeof(remapFrom_), "%s", common.c_str());
            remapTo_[0] = '\0';
            remapResult_.clear();
            remapPrimed_ = true;
        }
        ImGui::TextUnformatted("Replace a base directory across every asset path.");
        ImGui::SetNextItemWidth(420.0f);
        ImGui::InputText("##from", remapFrom_, sizeof(remapFrom_));
        ImGui::SameLine();
        if (ImGui::Button("Browse##from")) {
            std::string p = pickFolderDialog("Remap from", mediaDir_.empty() ? std::string(remapFrom_) : mediaDir_);
            if (!p.empty()) std::snprintf(remapFrom_, sizeof(remapFrom_), "%s", p.c_str());
        }
        ImGui::TextUnformatted("From (old base)");
        ImGui::SetNextItemWidth(420.0f);
        ImGui::InputText("##to", remapTo_, sizeof(remapTo_));
        ImGui::SameLine();
        if (ImGui::Button("Browse##to")) {
            std::string p = pickFolderDialog("Remap to", mediaDir_);
            if (!p.empty()) std::snprintf(remapTo_, sizeof(remapTo_), "%s", p.c_str());
        }
        ImGui::TextUnformatted("To (new base)");
        if (!remapResult_.empty()) { ImGui::Separator(); ImGui::TextUnformatted(remapResult_.c_str()); }
        ImGui::Separator();
        if (ImGui::Button("Apply")) {
            int n = lib.remapPathPrefix(remapFrom_, remapTo_);
            remapResult_ = "remapped " + std::to_string(n) + " asset" + (n == 1 ? "" : "s");
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) { remapPrimed_ = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void AssetsPanel::draw(AssetLibrary& lib, bool* open, const std::string& mediaDir) {
    mediaDir_ = mediaDir;
    drawRemapModal(lib);            // render before the open-guard so the menu's Remap works when the window is closed
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
        if (ImGui::BeginTabItem("Image")) {
            drawTab(lib, AssetType::Image, "image file",
                    {"png", "jpg", "jpeg", "bmp", "tga", "gif", "hdr", "psd"});
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
