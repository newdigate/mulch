#include "ui/AutomationPanel.h"
#include "core/Graph.h"
#include "modules/AutomationNode.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <string>

namespace oss {

static ImU32 channelColour(int c) {
    static const ImU32 cols[4] = {
        IM_COL32( 90, 200, 255, 255),   // cyan
        IM_COL32(255, 180,  80, 255),   // orange
        IM_COL32(140, 255, 120, 255),   // green
        IM_COL32(255, 120, 200, 255),   // pink
    };
    return cols[c & 3];
}

void AutomationPanel::draw(Graph& graph) {
    AutomationNode* node = nullptr;
    for (auto& up : graph.nodes())
        if (auto* a = dynamic_cast<AutomationNode*>(up.get())) { node = a; break; }

    ImGui::Begin("Automation");
    if (!node) {
        ImGui::TextDisabled("Add an Automation node to the graph to edit its channels.");
        ImGui::End();
        return;
    }

    // --- Toolbar ---
    ImGui::SetNextItemWidth(90.0f);
    int len = (int)node->lengthBars();
    if (ImGui::InputInt("Length (bars)", &len)) node->setLengthBars((float)len);
    ImGui::SameLine();
    ImGui::TextDisabled("one channel per row; top row reserved");

    // --- Grid: a fixed left header column + a horizontally-scrollable lane area ---
    const int   channels = node->channelCount();
    const int   rows     = 1 + channels;            // row 0 = reserved header
    const float rowH     = 52.0f;
    const float leftW    = 220.0f;
    const float pxPerBar = 90.0f;
    const float inset    = 7.0f;                     // lane vertical padding
    const float totalH   = rows * rowH;
    const float scrollH  = ImGui::GetStyle().ScrollbarSize;

    // Left column: per-row labels and controls (no scrolling).
    ImGui::BeginChild("autoLabels", ImVec2(leftW, totalH), false);
    for (int r = 0; r < rows; ++r) {
        float yTop = r * rowH;
        if (r == 0) {
            ImGui::SetCursorPos(ImVec2(8.0f, yTop + rowH * 0.5f - 8.0f));
            ImGui::TextDisabled("Reserved");
            continue;
        }
        int c = r - 1;
        ImGui::SetCursorPos(ImVec2(8.0f, yTop + 5.0f));
        ImGui::TextColored(ImColor(channelColour(c)), "ch %d", c + 1);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(74.0f);
        int ci = (int)node->category(c);
        const char* cats[] = { "stream", "ui" };
        if (ImGui::Combo(("##cat" + std::to_string(c)).c_str(), &ci, cats, 2))
            node->setCategory(c, (AutoCategory)ci);

        ImGui::SetCursorPos(ImVec2(8.0f, yTop + 28.0f));
        float lo = node->outMin(c), hi = node->outMax(c);
        ImGui::SetNextItemWidth(54.0f);
        if (ImGui::DragFloat(("##lo" + std::to_string(c)).c_str(), &lo, 0.01f, 0, 0, "%.2f"))
            node->setOutRange(c, lo, hi);
        ImGui::SameLine(); ImGui::SetNextItemWidth(54.0f);
        if (ImGui::DragFloat(("##hi" + std::to_string(c)).c_str(), &hi, 0.01f, 0, 0, "%.2f"))
            node->setOutRange(c, lo, hi);
        ImGui::SameLine();
        if (ImGui::SmallButton(("clr##" + std::to_string(c)).c_str())) node->channel(c).clear();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Lane area: shared time axis, scrolls horizontally. Extra height leaves room
    // for the horizontal scrollbar so the content rows don't also scroll vertically.
    ImGui::BeginChild("autoLanes", ImVec2(0, totalH + scrollH + 4.0f), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const float contentW = std::max(1.0f, node->lengthBars() * pxPerBar);
    ImGui::InvisibleButton("lanes", ImVec2(contentW, totalH));
    const bool   hovered = ImGui::IsItemHovered();
    const ImVec2 o = ImGui::GetItemRectMin();
    ImDrawList*  dl = ImGui::GetWindowDrawList();

    auto X      = [&](float bar) { return o.x + bar * pxPerBar; };
    auto rowTop = [&](int r)     { return o.y + r * rowH; };
    auto laneY  = [&](int r, float v) { float t = rowTop(r) + inset, b = rowTop(r) + rowH - inset; return b - v * (b - t); };
    auto toVal  = [&](int r, float py) { float t = rowTop(r) + inset, b = rowTop(r) + rowH - inset; return (b - py) / (b - t); };
    auto toBar  = [&](float px)  { return (px - o.x) / pxPerBar; };

    // Row backgrounds (alternating; reserved row tinted) + grid + separators.
    for (int r = 0; r < rows; ++r) {
        ImU32 bg = r == 0 ? IM_COL32(30, 32, 40, 255)
                          : (r & 1 ? IM_COL32(22, 24, 30, 255) : IM_COL32(18, 20, 26, 255));
        dl->AddRectFilled(ImVec2(o.x, rowTop(r)), ImVec2(o.x + contentW, rowTop(r) + rowH), bg);
    }
    for (int b = 0; b <= (int)node->lengthBars(); ++b) {
        float x = X((float)b);
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + totalH), IM_COL32(55, 58, 68, 255));
        char t[8]; std::snprintf(t, sizeof(t), "%d", b);
        dl->AddText(ImVec2(x + 3, rowTop(0) + rowH * 0.5f - 7.0f), IM_COL32(120, 128, 140, 255), t);
    }
    for (int r = 1; r < rows; ++r)
        dl->AddLine(ImVec2(o.x, rowTop(r)), ImVec2(o.x + contentW, rowTop(r)), IM_COL32(40, 42, 50, 255));

    // Each channel's curve + breakpoints in its own lane.
    for (int c = 0; c < channels; ++c) {
        int r = c + 1;
        auto& p = node->channel(c);
        ImU32 col = channelColour(c);
        if (!p.empty()) {
            dl->AddLine(ImVec2(o.x, laneY(r, p.front().value)), ImVec2(X(p.front().bar), laneY(r, p.front().value)), col, 2.0f);
            for (std::size_t i = 0; i + 1 < p.size(); ++i)
                dl->AddLine(ImVec2(X(p[i].bar), laneY(r, p[i].value)), ImVec2(X(p[i+1].bar), laneY(r, p[i+1].value)), col, 2.0f);
            dl->AddLine(ImVec2(X(p.back().bar), laneY(r, p.back().value)), ImVec2(o.x + contentW, laneY(r, p.back().value)), col, 2.0f);
            for (auto& pt : p) dl->AddCircleFilled(ImVec2(X(pt.bar), laneY(r, pt.value)), 4.0f, col);
        }
    }

    // Playhead across all lanes.
    float phx = X(std::clamp(node->currentBar(), 0.0f, node->lengthBars()));
    dl->AddLine(ImVec2(phx, o.y), ImVec2(phx, o.y + totalH), IM_COL32(255, 80, 80, 220), 2.0f);

    // --- Mouse editing (per row) ---
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int hoverRow = (int)((m.y - o.y) / rowH);
    auto hitPoint = [&](int c) -> int {
        int r = c + 1;
        auto& p = node->channel(c);
        for (int i = 0; i < (int)p.size(); ++i) {
            float dx = m.x - X(p[i].bar), dy = m.y - laneY(r, p[i].value);
            if (dx * dx + dy * dy <= 8.0f * 8.0f) return i;
        }
        return -1;
    };
    const bool inChannelRow = hovered && hoverRow >= 1 && hoverRow < rows;
    if (inChannelRow && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        int c = hoverRow - 1, hit = hitPoint(c);
        if (hit >= 0) node->channel(c).erase(node->channel(c).begin() + hit);
    }
    if (inChannelRow && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int c = hoverRow - 1, hit = hitPoint(c);
        auto& p = node->channel(c);
        if (hit >= 0) { dragChannel_ = c; dragPoint_ = hit; }
        else {
            AutoPoint np{ std::clamp(toBar(m.x), 0.0f, node->lengthBars()),
                          std::clamp(toVal(hoverRow, m.y), 0.0f, 1.0f) };
            auto it = std::lower_bound(p.begin(), p.end(), np,
                                       [](const AutoPoint& a, const AutoPoint& b) { return a.bar < b.bar; });
            dragChannel_ = c; dragPoint_ = (int)(it - p.begin());
            p.insert(it, np);
        }
    }
    if (dragChannel_ >= 0 && dragPoint_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        int c = dragChannel_, r = c + 1;
        auto& p = node->channel(c);
        if (dragPoint_ < (int)p.size()) {
            float nb = std::clamp(toBar(m.x), 0.0f, node->lengthBars());
            float nv = std::clamp(toVal(r, m.y), 0.0f, 1.0f);
            if (dragPoint_ > 0)                  nb = std::max(nb, p[dragPoint_ - 1].bar + 1e-4f);
            if (dragPoint_ + 1 < (int)p.size())  nb = std::min(nb, p[dragPoint_ + 1].bar - 1e-4f);
            p[dragPoint_].bar = nb; p[dragPoint_].value = nv;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { dragChannel_ = -1; dragPoint_ = -1; }

    ImGui::EndChild();
    ImGui::TextDisabled("Click a lane to add a point, drag to move, right-click to delete. "
                        "Red line = transport playhead.");
    ImGui::End();
}

} // namespace oss
