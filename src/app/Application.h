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

    void frame(float dt);             // build the editor UI and evaluate the graph
    TexRef outputTexture() const;     // the OutputNode's current texture (for the output window)
    Graph& graph() { return graph_; }

private:
    GLFWwindow* window_;   // reserved for framebuffer-size / HiDPI queries
    Graph graph_;
    NodeEditorPanel editor_;
};

// Factory used by the app and the add-node menu.
std::unique_ptr<Node> makeNode(const std::string& type);

// A named group of node-type labels, used to lay out the add-node menu as
// submenus (Texture / Audio / MIDI / 3D) instead of one long flat list.
struct NodeCategory { std::string name; std::vector<std::string> types; };
const std::vector<NodeCategory>& nodeCategories();

} // namespace oss
