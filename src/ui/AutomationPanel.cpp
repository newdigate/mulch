#include "ui/AutomationPanel.h"
#include "core/Graph.h"
#include "core/Transport.h"
#include "core/AutomationStore.h"
#include "modules/AutomationNode.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace oss {

static ImU32 streamColour(int c) {
    static const ImU32 cols[4] = {
        IM_COL32( 90, 200, 255, 255),   // cyan
        IM_COL32(255, 180,  80, 255),   // orange
        IM_COL32(140, 255, 120, 255),   // green
        IM_COL32(255, 120, 200, 255),   // pink
    };
    return cols[c & 3];
}
static ImU32 uiColour(int i) {
    static const ImU32 cols[4] = {
        IM_COL32(179, 157, 219, 255),   // purple
        IM_COL32(128, 203, 196, 255),   // teal
        IM_COL32(255, 171, 145, 255),   // coral
        IM_COL32(197, 225, 165, 255),   // light green
    };
    return cols[i & 3];
}

void AutomationPanel::draw(Graph& graph) {
    ImGui::Begin("Automation");
    AutomationStore& store = graph.automation();

    // --- Toolbar: one global song length, shared by every lane's time axis ---
    ImGui::SetNextItemWidth(90.0f);
    int len = (int)store.lengthBars();
    if (ImGui::InputInt("Length (bars)", &len)) store.setLengthBars((float)len);
    ImGui::SameLine();
    ImGui::TextDisabled("global \xE2\x80\xA2 right-click a node to automate a parameter");

    // --- Build this frame's row layout: a ruler, then group headers + lanes ---
    struct Row {
        enum Kind { Ruler, Header, Lane } kind = Lane;
        long key = 0; std::string htext;                   // Header
        std::vector<AutoPoint>* pts = nullptr;             // Lane points
        float omin = 0.0f, omax = 1.0f; ImU32 col = 0; std::string label;
        AutomationNode* anode = nullptr; int achan = -1;   // stream source
        UiAutomationChannel* uich = nullptr;               // ui source
        int delNode = 0, delPort = 0;                      // ui delete target
    };
    const float rulerH = 24.0f, headerH = 22.0f, laneH = 46.0f;
    const float leftW = 210.0f, pxPerBar = 55.0f, inset = 7.0f;

    std::vector<Row> rows;
    std::vector<float> hgt;
    auto pushRow = [&](Row r, float h){ rows.push_back(std::move(r)); hgt.push_back(h); };

    { Row r; r.kind = Row::Ruler; pushRow(r, rulerH); }

    // Stream groups: one per AutomationNode, its 4 channel lanes.
    for (auto& up : graph.nodes()) {
        auto* an = dynamic_cast<AutomationNode*>(up.get());
        if (!an) continue;
        long key = (long)an->id() * 2 + 0;
        Row h; h.kind = Row::Header; h.key = key;
        h.htext = "Automation #" + std::to_string(an->id());
        pushRow(h, headerH);
        if (!isOpen(key)) continue;
        for (int c = 0; c < an->channelCount(); ++c) {
            Row r; r.kind = Row::Lane;
            r.pts = &an->channel(c); r.omin = an->outMin(c); r.omax = an->outMax(c);
            r.col = streamColour(c); r.label = "ch " + std::to_string(c + 1);
            r.anode = an; r.achan = c;
            pushRow(r, laneH);
        }
    }

    // UI groups: one per target node (first-seen order), one lane per channel.
    std::vector<int> uiNodes;
    for (auto& ch : store.channels())
        if (std::find(uiNodes.begin(), uiNodes.end(), ch.nodeId) == uiNodes.end())
            uiNodes.push_back(ch.nodeId);
    for (int nid : uiNodes) {
        Node* n = graph.findNode(nid);
        std::string nm = n ? n->name() : std::string("node");
        long key = (long)nid * 2 + 1;
        Row h; h.kind = Row::Header; h.key = key;
        h.htext = nm + " #" + std::to_string(nid);
        pushRow(h, headerH);
        if (!isOpen(key)) continue;
        int idx = 0;
        for (auto& ch : store.channels()) {
            if (ch.nodeId != nid) continue;
            std::string pname = (n && ch.port < (int)n->inputs().size())
                                ? n->inputs()[ch.port].name
                                : ("port " + std::to_string(ch.port));
            Row r; r.kind = Row::Lane;
            r.pts = &ch.curve.points; r.omin = ch.outMin; r.omax = ch.outMax;
            r.col = uiColour(idx); r.label = pname;
            r.uich = &ch; r.delNode = nid; r.delPort = ch.port;
            pushRow(r, laneH);
            ++idx;
        }
    }

    if (rows.size() == 1) {   // only the ruler -> nothing to edit yet
        ImGui::TextDisabled("Add an Automation node, or right-click any node to automate a parameter.");
        ImGui::End();
        return;
    }

    // Cumulative y offset per row.
    std::vector<float> y(rows.size() + 1, 0.0f);
    for (std::size_t i = 0; i < rows.size(); ++i) y[i + 1] = y[i] + hgt[i];
    const float totalH = y[rows.size()];
    const float scrollH = ImGui::GetStyle().ScrollbarSize;

    int pendingDelNode = -1, pendingDelPort = -1;   // deferred ui-channel delete

    // --- Left column: per-row labels and controls (no horizontal scroll) ---
    ImGui::BeginChild("autoLabels", ImVec2(leftW, totalH), false);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        Row& r = rows[i];
        if (r.kind == Row::Ruler) {
            ImGui::SetCursorPos(ImVec2(8.0f, y[i] + rulerH * 0.5f - 8.0f));
            ImGui::TextDisabled("bars");
        } else if (r.kind == Row::Header) {
            ImGui::SetCursorPos(ImVec2(2.0f, y[i] + 2.0f));
            bool op = isOpen(r.key);
            std::string lbl = (op ? "v  " : ">  ") + r.htext + "##grp" + std::to_string(r.key);
            if (ImGui::Selectable(lbl.c_str(), false, 0, ImVec2(leftW - 4.0f, headerH - 3.0f)))
                open_[r.key] = !op;
        } else {
            ImGui::PushID((int)i);
            ImGui::SetCursorPos(ImVec2(12.0f, y[i] + 4.0f));
            ImGui::TextColored(ImColor(r.col), "%s", r.label.c_str());
            ImGui::SetCursorPos(ImVec2(12.0f, y[i] + 24.0f));
            float lo = r.omin, hi = r.omax;
            ImGui::SetNextItemWidth(52.0f);
            if (ImGui::DragFloat("##lo", &lo, 0.01f, 0, 0, "%.2f")) {
                if (r.anode)      r.anode->setOutRange(r.achan, lo, hi);
                else if (r.uich)  r.uich->outMin = lo;
            }
            ImGui::SameLine(); ImGui::SetNextItemWidth(52.0f);
            if (ImGui::DragFloat("##hi", &hi, 0.01f, 0, 0, "%.2f")) {
                if (r.anode)      r.anode->setOutRange(r.achan, lo, hi);
                else if (r.uich)  r.uich->outMax = hi;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("clr")) r.pts->clear();
            if (r.uich) {
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) { pendingDelNode = r.delNode; pendingDelPort = r.delPort; }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // --- Lane area: shared time axis, scrolls horizontally ---
    ImGui::BeginChild("autoLanes", ImVec2(0, totalH + scrollH + 4.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const float length   = std::max(1.0f, store.lengthBars());
    const float contentW = std::max(1.0f, length * pxPerBar);
    ImGui::InvisibleButton("lanes", ImVec2(contentW, totalH));
    const bool   hovered = ImGui::IsItemHovered();
    const ImVec2 o = ImGui::GetItemRectMin();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    auto X = [&](float bar) { return o.x + bar * pxPerBar; };

    // Row backgrounds.
    for (std::size_t i = 0; i < rows.size(); ++i) {
        float top = o.y + y[i], bot = o.y + y[i] + hgt[i];
        ImU32 bg;
        if      (rows[i].kind == Row::Ruler)  bg = IM_COL32(32, 35, 44, 255);
        else if (rows[i].kind == Row::Header) bg = IM_COL32(44, 49, 62, 255);
        else                                  bg = (i & 1) ? IM_COL32(29, 32, 38, 255)
                                                           : IM_COL32(24, 26, 32, 255);
        dl->AddRectFilled(ImVec2(o.x, top), ImVec2(o.x + contentW, bot), bg);
    }
    // Bar grid lines + numbers (numbers in the ruler row).
    for (int b = 0; b <= (int)length; ++b) {
        float x = X((float)b);
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + totalH), IM_COL32(55, 58, 68, 255));
        char t[8]; std::snprintf(t, sizeof(t), "%d", b);
        dl->AddText(ImVec2(x + 3, o.y + rulerH * 0.5f - 7.0f), IM_COL32(120, 128, 140, 255), t);
    }
    // Each lane's curve + breakpoints.
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind != Row::Lane) continue;
        auto* p = rows[i].pts; ImU32 col = rows[i].col;
        float t = o.y + y[i] + inset, bt = o.y + y[i] + hgt[i] - inset;
        auto Yv = [&](float v) { return bt - v * (bt - t); };
        if (p->empty()) continue;
        dl->AddLine(ImVec2(o.x, Yv(p->front().value)), ImVec2(X(p->front().bar), Yv(p->front().value)), col, 2.0f);
        for (std::size_t k = 0; k + 1 < p->size(); ++k)
            dl->AddLine(ImVec2(X((*p)[k].bar), Yv((*p)[k].value)),
                        ImVec2(X((*p)[k + 1].bar), Yv((*p)[k + 1].value)), col, 2.0f);
        dl->AddLine(ImVec2(X(p->back().bar), Yv(p->back().value)), ImVec2(o.x + contentW, Yv(p->back().value)), col, 2.0f);
        for (auto& pt : *p) dl->AddCircleFilled(ImVec2(X(pt.bar), Yv(pt.value)), 4.0f, col);
    }
    // Playhead across the ruler + all lanes.
    float phx = X(std::clamp((float)graph.transport().bars(), 0.0f, length));
    dl->AddLine(ImVec2(phx, o.y), ImVec2(phx, o.y + totalH), IM_COL32(255, 80, 80, 220), 2.0f);

    // --- Mouse editing ---
    const ImVec2 m = ImGui::GetIO().MousePos;
    auto toBar  = [&](float px) { return (px - o.x) / pxPerBar; };
    auto rowVal = [&](std::size_t i, float py) {
        float t = o.y + y[i] + inset, bt = o.y + y[i] + hgt[i] - inset;
        return (bt - py) / (bt - t);
    };
    auto hitPoint = [&](std::size_t i) -> int {
        auto* p = rows[i].pts;
        float t = o.y + y[i] + inset, bt = o.y + y[i] + hgt[i] - inset;
        for (int k = 0; k < (int)p->size(); ++k) {
            float dx = m.x - X((*p)[k].bar), dy = m.y - (bt - (*p)[k].value * (bt - t));
            if (dx * dx + dy * dy <= 8.0f * 8.0f) return k;
        }
        return -1;
    };
    int hoverRow = -1;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == Row::Lane && m.y >= o.y + y[i] && m.y < o.y + y[i] + hgt[i]) { hoverRow = (int)i; break; }

    if (hovered && hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        auto* p = rows[hoverRow].pts; int h = hitPoint(hoverRow);
        if (h >= 0) p->erase(p->begin() + h);
    }
    if (hovered && hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto* p = rows[hoverRow].pts; int h = hitPoint(hoverRow);
        if (h >= 0) { dragPts_ = p; dragPoint_ = h; }
        else {
            AutoPoint np{ std::clamp(toBar(m.x), 0.0f, length),
                          std::clamp(rowVal(hoverRow, m.y), 0.0f, 1.0f) };
            auto it = std::lower_bound(p->begin(), p->end(), np,
                                       [](const AutoPoint& a, const AutoPoint& b) { return a.bar < b.bar; });
            dragPts_ = p; dragPoint_ = (int)(it - p->begin());
            p->insert(it, np);
        }
    }
    // Continue an in-progress drag by re-finding its lane (so the drag survives the
    // cursor leaving the lane vertically). Skip if the lane vanished (e.g. collapsed).
    if (dragPts_ && dragPoint_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        int dr = -1;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].kind == Row::Lane && rows[i].pts == dragPts_) { dr = (int)i; break; }
        if (dr >= 0 && dragPoint_ < (int)dragPts_->size()) {
            auto* p = dragPts_;
            float nb = std::clamp(toBar(m.x), 0.0f, length);
            float nv = std::clamp(rowVal((std::size_t)dr, m.y), 0.0f, 1.0f);
            if (dragPoint_ > 0)                 nb = std::max(nb, (*p)[dragPoint_ - 1].bar + 1e-4f);
            if (dragPoint_ + 1 < (int)p->size()) nb = std::min(nb, (*p)[dragPoint_ + 1].bar - 1e-4f);
            (*p)[dragPoint_].bar = nb; (*p)[dragPoint_].value = nv;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { dragPts_ = nullptr; dragPoint_ = -1; }

    ImGui::EndChild();

    if (pendingDelNode >= 0) store.remove(pendingDelNode, pendingDelPort);

    ImGui::TextDisabled("Click a lane to add a point, drag to move, right-click to delete. "
                        "Click a group header to collapse. Red line = transport playhead.");
    ImGui::End();
}

} // namespace oss
