#include "core/ProjectFile.h"
#include <sstream>

namespace oss {

namespace {

std::string escape(const std::string& s) {
    std::string o;
    for (char ch : s) { if (ch == '\\') o += "\\\\"; else if (ch == '\n') o += "\\n"; else o += ch; }
    return o;
}
std::string unescape(const std::string& s) {
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
std::string restOfLine(std::istringstream& ls) {
    std::string rest; std::getline(ls >> std::ws, rest); return rest;
}
void writeInput(std::string& out, int port, const Value& v) {
    const std::string p = std::to_string(port);
    switch (typeOf(v)) {
        case PortType::Float:  out += "inf " + p + " " + std::to_string(std::get<float>(v)) + "\n"; break;
        case PortType::Bool:   out += "inb " + p + " " + (std::get<bool>(v) ? "1" : "0") + "\n"; break;
        case PortType::Colour: { glm::vec4 c = std::get<glm::vec4>(v);
            out += "inc " + p + " " + std::to_string(c.x) + " " + std::to_string(c.y) + " "
                 + std::to_string(c.z) + " " + std::to_string(c.w) + "\n"; break; }
        case PortType::String: out += "ins " + p + " " + escape(std::get<std::string>(v)) + "\n"; break;
        default: break;   // runtime refs: not serialized
    }
}

} // namespace

std::string serializeProject(const ProjectDoc& d) {
    std::string out = "oss-project 1\n";
    out += "transport " + std::to_string(d.bpm) + " " + std::to_string(d.beatsPerBar) + " "
         + (d.looping ? "1" : "0") + " " + std::to_string(d.loopStartBar) + " "
         + std::to_string(d.loopEndBar) + " " + std::to_string(d.lengthBars) + "\n";
    for (const DocNode& n : d.nodes) {
        out += "node " + std::to_string(n.id) + " " + std::to_string(n.x) + " " + std::to_string(n.y) + "\n";
        out += "type " + n.type + "\n";
        for (const DocInput& di : n.inputs) writeInput(out, di.port, di.value);
        if (!n.state.empty()) out += "state " + n.state + "\n";
    }
    for (const Connection& c : d.connections)
        out += "conn " + std::to_string(c.srcNode) + " " + std::to_string(c.srcPort) + " "
             + std::to_string(c.dstNode) + " " + std::to_string(c.dstPort) + "\n";
    for (const DocAuto& a : d.autos)
        out += "auto " + std::to_string(a.nodeId) + " " + std::to_string(a.port) + " "
             + std::to_string(a.outMin) + " " + std::to_string(a.outMax) + " " + encodeCurve(a.curve) + "\n";
    return out;
}

bool parseProject(const std::string& text, ProjectDoc& out) {
    out = ProjectDoc{};
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("oss-project", 0) != 0) return false;
    DocNode* cur = nullptr;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string kw; ls >> kw;
        if (kw == "transport") {
            int looping = 0;
            ls >> out.bpm >> out.beatsPerBar >> looping >> out.loopStartBar >> out.loopEndBar >> out.lengthBars;
            if (ls.fail()) return false;
            out.looping = (looping != 0);
        } else if (kw == "node") {
            DocNode n{}; ls >> n.id >> n.x >> n.y;
            if (ls.fail()) return false;
            out.nodes.push_back(n);
            cur = &out.nodes.back();
        } else if (kw == "type") {
            if (!cur) return false;
            cur->type = restOfLine(ls);
        } else if (kw == "inf") {
            if (!cur) return false;
            int port; float v; ls >> port >> v; if (ls.fail()) return false;
            cur->inputs.push_back({port, Value(v)});
        } else if (kw == "inb") {
            if (!cur) return false;
            int port, v; ls >> port >> v; if (ls.fail()) return false;
            cur->inputs.push_back({port, Value(v != 0)});
        } else if (kw == "inc") {
            if (!cur) return false;
            int port; float r, g, b, a; ls >> port >> r >> g >> b >> a; if (ls.fail()) return false;
            cur->inputs.push_back({port, Value(glm::vec4(r, g, b, a))});
        } else if (kw == "ins") {
            if (!cur) return false;
            int port; ls >> port; if (ls.fail()) return false;
            cur->inputs.push_back({port, Value(unescape(restOfLine(ls)))});
        } else if (kw == "state") {
            if (!cur) return false;
            cur->state = restOfLine(ls);
        } else if (kw == "conn") {
            Connection c{}; ls >> c.srcNode >> c.srcPort >> c.dstNode >> c.dstPort; if (ls.fail()) return false;
            out.connections.push_back(c);
        } else if (kw == "auto") {
            DocAuto a{}; ls >> a.nodeId >> a.port >> a.outMin >> a.outMax; if (ls.fail()) return false;
            a.curve = decodeCurve(restOfLine(ls));
            out.autos.push_back(a);
        }
        // unknown keyword -> ignored (forward compatible)
    }
    return true;
}

} // namespace oss
