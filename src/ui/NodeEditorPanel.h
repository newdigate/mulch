#pragma once
#include <functional>
#include <memory>
#include <string>
#include "core/Graph.h"

namespace oss {

// Owns the imgui-node-editor context; renders the graph and applies user edits
// (create/delete links, delete nodes, add nodes via context menu).
class NodeEditorPanel {
public:
    NodeEditorPanel();
    ~NodeEditorPanel();

    // Draw the editor for `graph`. `addNodeOfType` is the app's factory hook,
    // invoked when the user picks a type from the background context menu.
    void draw(Graph& graph,
              const std::function<int(const std::string&, glm::vec2)>& addNodeOfType);

    // The primary selected node's id (first of the editor's selection), or -1 if none.
    // Valid after draw(); used by the Properties/Controls panels.
    int selectedNodeId() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace oss
