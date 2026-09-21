#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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
// The file-name part of `path` (after the last '/' or '\\') as a view: no allocation.
inline std::string_view baseNameView(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string_view(path)
                                      : std::string_view(path).substr(slash + 1);
}
} // namespace detail

// The Milkdrop presets directly inside `dir` (no recursion): "*.milk", extension matched
// case-insensitively, sorted case-insensitively by file name. Each entry is dir + "/" + name.
// A missing or empty `dir` yields an empty list.
inline std::vector<std::string> listPresetsInDir(const std::string& dir) {
    std::vector<std::string> out;
    if (dir.empty()) return out;
    std::vector<std::pair<std::string, std::string>> keyed;   // (lowercased name, name)
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        if (detail::lowered(it->path().extension().string()) != ".milk") continue;
        std::string name = it->path().filename().string();
        std::string key  = detail::lowered(name);
        keyed.emplace_back(std::move(key), std::move(name));
    }
    std::sort(keyed.begin(), keyed.end());                    // by lowercased name, then by exact name
    out.reserve(keyed.size());
    for (const auto& kn : keyed) out.push_back(dir + "/" + kn.second);
    return out;
}

// Index of `path` in `files`, matched by file name (one folder, so names are unique; this also
// survives '/' vs '\\' differences). -1 when absent.
inline int indexOfPreset(const std::vector<std::string>& files, const std::string& path) {
    if (path.empty()) return -1;
    const std::string_view want = detail::baseNameView(path);
    for (std::size_t i = 0; i < files.size(); ++i)
        if (detail::baseNameView(files[i]) == want) return (int)i;
    return -1;
}

// index + delta, wrapped into [0, count). count <= 0 -> -1.
inline int stepPresetIndex(int index, int delta, int count) {
    if (count <= 0) return -1;
    long long i = ((long long)index + delta) % count;
    return (int)((i + count) % count);
}

// Which bar-synced step song position `bars` falls in: floor(bars / N). N < 1 is treated as 1.
// A non-finite or out-of-range position yields 0 (casting it to long long would be undefined).
inline long long syncedPresetStep(double bars, int everyNBars) {
    double n = (double)(everyNBars < 1 ? 1 : everyNBars);
    double q = std::floor(bars / n);
    if (!(q > -9.0e18 && q < 9.0e18)) return 0;               // also false for NaN
    return (long long)q;
}

// The preset index for a synced `step`. Sequential: step mod count (an ABSOLUTE position in the
// sorted folder). Shuffle: a hash of (step, seed), so it replays identically after a loop or seek.
// Shuffle is a HASH, not a permutation, so consecutive steps can land on the same preset (about
// 1 in `count`) -- that is the price of replaying identically after a loop or seek; do not "fix"
// it into a stateful permutation. For count <= 0 it returns 0 (mirroring syncedImageIndex), so
// callers must check the list is non-empty before indexing.
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
// A pick or a button in a frame outranks a sync boundary in that same frame -- the boundary is
// consumed, so the click is never swallowed. Sync re-primes (rather than switching) when
// `everyNBars` changes; a pick in another folder is covered by the same manual-action rule.
// While `sync` is on, prev/next/random hold only until the next boundary, when absolute
// positioning reclaims the selection.
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
            changed = select(in.incoming);
        }
        rescanIfFolderChanged();
        const int n = (int)files_.size();

        if (in.button >= 0 && in.button <= 2 && n > 0) {
            const int i = index_;
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
        const bool manual = changed;        // a pick or a button this frame outranks a sync boundary

        if (in.sync && in.playing && n > 0) {
            long long step = syncedPresetStep(in.bars, in.everyNBars);
            if (!syncPrimed_ || in.everyNBars != lastEveryN_) {
                syncPrimed_ = true; lastEveryN_ = in.everyNBars; lastStep_ = step;   // (re)prime: no switch
            } else if (step != lastStep_) {
                lastStep_ = step;           // the boundary is consumed either way
                if (!manual) {
                    int idx = syncedPresetIndex(step, n, in.shuffle, kPresetShuffleSeed);
                    changed = select(files_[(std::size_t)idx]) || changed;
                }
            }
        } else {
            syncPrimed_ = false;
        }
        return changed;
    }

    const std::string& current() const { return current_; }
    int count() const { return (int)files_.size(); }
    int index() const { return index_; }    // cached: -1 when current() is not in the folder listing

private:
    bool select(const std::string& path) {
        if (path == current_) return false;
        current_ = path;
        index_   = indexOfPreset(files_, current_);
        return true;
    }
    void rescanIfFolderChanged() {
        std::string dir = parentDir(current_);
        if (dir == folder_) return;
        folder_ = dir;
        files_  = dir.empty() ? std::vector<std::string>{} : lister_(dir);
        index_  = indexOfPreset(files_, current_);
    }

    Lister                   lister_;
    std::vector<std::string> files_;
    std::string              folder_, current_, lastIncoming_;
    int                      index_      = -1;
    bool                     syncPrimed_ = false;
    long long                lastStep_   = 0;
    int                      lastEveryN_ = 0;
};

} // namespace oss
