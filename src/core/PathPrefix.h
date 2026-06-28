#pragma once
#include <string>
#include <vector>

namespace oss {

// The longest directory prefix shared by every usable path's directory (split on '/' and '\\'),
// trimmed to a directory boundary and without a trailing separator. "" when there is no shared
// directory (e.g. differing roots, a bare filename, or no usable paths).
inline std::string commonDirPrefix(const std::vector<std::string>& paths) {
    auto dirOf = [](const std::string& p) {
        std::size_t slash = p.find_last_of("/\\");
        return slash == std::string::npos ? std::string() : p.substr(0, slash);
    };
    std::vector<std::string> dirs;
    for (const std::string& p : paths) {
        if (p.empty()) continue;
        std::string d = dirOf(p);
        if (d.empty()) return "";                 // a usable path with no directory -> nothing shared
        dirs.push_back(d);
    }
    if (dirs.empty()) return "";

    std::string cp = dirs[0];                     // longest common *character* prefix of the dirs
    for (std::size_t k = 1; k < dirs.size(); ++k) {
        std::size_t i = 0;
        while (i < cp.size() && i < dirs[k].size() && cp[i] == dirs[k][i]) ++i;
        cp.erase(i);
    }
    // cp is a clean boundary only if, in every dir, it's the whole dir or the next char is a separator.
    bool boundary = true;
    for (const std::string& d : dirs) {
        bool ok = cp.size() == d.size() ||
                  (cp.size() < d.size() && (d[cp.size()] == '/' || d[cp.size()] == '\\'));
        if (!ok) { boundary = false; break; }
    }
    if (!boundary) {                              // cut mid-segment -> back up to the last separator
        std::size_t slash = cp.find_last_of("/\\");
        cp = (slash == std::string::npos) ? std::string() : cp.substr(0, slash);
    }
    while (!cp.empty() && (cp.back() == '/' || cp.back() == '\\')) cp.pop_back();
    return cp;
}

} // namespace oss
