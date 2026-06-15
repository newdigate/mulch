#include "app/Application.h"
#include <imgui.h>
#include "modules/ColourNode.h"
#include "modules/MixNode.h"
#include "modules/OutputNode.h"
#include "modules/SpectrographNode.h"

namespace oss {

std::unique_ptr<Node> makeNode(const std::string& type) {
    if (type == "Colour")      return std::make_unique<ColourNode>();
    if (type == "Spectrograph") return std::make_unique<SpectrographNode>();
    if (type == "Mix")         return std::make_unique<MixNode>();
    if (type == "Output")      return std::make_unique<OutputNode>();
    return nullptr;
}

const std::vector<std::string>& nodeTypeNames() {
    static const std::vector<std::string> names = { "Colour", "Spectrograph", "Mix", "Output" };
    return names;
}

Application::Application(GLFWwindow* window) : window_(window) {
    // Seed a small starting graph (Colour -> Output) so the canvas isn't empty
    // on launch. The user can add/connect/delete nodes freely from here.
    int c = addNodeOfType("Colour", {40, 40});
    int o = addNodeOfType("Output", {360, 40});
    graph_.connect(c, 0, o, 0);
}

Application::~Application() = default;

int Application::addNodeOfType(const std::string& type, glm::vec2 pos) {
    auto node = makeNode(type);
    if (!node) return -1;
    node->initGL();
    node->pos = pos;
    return graph_.addNode(std::move(node));
}

void Application::frame(float dt) {
    editor_.draw(graph_, [this](const std::string& t, glm::vec2 p){ return addNodeOfType(t, p); });
    graph_.evaluate(dt);
    drawViewer();
}

void Application::drawViewer() {
    ImGui::Begin("Viewer");
    TexRef tex{};
    for (auto& n : graph_.nodes())
        if (auto* o = dynamic_cast<OutputNode*>(n.get())) { tex = o->current(); break; }
    if (tex.id) {
        float avail = ImGui::GetContentRegionAvail().x;
        float aspect = tex.h ? (float)tex.h / (float)tex.w : 0.5625f;
        ImVec2 size(avail, avail * aspect);
        // Flip V: FBO origin is bottom-left, ImGui expects top-left.
        ImGui::Image((ImTextureID)(intptr_t)tex.id, size, ImVec2(0, 1), ImVec2(1, 0));
    } else {
        ImGui::TextUnformatted("No output texture.");
    }
    ImGui::End();
}

} // namespace oss
