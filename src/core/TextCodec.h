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
// One trailing '\r' is dropped so a CRLF-edited .oss/.osslib parses like an LF one: the values here
// are paths, labels, node types and control text, none of which legitimately ends in a CR, and a CR
// left on a path fails to open with an error that looks correct. Numeric fields need no help
// (`operator>>` treats '\r' as whitespace) and the header checks are prefix tests.
inline std::string restOfLine(std::istringstream& ls) {
    std::string rest; std::getline(ls >> std::ws, rest);
    if (!rest.empty() && rest.back() == '\r') rest.pop_back();
    return rest;
}

} // namespace oss
