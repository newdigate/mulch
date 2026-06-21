#include "ui/AutomationPanel.h"
#include "core/Graph.h"
#include "core/Transport.h"
#include "core/AutomationStore.h"
#include "core/AutoCurve.h"
#include "modules/AutomationNode.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
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

    // Zoom: scale the time axis (H) and lane height (V). Each click is x1.25,
    // clamped; the factors persist for the session.
    ImGui::SameLine();
    ImGui::TextDisabled("|  Zoom");
    ImGui::SameLine();
    if (ImGui::SmallButton("H-")) zoomX_ = std::max(0.3f, zoomX_ / 1.25f);
    ImGui::SameLine();
    if (ImGui::SmallButton("H+")) zoomX_ = std::min(4.0f, zoomX_ * 1.25f);
    ImGui::SameLine();
    if (ImGui::SmallButton("V-")) zoomY_ = std::max(0.5f, zoomY_ / 1.25f);
    ImGui::SameLine();
    if (ImGui::SmallButton("V+")) zoomY_ = std::min(3.0f, zoomY_ * 1.25f);

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
    const float rulerH = 24.0f, headerH = 22.0f;
    const float leftW = 210.0f, inset = 7.0f;
    // Lane size scales with the zoom factors (toolbar buttons), clamped.
    const float laneH    = std::clamp(46.0f * zoomY_, 24.0f, 140.0f);
    const float pxPerBar = std::clamp(55.0f * zoomX_, 16.0f, 220.0f);

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
            r.key = (long)an->id() * 1000 + c;   // stable lane identity (drag + PushID)
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
            r.key = (long)nid * 1000 + 500 + ch.port;   // stable lane identity (drag + PushID)
            r.pts = &ch.curve.points; r.omin = ch.outMin; r.omax = ch.outMax;
            r.col = uiColour(idx); r.label = pname;
            r.uich = &ch; r.delNode = nid; r.delPort = ch.port;
            pushRow(r, laneH);
            ++idx;
        }
    }

    // Drop collapse state for groups that no longer exist (so open_ can't grow
    // without bound across a long session of node creation/deletion).
    {
        std::vector<long> live;
        for (auto& r : rows) if (r.kind == Row::Header) live.push_back(r.key);
        for (auto it = open_.begin(); it != open_.end(); )
            it = (std::find(live.begin(), live.end(), it->first) == live.end())
                 ? open_.erase(it) : std::next(it);
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
            ImGui::PushID((int)r.key);
            ImGui::SetCursorPos(ImVec2(12.0f, y[i] + 4.0f));
            ImGui::TextColored(ImColor(r.col), "%s", r.label.c_str());
            // The range + buttons row only fits when the lane is tall enough; at
            // strong vertical zoom-out a compact lane shows just the label + curve.
            if (laneH >= 40.0f) {
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
                if (ImGui::SmallButton("clr")) {
                    r.pts->clear();
                    if (dragLane_ == r.key) { dragLane_ = -1; dragPoint_ = -1; }
                    if (selLane_  == r.key) { selLane_  = -1; selPoint_  = -1; }
                }
                if (r.uich) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) { pendingDelNode = r.delNode; pendingDelPort = r.delPort; }
                }
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
    auto laneYv  = [&](std::size_t i, float v) {
        float tp = o.y + y[i] + inset, bp = o.y + y[i] + hgt[i] - inset;
        return bp - v * (bp - tp);
    };
    auto laneVal = [&](std::size_t i, float py) {
        float tp = o.y + y[i] + inset, bp = o.y + y[i] + hgt[i] - inset;
        return (bp - py) / (bp - tp);
    };
    const float stubPx = 26.0f;   // retracted handles draw as a short, grabbable stub
    auto outTipS = [&](std::size_t i, const AutoPoint& sp) {
        return (sp.outDBar != 0.0f || sp.outDValue != 0.0f)
            ? ImVec2(X(sp.bar + sp.outDBar), laneYv(i, sp.value + sp.outDValue))
            : ImVec2(X(sp.bar) + stubPx, laneYv(i, sp.value));
    };
    auto inTipS = [&](std::size_t i, const AutoPoint& sp) {
        return (sp.inDBar != 0.0f || sp.inDValue != 0.0f)
            ? ImVec2(X(sp.bar + sp.inDBar), laneYv(i, sp.value + sp.inDValue))
            : ImVec2(X(sp.bar) - stubPx, laneYv(i, sp.value));
    };

    // Row backgrounds (lanes alternate by lane position, not absolute row index).
    int laneStripe = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        float top = o.y + y[i], bot = o.y + y[i] + hgt[i];
        ImU32 bg;
        if      (rows[i].kind == Row::Ruler)  bg = IM_COL32(32, 35, 44, 255);
        else if (rows[i].kind == Row::Header) bg = IM_COL32(44, 49, 62, 255);
        else                                  bg = (laneStripe++ & 1) ? IM_COL32(29, 32, 38, 255)
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
        for (std::size_t k = 0; k + 1 < p->size(); ++k) {
            CurvePt cc[4]; bezierControls((*p)[k], (*p)[k + 1], cc);
            dl->AddBezierCubic(ImVec2(X(cc[0].bar), Yv(cc[0].value)), ImVec2(X(cc[1].bar), Yv(cc[1].value)),
                               ImVec2(X(cc[2].bar), Yv(cc[2].value)), ImVec2(X(cc[3].bar), Yv(cc[3].value)),
                               col, 2.0f, 0);
        }
        dl->AddLine(ImVec2(X(p->back().bar), Yv(p->back().value)), ImVec2(o.x + contentW, Yv(p->back().value)), col, 2.0f);
        for (auto& pt : *p) dl->AddCircleFilled(ImVec2(X(pt.bar), Yv(pt.value)), 4.0f, col);
    }
    // Selected point's tangent handles (drawn only for the selected lane/point).
    if (selLane_ >= 0 && selPoint_ >= 0) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].kind != Row::Lane || rows[i].key != selLane_) continue;
            auto* p = rows[i].pts;
            if (selPoint_ < (int)p->size()) {
                const AutoPoint& sp = (*p)[selPoint_];
                ImVec2 pc(X(sp.bar), laneYv(i, sp.value));
                const ImU32 hcol = IM_COL32(232, 232, 244, 230);
                if (selPoint_ + 1 < (int)p->size()) {            // out-handle (segment to the right exists)
                    ImVec2 tp = outTipS(i, sp);
                    dl->AddLine(pc, tp, hcol, 1.0f);
                    dl->AddRectFilled(ImVec2(tp.x - 3, tp.y - 3), ImVec2(tp.x + 3, tp.y + 3), hcol);
                }
                if (selPoint_ > 0) {                             // in-handle (segment to the left exists)
                    ImVec2 tp = inTipS(i, sp);
                    dl->AddLine(pc, tp, hcol, 1.0f);
                    dl->AddRectFilled(ImVec2(tp.x - 3, tp.y - 3), ImVec2(tp.x + 3, tp.y + 3), hcol);
                }
                dl->AddCircle(pc, 6.0f, hcol, 0, 1.5f);          // ring marks the selected point
            }
            break;
        }
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
    auto hitHandle = [&](std::size_t i) -> int {
        if (rows[i].key != selLane_ || selPoint_ < 0) return 0;
        auto* p = rows[i].pts;
        if (selPoint_ >= (int)p->size()) return 0;
        const AutoPoint& sp = (*p)[selPoint_];
        if (selPoint_ + 1 < (int)p->size()) {
            ImVec2 t = outTipS(i, sp);
            if ((m.x - t.x) * (m.x - t.x) + (m.y - t.y) * (m.y - t.y) <= 7.0f * 7.0f) return 1;
        }
        if (selPoint_ > 0) {
            ImVec2 t = inTipS(i, sp);
            if ((m.x - t.x) * (m.x - t.x) + (m.y - t.y) * (m.y - t.y) <= 7.0f * 7.0f) return 2;
        }
        return 0;
    };
    int hoverRow = -1;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == Row::Lane && m.y >= o.y + y[i] && m.y < o.y + y[i] + hgt[i]) { hoverRow = (int)i; break; }

    if (hovered && hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        int hh = hitHandle((std::size_t)hoverRow);
        if (hh != 0) {
            AutoPoint& sp = (*rows[hoverRow].pts)[selPoint_];
            if (hh == 1) { sp.outDBar = 0.0f; sp.outDValue = 0.0f; }   // retract the handle
            else         { sp.inDBar  = 0.0f; sp.inDValue  = 0.0f; }
        } else {
            auto* p = rows[hoverRow].pts; int h = hitPoint(hoverRow);
            if (h >= 0) {
                p->erase(p->begin() + h);
                // keep an in-progress drag on this lane pointing at the right point
                if (dragLane_ == rows[hoverRow].key) {
                    if (h < dragPoint_)      --dragPoint_;
                    else if (h == dragPoint_) { dragLane_ = -1; dragPoint_ = -1; }
                }
                // keep the selection valid after a delete on its lane
                if (selLane_ == rows[hoverRow].key) {
                    if (h < selPoint_)       --selPoint_;
                    else if (h == selPoint_) { selLane_ = -1; selPoint_ = -1; }
                }
            }
        }
    }
    if (hovered && hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int hh = hitHandle((std::size_t)hoverRow);
        if (hh != 0) {                                   // grab the selected point's handle
            dragLane_ = selLane_; dragPoint_ = selPoint_; dragHandle_ = hh;
        } else {
            auto* p = rows[hoverRow].pts; int h = hitPoint(hoverRow);
            if (h >= 0) {                                // select + start moving an existing point
                dragLane_ = rows[hoverRow].key; dragPoint_ = h; dragHandle_ = 0;
                selLane_  = rows[hoverRow].key; selPoint_  = h;
            } else {                                     // add a new (linear) point + select it
                AutoPoint np{ std::clamp(toBar(m.x), 0.0f, length),
                              std::clamp(rowVal(hoverRow, m.y), 0.0f, 1.0f) };
                auto it = std::lower_bound(p->begin(), p->end(), np,
                                           [](const AutoPoint& a, const AutoPoint& b) { return a.bar < b.bar; });
                int idx = (int)(it - p->begin());
                p->insert(it, np);
                dragLane_ = rows[hoverRow].key; dragPoint_ = idx; dragHandle_ = 0;
                selLane_  = rows[hoverRow].key; selPoint_  = idx;
            }
        }
    } else if (hovered && hoverRow < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        selLane_ = -1; selPoint_ = -1;                   // clicked empty space -> deselect
    }
    // Continue an in-progress drag by re-finding its lane each frame from the stable
    // lane key -- so the drag survives the cursor leaving the lane vertically, and a
    // points vector belonging to a since-deleted node/channel is never dereferenced.
    if (dragLane_ >= 0 && dragPoint_ >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        int dr = -1;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].kind == Row::Lane && rows[i].key == dragLane_) { dr = (int)i; break; }
        if (dr >= 0) {
            auto* p = rows[dr].pts;
            if (dragPoint_ < (int)p->size()) {
                if (dragHandle_ == 0) {
                    // ---- move the point (unchanged, neighbour-clamped) ----
                    float nb = std::clamp(toBar(m.x), 0.0f, length);
                    float nv = std::clamp(rowVal((std::size_t)dr, m.y), 0.0f, 1.0f);
                    if (dragPoint_ > 0)                  nb = std::max(nb, (*p)[dragPoint_ - 1].bar + 1e-4f);
                    if (dragPoint_ + 1 < (int)p->size()) nb = std::min(nb, (*p)[dragPoint_ + 1].bar - 1e-4f);
                    (*p)[dragPoint_].bar = nb; (*p)[dragPoint_].value = nv;
                } else {
                    // ---- drag a tangent handle ----
                    AutoPoint& sp = (*p)[dragPoint_];
                    int n = (int)p->size();
                    bool out = (dragHandle_ == 1);
                    if (ImGui::GetIO().KeyAlt) sp.broken = true;     // Alt-drag breaks alignment (sticky)
                    // primary handle: offset from cursor, clamped into its segment + lane
                    float nv = std::clamp(laneVal((std::size_t)dr, m.y), 0.0f, 1.0f) - sp.value;
                    if (out) {
                        float nb = std::max(0.0f, toBar(m.x) - sp.bar);
                        if (dragPoint_ + 1 < n) nb = std::min(nb, (*p)[dragPoint_ + 1].bar - sp.bar);
                        sp.outDBar = nb; sp.outDValue = nv;
                    } else {
                        float nb = std::min(0.0f, toBar(m.x) - sp.bar);
                        if (dragPoint_ > 0)     nb = std::max(nb, (*p)[dragPoint_ - 1].bar - sp.bar);
                        sp.inDBar = nb; sp.inDValue = nv;
                    }
                    // align the opposite handle in SCREEN space (preserve its length) unless broken
                    bool haveOpp = out ? (dragPoint_ > 0) : (dragPoint_ + 1 < n);
                    if (!sp.broken && haveOpp) {
                        ImVec2 pcS(X(sp.bar), laneYv((std::size_t)dr, sp.value));
                        ImVec2 priS = out ? outTipS((std::size_t)dr, sp) : inTipS((std::size_t)dr, sp);
                        ImVec2 oppS = out ? inTipS((std::size_t)dr, sp)  : outTipS((std::size_t)dr, sp);
                        float dx = priS.x - pcS.x, dy = priS.y - pcS.y;
                        float len = std::sqrt(dx * dx + dy * dy);
                        float odx = oppS.x - pcS.x, ody = oppS.y - pcS.y;
                        float oppLen = std::sqrt(odx * odx + ody * ody);
                        if (len > 1e-3f) {
                            float tx = pcS.x - dx / len * oppLen;
                            float ty = pcS.y - dy / len * oppLen;
                            float ob = toBar(tx) - sp.bar;
                            float ov = std::clamp(laneVal((std::size_t)dr, ty), 0.0f, 1.0f) - sp.value;
                            if (out) {                                  // opposite is the in-handle (backward)
                                ob = std::min(0.0f, ob);
                                if (dragPoint_ > 0) ob = std::max(ob, (*p)[dragPoint_ - 1].bar - sp.bar);
                                sp.inDBar = ob; sp.inDValue = ov;
                            } else {                                    // opposite is the out-handle (forward)
                                ob = std::max(0.0f, ob);
                                if (dragPoint_ + 1 < n) ob = std::min(ob, (*p)[dragPoint_ + 1].bar - sp.bar);
                                sp.outDBar = ob; sp.outDValue = ov;
                            }
                        }
                    }
                }
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { dragLane_ = -1; dragPoint_ = -1; dragHandle_ = 0; }

    ImGui::EndChild();

    if (pendingDelNode >= 0) store.remove(pendingDelNode, pendingDelPort);

    ImGui::TextDisabled("Click a lane to add/select a point, drag to move, right-click to delete. "
                        "Select a point to reveal its Bezier handles; drag a handle to curve, "
                        "Alt-drag to break, right-click a handle to reset. Red line = playhead.");
    ImGui::End();
}

} // namespace oss
