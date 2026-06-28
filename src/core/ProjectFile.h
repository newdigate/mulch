#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "core/Value.h"
#include "core/Connection.h"
#include "core/AutoCurve.h"
#include "core/AssetLibrary.h"

namespace oss {

class Graph;
class Node;

// --- The intermediate document a project (de)serializes through (GL-free POD). ---
struct DocInput { int port; Value value; };                 // a control-type default
struct DocNode  { int id; float x, y; std::string type;
                  std::vector<DocInput> inputs; std::string state; };
struct DocAuto  { int nodeId, port; float outMin, outMax; AutoCurve curve; };
struct ProjectDoc {
    double bpm = 120.0; int beatsPerBar = 4; bool looping = false;
    double loopStartBar = 0.0, loopEndBar = 4.0; float lengthBars = 8.0f;
    std::vector<DocNode>    nodes;
    std::vector<Connection> connections;
    std::vector<DocAuto>    autos;
    std::vector<Asset>      assets;
    std::map<std::string, glm::vec4> tagColors;
    std::string assetLibraryPath;   // referenced .osslib (empty = none / legacy embedded)
};

std::string serializeProject(const ProjectDoc& d);
bool        parseProject(const std::string& text, ProjectDoc& out);   // false on bad header/numbers

ProjectDoc  captureProject(const Graph& g);

using NodeFactory = std::function<std::unique_ptr<Node>(const std::string&)>;
using NodeInit    = std::function<void(Node&)>;
void restoreProject(const ProjectDoc& d, Graph& g, const NodeFactory& factory, const NodeInit& init);

std::string saveProject(const Graph& g);
bool        loadProject(const std::string& text, Graph& g, const NodeFactory& f, const NodeInit& i);

} // namespace oss
