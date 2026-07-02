#pragma once
#include <string>
#include <cctype>

namespace oss {

// The filename portion of a path (after the last '/' or '\\'); the whole string if no
// separator, and "" for a trailing-slash path. GL-free, header-only.
inline std::string fileBaseName(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// The directory portion of a path (before the last '/' or '\\'); "" if there is no
// separator (a bare filename). Complements fileBaseName (same split point).
inline std::string parentDir(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash);
}

// Append "." + ext to `path` unless it already ends in ".<ext>" (case-insensitive).
// `ext` is given without a dot (e.g. "oss"). Empty path or ext returns `path` unchanged.
inline std::string ensureExtension(const std::string& path, const std::string& ext) {
    if (path.empty() || ext.empty()) return path;
    const std::string dotExt = "." + ext;
    if (path.size() >= dotExt.size()) {
        bool match = true;
        std::size_t off = path.size() - dotExt.size();
        for (std::size_t i = 0; i < dotExt.size(); ++i) {
            if (std::tolower((unsigned char)path[off + i]) !=
                std::tolower((unsigned char)dotExt[i])) { match = false; break; }
        }
        if (match) return path;
    }
    return path + dotExt;
}

} // namespace oss
