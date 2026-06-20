# Project Save / Load — Design

**Date:** 2026-06-20
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

Save the whole project to a file and load it back: every node (type, canvas position, all
control values), every connection, the transport (tempo + loop), and the automation (the
right-click UI automation channels **and** the Automation node's internal curves). A
hand-rolled, GL-free, line-based `.oss` text format keeps `src/core/` dependency-free and
unit-testable; the toolbar gets **Save** / **Load** buttons + a filename field.

## Architecture

A GL-free `core/ProjectFile` module does all serialization. It funnels through an
intermediate `ProjectDoc` POD so that **parse → rebuild is atomic** (a malformed file is
rejected before the live graph is touched) and so serialize/parse are testable without a
graph. Rebuilding needs `makeNode` (app) + `initGL` (GL), so `loadProject` takes those as
callbacks — the core stays GL-free. One `Node` hook covers the only editable state that is
not an input port (the Automation node's curves).

### File format (`.oss`, version 1)

Flat, one keyword per line; the last field of `type`/`ins`/`state`/`auto` is the **rest of
the line**, so names and paths with spaces need no quoting. Newlines inside string values
are escaped `\n` (and `\` as `\\`).

```
oss-project 1
transport 120 4 0 0 4 8          # bpm beatsPerBar looping(0/1) loopStartBar loopEndBar lengthBars
node 1 40 60                     # id x y   (id is the in-file id)
type Sine                        # rest-of-line = makeNode key (== node->name())
inf 0 220                        # float input:  port value
inf 1 0.8
node 2 300 60
type Audio Out
state 0,0.2|2,0.4||1,0.9         # rest-of-line = this node's saveState() (attaches to the node above; only the Automation node emits one)
conn 1 0 2 0                     # srcNode srcPort dstNode dstPort  (in-file ids)
auto 3 5 0 1 0,0.2,2,0.8,4,0.5   # nodeId port outMin outMax  inline-curve(bar,val,bar,val,…)
```
- Input lines, one per **control-type** default (refs are skipped — the node ctor recreates them):
  `inf <port> <float>`, `inb <port> <0|1>`, `inc <port> <r> <g> <b> <a>`, `ins <port> <string>`.
- `type`/`in*`/`state` attach to the most recent `node`; `conn`/`auto` are top-level (in-file ids).
- `auto`/`state`/`conn` use the in-file node ids; load remaps them.

### Unit 1 — small enablers (existing core files)

- **`core/AutoCurve.h`**: add inline `std::string encodeCurve(const AutoCurve&)` (comma-joined
  `bar,value` pairs; empty curve → `""`) and `AutoCurve decodeCurve(const std::string&)`.
  Shared by the `auto` lines and the Automation node's state. Unit-tested.
- **`core/Node.h`**: add `virtual std::string saveState() const { return {}; }` and
  `virtual void loadState(const std::string&) {}`. Default empty (every port-only node).
- **`src/modules/AutomationNode.h`**: override them — encode its 4 `curve_[]` as
  `encodeCurve(c0)|encodeCurve(c1)|encodeCurve(c2)|encodeCurve(c3)` and decode the reverse.
- **`core/Graph.{h,cpp}`**: add `void clear()` — drop `nodes_`/`connections_`/`outputs_`,
  reset `nextId_ = 1` and `orderDirty_`, and clear the automation store.
- **`core/AutomationStore`**: add `void clear()` (drop all channels; reset `lengthBars` to its
  default) if it lacks one, used by `Graph::clear()`.

### Unit 2 — `core/ProjectFile.{h,cpp}` (new, GL-free, unit-tested)

```cpp
struct DocInput { int port; Value value; };                 // control value only
struct DocNode  { int id; float x, y; std::string type;
                  std::vector<DocInput> inputs; std::string state; };
struct DocAuto  { int nodeId, port; float outMin, outMax; AutoCurve curve; };
struct ProjectDoc {
    double bpm = 120; int beatsPerBar = 4; bool looping = false;
    double loopStartBar = 0, loopEndBar = 4; float lengthBars = 8;
    std::vector<DocNode> nodes;
    std::vector<Connection> connections;
    std::vector<DocAuto> autos;
};

std::string serializeProject(const ProjectDoc&);
bool        parseProject(const std::string&, ProjectDoc&);   // false on bad header/numbers
ProjectDoc  captureProject(const Graph&);                    // GL-free: reads the graph + saveState()

using NodeFactory = std::function<std::unique_ptr<Node>(const std::string&)>;  // makeNode
using NodeInit    = std::function<void(Node&)>;                                // n.initGL()
void restoreProject(const ProjectDoc&, Graph&, const NodeFactory&, const NodeInit&);

// Convenience:
std::string saveProject(const Graph& g);                                       // serialize(capture)
bool        loadProject(const std::string&, Graph&, const NodeFactory&, const NodeInit&); // parse then restore
```

- **`captureProject`**: for each node — `id()`, `pos`, `name()`, every input whose default is a
  control type (Float/Bool/Colour/String) → a `DocInput`, and `saveState()`. Plus
  `connections()`, transport fields, `automation().lengthBars()`, and each
  `automation().channels()` entry → a `DocAuto`.
- **`serializeProject`**: emit the format above. The control-Value codec writes `inf/inb/inc/ins`
  by `typeOf(value)`.
- **`parseProject`**: read line by line into a `ProjectDoc`; reject a missing/garbled
  `oss-project 1` header or unparseable numbers (return false, no side effects).
- **`restoreProject`**: `graph.clear()`, then for each `DocNode`: `factory(type)` (if it returns
  null — unknown type — skip the node and remember it's absent), set `pos`, apply each
  `DocInput` to `inputDefault(port)` **only when `port` is in range and the value's `PortType`
  matches the port's type**, `loadState(state)`, `init(node)`, `addNode` → record
  `fileId → newId`. Then add `connections` and `autos` remapped through that map (skip any
  referencing an absent node). Restore transport fields + `automation().setLengthBars()`;
  leave `seconds = 0`, `playing = false`. For each `DocAuto`: `ch = automation().add(graph,
  newNodeId, port); if (ch) { ch->curve = a.curve; ch->outMin = a.outMin; ch->outMax = a.outMax; }`.

### Unit 3 — App + UI (GL)

- **`Application`**: `bool saveProjectToFile(const std::string& path)` writes
  `saveProject(graph_)` to `path`; `bool loadProjectFromFile(const std::string& path)` reads the
  file and calls `loadProject(text, graph_, [this](auto& t){ return makeNode(t); },
  [](Node& n){ n.initGL(); })`. Keep a short `projectStatus_` string ("saved <path>" /
  "loaded <path>" / "save failed" / "load failed"). Uses `<fstream>`.
- **`TransportBar`**: extend `drawTransportBar` to also take the filename buffer + save/load
  callbacks (e.g. `drawTransportBar(Transport&, char* filenameBuf, size_t bufLen, const
  std::function<void()>& onSave, const std::function<void()>& onLoad, const std::string&
  status)`). Inside the existing main menu bar add a separator, an `InputText("##file",
  filenameBuf, bufLen)` (default `project.oss`), **Save** and **Load** buttons calling the
  callbacks, and the status text. The app owns the `char filename_[256]` buffer + binds the
  callbacks to its save/load methods.

### CMake

- Add `src/core/ProjectFile.cpp` to `APP_SOURCES`, `core_tests`, and `gl_smoke` (it's GL-free
  core, needed by the app, the unit test, and the gl_smoke round-trip).
- Add `tests/test_project_file.cpp` to `core_tests`. `AutoCurve.h` stays header-only.

## Data flow

```
Save:  Graph + Transport + AutomationStore ──captureProject──► ProjectDoc ──serializeProject──► .oss text ──► file
Load:  file ──► .oss text ──parseProject──► ProjectDoc ──restoreProject(makeNode, initGL)──► rebuilt Graph
```

## Edge cases

- **Malformed file** → `parseProject` returns false *before* any graph mutation; the open
  graph is untouched; status shows "load failed".
- **Unknown node type** (older/newer build) → that node is skipped; connections/automation
  referencing it are skipped; the rest loads. (Soft error, not a failed load.)
- **Node definition changed** (port count/types differ) → out-of-range or type-mismatched
  saved inputs are skipped; in-range matching ones apply.
- **In-file ids non-dense / out of order** (nodes were deleted before save) → handled by the
  `fileId → newId` remap; never assume ids equal positions.
- **String values with spaces** → fine (rest-of-line); embedded newlines are escaped `\n`.
- **Automation depends on nodes** → `restoreProject` adds nodes first, then channels (so
  `automation().add` can seed from the now-present control), then overwrites the curve/range.
- **Runtime refs** (Tex/Audio/Midi/Vertex/Transform/Shader input defaults) → not serialized;
  recreated empty by the node constructor (textures/VBOs regenerate at evaluate).

## Testing

- **`tests/test_project_file.cpp`** (`core_tests`, GL-free):
  - `encodeCurve`/`decodeCurve` round-trip (incl. empty curve, multi-point).
  - `serializeProject`→`parseProject` round-trip on a hand-built `ProjectDoc` (all input kinds,
    a connection, an automation channel, transport tweaks) → equal doc.
  - Full `captureProject`→`serializeProject`→`parseProject`→`restoreProject` round-trip using
    a couple of in-test **stub** `Node` subclasses (Float/Bool/Colour/String inputs; one stub
    overrides `saveState/loadState`) and a stub `NodeFactory`/no-op `NodeInit`: assert the
    reloaded graph matches — node names, `pos`, input defaults, connections, transport, the
    automation channel (node/port/range/curve), and the restored node state.
  - `parseProject` returns false on a bad header and on garbage numerics (graph untouched).
  - Unknown-type and out-of-range-input are skipped without aborting the load.
- **`gl_smoke`**: build a real graph via `makeNode` (e.g. `Colour → Output`, plus a node with a
  tweaked Float control), `saveProject` to `build/_project.oss`, `loadProject` into the same
  graph with `makeNode` + `n.initGL()`, and assert the reconstructed graph (node count, the
  tweaked control value, the connection survive) — exercising the real factory + GL-init path.

## Docs

- **README.md** — a short **Save / Load** note: the toolbar Save/Load + filename field writes/
  reads a `.oss` project file (nodes, positions, control values, connections, transport,
  automation).
- **CLAUDE.md** — an Architecture bullet: project save/load lives in the GL-free
  `core/ProjectFile` (`.oss` line-based text, via a `ProjectDoc`; `loadProject` takes a
  `makeNode` factory + an `initGL` callback so core stays GL-free); the `Node::saveState/
  loadState` hook captures non-port state (only the Automation node uses it); `Graph::clear()`;
  curve codec in `core/AutoCurve.h`.
</content>
