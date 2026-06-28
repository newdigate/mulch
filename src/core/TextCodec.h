#pragma once
#include <sstream>
#include <string>

namespace oss {

// Escape '\\' and '\n' so a free-text field survives the line-based .oss/.osslib codecs.
inline std::string escape(const std::string& s) {
    std::string o;
    for (char ch : s) { if (ch == '\\') o += "\\\\"; else if (ch == '\n') o += "\\n"; else o += ch; }
    return o;
}
inline std::string unescape(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == 'n') { o += '\n'; ++i; }
            else if (n == '\\') { o += '\\'; ++i; }
            else o += s[i];
        } else o += s[i];
    }
    return o;
}
// The remainder of `ls` after the current token, leading whitespace trimmed (rest-of-line fields).
inline std::string restOfLine(std::istringstream& ls) {
    std::string rest; std::getline(ls >> std::ws, rest); return rest;
}

} // namespace oss
