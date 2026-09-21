#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#include "core/PathUtil.h"

namespace oss {

// Fixed seed for the bar-synced shuffle, so the order is identical in every session (node ids are
// remapped on project load, so they are not a stable seed).
inline constexpr unsigned kPresetShuffleSeed = 0x9E3779B9u;

namespace detail {
inline std::string lowered(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
inline bool hasMilkExtension(const std::string& name) {
    return name.size() > 5 && lowered(name.substr(name.size() - 5)) == ".milk";
}
} // namespace detail

// The Milkdrop presets directly inside `dir` (no recursion): "*.milk", extension matched
// case-insensitively, sorted case-insensitively by file name. Each entry is dir + "/" + name.
// A missing or empty `dir` yields an empty list.
inline std::vector<std::string> listPresetsInDir(const std::string& dir) {
    std::vector<std::string> names;
    if (dir.empty()) return names;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        std::string name = it->path().filename().string();
        if (detail::hasMilkExtension(name)) names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        std::string la = detail::lowered(a), lb = detail::lowered(b);
        return la != lb ? la < lb : a < b;
    });
    for (std::string& n : names) n = dir + "/" + n;
    return names;
}

// Index of `path` in `files`, matched by file name (one folder, so names are unique; this also
// survives '/' vs '\\' differences). -1 when absent.
inline int indexOfPreset(const std::vector<std::string>& files, const std::string& path) {
    if (path.empty()) return -1;
    std::string want = fileBaseName(path);
    for (std::size_t i = 0; i < files.size(); ++i)
        if (fileBaseName(files[i]) == want) return (int)i;
    return -1;
}

// index + delta, wrapped into [0, count). count <= 0 -> -1.
inline int stepPresetIndex(int index, int delta, int count) {
    if (count <= 0) return -1;
    long long i = ((long long)index + delta) % count;
    return (int)((i + count) % count);
}

// Which bar-synced step song position `bars` falls in: floor(bars / N). N < 1 is treated as 1.
inline long long syncedPresetStep(double bars, int everyNBars) {
    double n = (double)(everyNBars < 1 ? 1 : everyNBars);
    return (long long)std::floor(bars / n);
}

// The preset index for a synced `step`. Sequential: step mod count (an ABSOLUTE position in the
// sorted folder). Shuffle: a hash of (step, seed), so it replays identically after a loop or seek.
inline int syncedPresetIndex(long long step, int count, bool shuffle, unsigned seed) {
    if (count <= 0) return 0;
    if (!shuffle) return (int)(((step % count) + count) % count);
    unsigned long long z = (unsigned long long)step + 0x9E3779B97F4A7C15ULL * ((unsigned long long)seed + 1ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;   // splitmix64 finalizer
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= (z >> 31);
    return (int)(z % (unsigned long long)count);
}

// One frame's inputs to the selector.
struct PresetSelectorInput {
    std::string incoming;          // the resolved `preset` input value this frame
    int      button      = -1;     // -1 none, 0 prev, 1 next, 2 random
    bool     sync        = false;
    bool     playing     = false;  // transport running
    double   bars        = 0.0;    // transport position in bars
    int      everyNBars  = 4;
    bool     shuffle     = false;  // applies to the synced step
    unsigned randomValue = 0;      // caller-supplied random number, used by the random button
};

// Decides which preset should be playing. The playlist is the folder of the current preset.
//   - a CHANGE in `incoming` selects it (field edit, asset pick, or an edge);
//   - a button steps through the folder;
//   - sync is edge-triggered and primed: the first synced frame only records the step, and a
//     switch happens when the step number later changes (bar boundary, loop seam, seek).
// GL-free; the directory lister is injectable for tests.
class PresetSelector {
public:
    using Lister = std::vector<std::string> (*)(const std::string& dir);
    explicit PresetSelector(Lister lister = &listPresetsInDir) : lister_(lister) {}

    // Returns true when current() changed this frame.
    bool update(const PresetSelectorInput& in) {
        bool changed = false;
        if (in.incoming != lastIncoming_) {
            lastIncoming_ = in.incoming;
            if (in.incoming != current_) { current_ = in.incoming; changed = true; }
        }
        rescanIfFolderChanged();
        const int n = (int)files_.size();

        if (in.button >= 0 && n > 0) {
            int i = indexOfPreset(files_, current_);
            int next;
            if (in.button == 2) {
                next = (int)(in.randomValue % (unsigned)n);
                if (n > 1 && next == i) next = (next + 1) % n;
            } else if (i < 0) {
                next = (in.button == 0) ? n - 1 : 0;
            } else {
                next = stepPresetIndex(i, in.button == 0 ? -1 : +1, n);
            }
            changed = select(files_[(std::size_t)next]) || changed;
        }

        if (in.sync && in.playing && n > 0) {
            long long step = syncedPresetStep(in.bars, in.everyNBars);
            if (!syncPrimed_) { syncPrimed_ = true; lastStep_ = step; }
            else if (step != lastStep_) {
                lastStep_ = step;
                int idx = syncedPresetIndex(step, n, in.shuffle, kPresetShuffleSeed);
                changed = select(files_[(std::size_t)idx]) || changed;
            }
        } else {
            syncPrimed_ = false;
        }
        return changed;
    }

    const std::string& current() const { return current_; }
    int count() const { return (int)files_.size(); }
    int index() const { return indexOfPreset(files_, current_); }

private:
    bool select(const std::string& path) {
        if (path == current_) return false;
        current_ = path;
        return true;
    }
    void rescanIfFolderChanged() {
        std::string dir = parentDir(current_);
        if (dir == folder_) return;
        folder_ = dir;
        files_  = dir.empty() ? std::vector<std::string>{} : lister_(dir);
    }

    Lister                   lister_;
    std::vector<std::string> files_;
    std::string              folder_, current_, lastIncoming_;
    bool                     syncPrimed_ = false;
    long long                lastStep_   = 0;
};

} // namespace oss
