# Multi-select + Broadcast Tagging in the Assets Grid — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ctrl/Cmd-click (toggle) and Shift-click (range) to multi-select rows in the Assets grid, and have a tag added/removed on a selected row apply to the whole selection.

**Architecture:** Entirely in `ui/AssetsPanel`. The panel gains per-tab `selected_` sets + an `anchor_`; holding a modifier renders rows as a read-only span-all-columns `Selectable` (so a click anywhere selects); in normal mode the tag add-box / chip-× broadcast to the selection via the existing `AssetLibrary::addTag`/`removeTag`. No core, persistence, or `.oss` change; no automated test (UI).

**Tech Stack:** C++17, Dear ImGui.

**Spec:** `docs/superpowers/specs/2026-06-28-asset-multiselect-tag-design.md`

---

## File Structure

| File | Change |
|---|---|
| `src/ui/AssetsPanel.h` | + `std::set<int> selected_[kAssetTypeCount]` + `int anchor_[kAssetTypeCount]`. |
| `src/ui/AssetsPanel.cpp` | `drawTab` rewrite: select-mode rows, selection highlight + status line, broadcast add/remove, prune on removal. |
| `CLAUDE.md`, `README.md` | docs. |

---

## Task 1: AssetsPanel multi-select + broadcast tagging

**Files:**
- Modify: `src/ui/AssetsPanel.h`, `src/ui/AssetsPanel.cpp`

No automated test (ImGui). Verified by build + a manual smoke check (DEFERRED — do not launch the GUI).

- [ ] **Step 1: Add the selection state to `AssetsPanel.h`**

In `src/ui/AssetsPanel.h`, add two members after the existing `addText_` line (inside the `private:` section):

```cpp
    std::set<int> selected_[kAssetTypeCount];                    // selected asset ids per media tab (transient)
    int           anchor_[kAssetTypeCount] = {-1, -1, -1, -1};  // Shift-range pivot (last-clicked id); kAssetTypeCount entries
```

(`<set>` is already included; `kAssetTypeCount` is 4, matching the four initializers.)

- [ ] **Step 2: Rewrite `drawTab` in `AssetsPanel.cpp`**

Replace the entire `AssetsPanel::drawTab(...)` function (the function only — leave `baseLabel`, `pushTagColors`, and `draw` as they are) with:

```cpp
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
```

- [ ] **Step 3: Build the app**

Run: `cmake --build build -j --target shader_streamer`
Expected: links cleanly. Then a full `cmake --build build -j && ctest --test-dir build --output-on-failure` to confirm nothing else broke (AssetsPanel isn't in the test targets, so this just re-confirms the rest).

- [ ] **Step 4: Manual smoke check (DEFERRED — do not launch the GUI as a subagent)**

The coordinator/user verifies: in **View → Assets**, hold Ctrl/Cmd and click rows (they highlight; "N selected" shows); Shift-click extends the range from the anchor; release the modifier, type a tag in a selected row's add-box → all selected rows gain it; click a chip's × on a selected row → all lose it; an unselected row tags only itself; **Clear selection** empties it; removing a row (×) drops it from the selection. Your verification is the clean build (Step 3) + re-reading the code.

- [ ] **Step 5: Commit**

```bash
git add src/ui/AssetsPanel.h src/ui/AssetsPanel.cpp
git commit -m "feat(ui): multi-select Assets rows (Ctrl/Cmd/Shift-click) + broadcast tagging"
```

---

## Task 2: Documentation

**Files:**
- Modify: `CLAUDE.md`, `README.md`

- [ ] **Step 1: Extend the CLAUDE.md Assets bullet**

In `CLAUDE.md`, find the **Assets / media library** bullet. Append this sentence at the end of the bullet (after the last sentence about the tag filter / persistence):

```
  Grid rows are multi-selectable — holding Ctrl/Cmd/Shift renders the rows as read-only span-all-columns
  `Selectable`s (Ctrl/Cmd-click toggles, Shift-click ranges from an `anchor_`); adding/removing a tag on a
  selected row then broadcasts to the whole selection. `selected_`/`anchor_` are transient panel state.
```

- [ ] **Step 2: Extend the README Assets subsection**

In `README.md`, in the **### Assets** subsection, append this sentence to the end of the paragraph:

```markdown
Hold **Ctrl/Cmd** and click rows (or **Shift**-click a range) to select several at once — then typing a
tag in any selected row (or removing one) applies it to every selected row.
```

- [ ] **Step 3: Verify only docs changed**

Run: `git diff --stat`
Expected: only `CLAUDE.md` and `README.md` changed.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md README.md
git commit -m "docs: multi-select + broadcast tagging in the Assets grid"
```

---

## Final verification (after all tasks)

- [ ] `cmake --build build -j && ctest --test-dir build --output-on-failure` — `shader_streamer` links; `core_tests` + `gl_smoke` pass.
- [ ] Manual: launch `./build/shader_streamer` — Ctrl/Cmd/Shift-select rows in an Assets tab, broadcast-tag them, verify highlight + "N selected" + Clear, and removal pruning.
