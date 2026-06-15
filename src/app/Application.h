#pragma once
#include <memory>
#include <string>
#include <vector>
#include <glm/vec2.hpp>
#include "core/Graph.h"
#include "ui/NodeEditorPanel.h"

struct GLFWwindow;

namespace oss {

class Application {
public:
    explicit Application(GLFWwindow* window);
    ~Application();

    // Create a node of the given type, run its GL setup, add it to the graph.
    int addNodeOfType(const std::string& type, glm::vec2 pos);

    void frame(float dt);   // build UI, evaluate graph, draw viewer
    Graph& graph() { return graph_; }

private:
    void drawViewer();

    GLFWwindow* window_;   // reserved for framebuffer-size / HiDPI queries
    Graph graph_;
    NodeEditorPanel editor_;
};

// Factory used by the app and (later) the add-node menu.
std::unique_ptr<Node> makeNode(const std::string& type);
const std::vector<std::string>& nodeTypeNames();

} // namespace oss
