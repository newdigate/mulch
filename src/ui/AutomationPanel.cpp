#include "ui/AutomationPanel.h"
#include "core/Graph.h"
#include "modules/AutomationNode.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <string>

namespace oss {

static ImU32 channelColour(int c, bool active) {
    static const ImU32 cols[4] = {
        IM_COL32( 90, 200, 255, 255),   // cyan
        IM_COL32(255, 180,  80, 255),   // orange
        IM_COL32(140, 255, 120, 255),   // green
        IM_COL32(255, 120, 200, 255),   // pink
    };
    ImU32 base = cols[c & 3];
    return active ? base : ((base & 0x00FFFFFFu) | (70u << 24));   // dim inactive
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
    if (activeChannel_ >= node->channelCount()) activeChannel_ = 0;

    // --- Toolbar ---
    for (int c = 0; c < node->channelCount(); ++c) {
        if (c) ImGui::SameLine();
        ImGui::RadioButton(std::to_string(c + 1).c_str(), &activeChannel_, c);
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(90.0f);
    int len = (int)node->lengthBars();
    if (ImGui::InputInt("Length (bars)", &len)) node->setLengthBars((float)len);

    float lo = node->outMin(activeChannel_), hi = node->outMax(activeChannel_);
    ImGui::SameLine(); ImGui::SetNextItemWidth(70.0f);
    if (ImGui::DragFloat("min", &lo, 0.01f)) node->setOutRange(activeChannel_, lo, hi);
    ImGui::SameLine(); ImGui::SetNextItemWidth(70.0f);
    if (ImGui::DragFloat("max", &hi, 0.01f)) node->setOutRange(activeChannel_, lo, hi);
    ImGui::SameLine();
    if (ImGui::Button("Clear ch")) node->channel(activeChannel_).clear();

    // --- Scrollable timeline ---
    const float pxPerBar = 90.0f;
    const float contentW = std::max(1.0f, node->lengthBars() * pxPerBar);
    ImGui::BeginChild("tl", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    float contentH = ImGui::GetContentRegionAvail().y;
    if (contentH < 80.0f) contentH = 80.0f;
    ImGui::InvisibleButton("canvas", ImVec2(contentW, contentH));
    const bool   hovered = ImGui::IsItemHovered();
    const ImVec2 o = ImGui::GetItemRectMin();          // content origin (accounts for scroll)
    ImDrawList*  dl = ImGui::GetWindowDrawList();

    auto X = [&](float bar) { return o.x + bar * pxPerBar; };
    auto Y = [&](float val) { return o.y + (1.0f - val) * contentH; };
    auto toBar = [&](float px) { return (px - o.x) / pxPerBar; };
    auto toVal = [&](float py) { return 1.0f - (py - o.y) / contentH; };

    dl->AddRectFilled(o, ImVec2(o.x + contentW, o.y + contentH), IM_COL32(20, 22, 28, 255));
    for (int b = 0; b <= (int)node->lengthBars(); ++b) {
        float x = X((float)b);
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + contentH), IM_COL32(60, 64, 74, 255));
        char t[8]; std::snprintf(t, sizeof(t), "%d", b);
        dl->AddText(ImVec2(x + 3, o.y + 3), IM_COL32(120, 128, 140, 255), t);
    }
    dl->AddLine(ImVec2(o.x, Y(0.5f)), ImVec2(o.x + contentW, Y(0.5f)), IM_COL32(45, 48, 58, 255));

    auto drawCurve = [&](int c, bool active) {
        auto& p = node->channel(c);
        ImU32 col = channelColour(c, active);
        float w = active ? 2.0f : 1.0f;
        if (!p.empty()) {
            dl->AddLine(ImVec2(o.x, Y(p.front().value)), ImVec2(X(p.front().bar), Y(p.front().value)), col, w);
            for (std::size_t i = 0; i + 1 < p.size(); ++i)
                dl->AddLine(ImVec2(X(p[i].bar), Y(p[i].value)), ImVec2(X(p[i+1].bar), Y(p[i+1].value)), col, w);
            dl->AddLine(ImVec2(X(p.back().bar), Y(p.back().value)), ImVec2(o.x + contentW, Y(p.back().value)), col, w);
        }
    };
    for (int c = 0; c < node->channelCount(); ++c) if (c != activeChannel_) drawCurve(c, false);
    drawCurve(activeChannel_, true);

    auto& ap = node->channel(activeChannel_);
    for (auto& pt : ap)
        dl->AddCircleFilled(ImVec2(X(pt.bar), Y(pt.value)), 4.0f, channelColour(activeChannel_, true));

    float phx = X(std::clamp(node->currentBar(), 0.0f, node->lengthBars()));
    dl->AddLine(ImVec2(phx, o.y), ImVec2(phx, o.y + contentH), IM_COL32(255, 80, 80, 220), 2.0f);

    // --- Mouse editing ---
    const ImVec2 m = ImGui::GetIO().MousePos;
    auto hitPoint = [&]() -> int {
        for (int i = 0; i < (int)ap.size(); ++i) {
            float dx = m.x - X(ap[i].bar), dy = m.y - Y(ap[i].value);
            if (dx * dx + dy * dy <= 8.0f * 8.0f) return i;
        }
        return -1;
    };
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        int hit = hitPoint();
        if (hit >= 0) ap.erase(ap.begin() + hit);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int hit = hitPoint();
        if (hit >= 0) {
            dragPoint_ = hit;
        } else {                                        // add a breakpoint, keep sorted
            AutoPoint np{ std::clamp(toBar(m.x), 0.0f, node->lengthBars()),
                          std::clamp(toVal(m.y), 0.0f, 1.0f) };
            auto it = std::lower_bound(ap.begin(), ap.end(), np,
                                       [](const AutoPoint& a, const AutoPoint& b) { return a.bar < b.bar; });
            dragPoint_ = (int)(it - ap.begin());
            ap.insert(it, np);
        }
    }
    if (dragPoint_ >= 0 && dragPoint_ < (int)ap.size() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float nb = std::clamp(toBar(m.x), 0.0f, node->lengthBars());
        float nv = std::clamp(toVal(m.y), 0.0f, 1.0f);
        if (dragPoint_ > 0)                  nb = std::max(nb, ap[dragPoint_ - 1].bar + 1e-4f);
        if (dragPoint_ + 1 < (int)ap.size()) nb = std::min(nb, ap[dragPoint_ + 1].bar - 1e-4f);
        ap[dragPoint_].bar = nb; ap[dragPoint_].value = nv;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragPoint_ = -1;

    ImGui::EndChild();
    ImGui::TextDisabled("Click to add a point, drag to move, right-click to delete. "
                        "The red line is the transport playhead.");
    ImGui::End();
}

} // namespace oss
