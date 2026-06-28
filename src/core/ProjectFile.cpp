#include "core/ProjectFile.h"
#include <sstream>
#include "core/Graph.h"
#include "core/Node.h"
#include "core/AutomationStore.h"
#include "core/AssetLibrary.h"
#include "core/TextCodec.h"
#include "core/AssetLibraryFile.h"
#include <unordered_map>

namespace oss {

namespace {

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
    if (!d.assetLibraryPath.empty()) out += "assetlib " + escape(d.assetLibraryPath) + "\n";
    appendAssetBlock(out, d.assets, d.tagColors);
    return out;
}

bool parseProject(const std::string& text, ProjectDoc& out) {
    out = ProjectDoc{};
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("oss-project", 0) != 0) return false;
    DocNode* cur = nullptr;
    Asset* curAsset = nullptr;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string kw; ls >> kw;
        if (parseAssetBlockLine(kw, ls, out.assets, out.tagColors, curAsset)) continue;
        if (kw == "assetlib") { out.assetLibraryPath = unescape(restOfLine(ls)); continue; }
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

ProjectDoc captureProject(const Graph& g) {
    ProjectDoc d;
    const Transport& t = g.transport();
    d.bpm = t.bpm; d.beatsPerBar = t.beatsPerBar; d.looping = t.looping;
    d.loopStartBar = t.loopStartBar; d.loopEndBar = t.loopEndBar;
    d.lengthBars = g.automation().lengthBars();
    for (const auto& np : g.nodes()) {
        DocNode dn; dn.id = np->id(); dn.x = np->pos.x; dn.y = np->pos.y; dn.type = np->name();
        const std::vector<Port>& ins = np->inputs();
        for (std::size_t i = 0; i < ins.size(); ++i) {
            PortType pt = typeOf(ins[i].defaultValue);
            if (pt == PortType::Float || pt == PortType::Bool || pt == PortType::Colour || pt == PortType::String)
                dn.inputs.push_back({(int)i, ins[i].defaultValue});
        }
        dn.state = np->saveState();
        d.nodes.push_back(std::move(dn));
    }
    for (const Connection& c : g.connections()) d.connections.push_back(c);
    for (const UiAutomationChannel& ch : g.automation().channels())
        d.autos.push_back({ch.nodeId, ch.port, ch.outMin, ch.outMax, ch.curve});
    return d;
}

void restoreProject(const ProjectDoc& d, Graph& g, const NodeFactory& factory, const NodeInit& init) {
    g.clear();
    g.assets().load(d.assets);
    g.assets().loadTagColors(d.tagColors);
    std::unordered_map<int, int> idMap;
    for (const DocNode& dn : d.nodes) {
        std::unique_ptr<Node> node = factory(dn.type);
        if (!node) continue;                                  // unknown type -> skip
        node->pos = glm::vec2(dn.x, dn.y);
        for (const DocInput& di : dn.inputs) {
            if (di.port >= 0 && (std::size_t)di.port < node->inputs().size()) {
                Value& slot = node->inputDefault((std::size_t)di.port);
                if (typeOf(slot) == typeOf(di.value)) slot = di.value;   // type-safe set
            }
        }
        node->loadState(dn.state);
        init(*node);
        idMap[dn.id] = g.addNode(std::move(node));
    }
    for (const Connection& c : d.connections) {
        auto s = idMap.find(c.srcNode), t = idMap.find(c.dstNode);
        if (s != idMap.end() && t != idMap.end()) g.connect(s->second, c.srcPort, t->second, c.dstPort);
    }
    Transport& tr = g.transport();
    tr.bpm = d.bpm; tr.beatsPerBar = d.beatsPerBar; tr.looping = d.looping;
    tr.loopStartBar = d.loopStartBar; tr.loopEndBar = d.loopEndBar;
    tr.seconds = 0.0; tr.playing = false;
    g.automation().setLengthBars(d.lengthBars);
    for (const DocAuto& a : d.autos) {
        auto it = idMap.find(a.nodeId);
        if (it == idMap.end()) continue;
        UiAutomationChannel* ch = g.automation().add(g, it->second, a.port);
        if (ch) { ch->curve = a.curve; ch->outMin = a.outMin; ch->outMax = a.outMax; }
    }
}

std::string saveProject(const Graph& g) { return serializeProject(captureProject(g)); }

bool loadProject(const std::string& text, Graph& g, const NodeFactory& f, const NodeInit& i) {
    ProjectDoc d;
    if (!parseProject(text, d)) return false;
    restoreProject(d, g, f, i);
    return true;
}

} // namespace oss
