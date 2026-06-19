#include "app/Application.h"
#include "modules/ColourNode.h"
#include "modules/ArpeggiatorNode.h"
#include "modules/ChordPlayerNode.h"
#include "modules/AutomationNode.h"
#include "modules/AudioInputNode.h"
#include "modules/AudioPlayerNode.h"
#include "modules/AcidNode.h"
#include "modules/AudioMixerNode.h"
#include "modules/AudioOutputNode.h"
#include "modules/MeshLoaderNode.h"
#include "modules/MidiInputNode.h"
#include "modules/MidiMergeNode.h"
#include "modules/MidiOutputNode.h"
#include "modules/MidiFilePlayerNode.h"
#include "modules/PitchGraphNode.h"
#include "modules/MixNode.h"
#include "modules/CompositorNode.h"
#include "modules/LfoNode.h"
#include "modules/OutputNode.h"
#include "modules/RecorderNode.h"
#include "modules/ShadedRenderNode.h"
#include "modules/SineWaveNode.h"
#include "modules/SpectrographNode.h"
#include "modules/OscilloscopeNode.h"
#include "modules/StepSequencerNode.h"
#include "modules/TextNode.h"
#include "modules/WorldTransformNode.h"
#include "modules/VideoPlayerNode.h"
#include "modules/WireframeNode.h"
#include "ui/TransportBar.h"

namespace oss {

std::unique_ptr<Node> makeNode(const std::string& type) {
    if (type == "Colour")      return std::make_unique<ColourNode>();
    if (type == "Video")       return std::make_unique<VideoPlayerNode>();
    if (type == "Sine")        return std::make_unique<SineWaveNode>();
    if (type == "Acid Bass")   return std::make_unique<AcidNode>();
    if (type == "Audio In")    return std::make_unique<AudioInputNode>();
    if (type == "Audio File")  return std::make_unique<AudioPlayerNode>();
    if (type == "Audio Mix")   return std::make_unique<AudioMixerNode>();
    if (type == "Spectrograph") return std::make_unique<SpectrographNode>();
    if (type == "Oscilloscope") return std::make_unique<OscilloscopeNode>();
    if (type == "Mix")         return std::make_unique<MixNode>();
    if (type == "Compositor")  return std::make_unique<CompositorNode>();
    if (type == "Mesh Loader") return std::make_unique<MeshLoaderNode>();
    if (type == "Text 2D")     return std::make_unique<Text2DNode>();
    if (type == "Text 3D")     return std::make_unique<Text3DNode>();
    if (type == "World Transform") return std::make_unique<WorldTransformNode>();
    if (type == "Wireframe")   return std::make_unique<WireframeNode>();
    if (type == "Shaded Render") return std::make_unique<ShadedRenderNode>();
    if (type == "Recorder")    return std::make_unique<RecorderNode>();
    if (type == "Output")      return std::make_unique<OutputNode>();
    if (type == "Audio Out")   return std::make_unique<AudioOutputNode>();
    if (type == "MIDI In")     return std::make_unique<MidiInputNode>();
    if (type == "MIDI File")   return std::make_unique<MidiFilePlayerNode>();
    if (type == "Chord Player") return std::make_unique<ChordPlayerNode>();
    if (type == "Pitch Graph")  return std::make_unique<PitchGraphNode>();
    if (type == "Step Seq")    return std::make_unique<StepSequencerNode>();
    if (type == "Arpeggiator") return std::make_unique<ArpeggiatorNode>();
    if (type == "MIDI Merge")  return std::make_unique<MidiMergeNode>();
    if (type == "MIDI Out")    return std::make_unique<MidiOutputNode>();
    if (type == "Automation")  return std::make_unique<AutomationNode>();
    if (type == "LFO")         return std::make_unique<LfoNode>();
    return nullptr;
}

const std::vector<NodeCategory>& nodeCategories() {
    static const std::vector<NodeCategory> cats = {
        { "Texture", { "Colour", "Video", "Mix", "Compositor", "Recorder", "Output" } },
        { "Audio",   { "Sine", "Acid Bass", "Audio File", "Audio In", "Audio Mix", "Spectrograph", "Oscilloscope", "Audio Out" } },
        { "MIDI",    { "MIDI In", "MIDI File", "Step Seq", "Chord Player", "Arpeggiator", "MIDI Merge", "MIDI Out", "Pitch Graph" } },
        { "3D",      { "Mesh Loader", "Text 2D", "Text 3D", "World Transform", "Wireframe", "Shaded Render" } },
        { "Control", { "Automation", "LFO" } },
    };
    return cats;
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
    drawTransportBar(graph_.transport());   // top toolbar: tempo + play/stop/scrub
    editor_.draw(graph_, [this](const std::string& t, glm::vec2 p){ return addNodeOfType(t, p); });
    automation_.draw(graph_);                // automation timeline window
    graph_.evaluate(dt);                     // advances the transport by dt
}

// The first OutputNode's current texture, shown in the dedicated output window.
TexRef Application::outputTexture() const {
    for (auto& n : graph_.nodes())
        if (auto* o = dynamic_cast<OutputNode*>(n.get())) return o->current();
    return TexRef{};
}

} // namespace oss
