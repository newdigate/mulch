#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace oss {

// 8 pattern slots, each a 4x16 tri-state step grid. Cell values: 0 = off, 1 = on, 2 = accent.
// The active slot is the playback / edit source of truth. GL-free, header-only. The text codec
// uses no spaces or ';' inside a grid so it is safe in the project file's saveState string.
class DrumPatterns {
public:
    static constexpr int kRows = 4, kCols = 16, kSlots = 8;

    int  active() const { return active_; }
    void setActive(int s) { active_ = std::clamp(s, 0, kSlots - 1); }

    int  cell(int r, int c) const { return at(active_, r, c); }          // active slot
    int  cell(int slot, int r, int c) const { return at(slot, r, c); }   // explicit slot

    // Cycle the active slot's cell off -> on -> accent -> off.
    void cycleCell(int r, int c) {
        if (r < 0 || r >= kRows || c < 0 || c >= kCols) return;
        std::uint8_t& v = grids_[active_][r][c];
        v = (std::uint8_t)((v + 1) % 3);
    }
    void setCell(int slot, int r, int c, int v) {
        if (slot < 0 || slot >= kSlots || r < 0 || r >= kRows || c < 0 || c >= kCols) return;
        grids_[slot][r][c] = (std::uint8_t)std::clamp(v, 0, 2);
    }

    // "<active>;<g0>;...;<g7>", each grid 64 chars (row-major 4x16) of '0'/'1'/'2'.
    std::string encode() const {
        std::string s = std::to_string(active_);
        for (int slot = 0; slot < kSlots; ++slot) {
            s += ';';
            for (int r = 0; r < kRows; ++r)
                for (int c = 0; c < kCols; ++c)
                    s += (char)('0' + grids_[slot][r][c]);
        }
        return s;
    }
    void decode(const std::string& s) {
        if (s.empty()) return;
        std::vector<std::string> parts; std::string cur;
        for (char ch : s) { if (ch == ';') { parts.push_back(cur); cur.clear(); } else cur += ch; }
        parts.push_back(cur);
        if (parts.empty()) return;
        try { active_ = std::clamp(std::stoi(parts[0]), 0, kSlots - 1); } catch (...) { active_ = 0; }
        for (int slot = 0; slot < kSlots && slot + 1 < (int)parts.size(); ++slot) {
            const std::string& g = parts[(std::size_t)(slot + 1)];
            for (int r = 0; r < kRows; ++r)
                for (int c = 0; c < kCols; ++c) {
                    int idx = r * kCols + c;
                    int v = (idx < (int)g.size()) ? (g[(std::size_t)idx] - '0') : 0;
                    grids_[slot][r][c] = (std::uint8_t)std::clamp(v, 0, 2);
                }
        }
    }

private:
    int at(int slot, int r, int c) const {
        if (slot < 0 || slot >= kSlots || r < 0 || r >= kRows || c < 0 || c >= kCols) return 0;
        return grids_[slot][r][c];
    }
    std::uint8_t grids_[kSlots][kRows][kCols] = {};
    int          active_ = 0;
};

} // namespace oss
