#include "app/Application.h"
#include "ui/DockLayout.h"
#include <imgui_internal.h>   // ImGuiDockNode / DockBuilderGetNode for the first-run/reset check
#include "modules/ColourNode.h"
#include "modules/ImageStreamerNode.h"
#include "modules/ArpeggiatorNode.h"
#include "modules/ChordPlayerNode.h"
#include "modules/AutomationNode.h"
#include "modules/AudioInputNode.h"
#include "modules/AudioPlayerNode.h"
#include "modules/DrumMachineNode.h"
#include "modules/AcidNode.h"
#include "modules/AudioMixerNode.h"
#include "modules/MonoToStereoNode.h"
#include "modules/StereoToMonoNode.h"
#include "modules/CrossoverFilterNode.h"
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
#include "modules/SkyboxNode.h"
#include "modules/VertexShaderNode.h"
#include "modules/DeformNode.h"
#include "modules/VertexTrailNode.h"
#include "modules/VideoPlayerNode.h"
#include "modules/WireframeNode.h"
#include "ui/TransportBar.h"
#include "ui/FileDialog.h"
#include "core/ProjectFile.h"
#include "core/PathUtil.h"
#include "core/AssetLibraryFile.h"
#include <fstream>
#include <sstream>

namespace oss {

std::unique_ptr<Node> makeNode(const std::string& type) {
    if (type == "Colour")      return std::make_unique<ColourNode>();
    if (type == "Image Streamer") return std::make_unique<ImageStreamerNode>();
    if (type == "Video")       return std::make_unique<VideoPlayerNode>();
    if (type == "Sine")        return std::make_unique<SineWaveNode>();
    if (type == "Acid Bass")   return std::make_unique<AcidNode>();
    if (type == "Audio In")    return std::make_unique<AudioInputNode>();
    if (type == "Audio File")  return std::make_unique<AudioPlayerNode>();
    if (type == "Drum Machine") return std::make_unique<DrumMachineNode>();
    if (type == "Audio Mix")   return std::make_unique<AudioMixerNode>();
    if (type == "Mono to Stereo") return std::make_unique<MonoToStereoNode>();
    if (type == "Stereo to Mono") return std::make_unique<StereoToMonoNode>();
    if (type == "Crossover Filter") return std::make_unique<CrossoverFilterNode>();
    if (type == "Spectrograph") return std::make_unique<SpectrographNode>();
    if (type == "Oscilloscope") return std::make_unique<OscilloscopeNode>();
    if (type == "Mix")         return std::make_unique<MixNode>();
    if (type == "Compositor")  return std::make_unique<CompositorNode>();
    if (type == "Mesh Loader") return std::make_unique<MeshLoaderNode>();
    if (type == "Text 2D")     return std::make_unique<Text2DNode>();
    if (type == "Text 3D")     return std::make_unique<Text3DNode>();
    if (type == "World Transform") return std::make_unique<WorldTransformNode>();
    if (type == "Wireframe")   return std::make_unique<WireframeNode>();
    if (type == "Vertex Trail") return std::make_unique<VertexTrailNode>();
    if (type == "Shaded Render") return std::make_unique<ShadedRenderNode>();
    if (type == "Skybox")        return std::make_unique<SkyboxNode>();
    if (type == "Vertex Shader") return std::make_unique<VertexShaderNode>();
    if (type == "Deform")        return std::make_unique<DeformNode>();
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
        { "Texture", { "Colour", "Image Streamer", "Video", "Mix", "Compositor", "Kaleidoscope", "Recorder", "Output" } },
        { "Audio",   { "Sine", "Acid Bass", "Audio File", "Audio In", "Audio Mix", "Mono to Stereo", "Stereo to Mono", "Crossover Filter", "Spectrograph", "Oscilloscope", "Drum Machine", "Audio Out" } },
        { "MIDI",    { "MIDI In", "MIDI File", "Step Seq", "Chord Player", "Arpeggiator", "MIDI Merge", "MIDI Out", "Pitch Graph" } },
        { "3D",      { "Mesh Loader", "Text 2D", "Text 3D", "World Transform", "Wireframe", "Shaded Render", "Skybox", "Vertex Trail" } },
        { "Control", { "Automation", "LFO" } },
        { "Shader",  { "Vertex Shader", "Deform" } },
    };
    return cats;
}

Application::Application(GLFWwindow* window) : window_(window) {
    // Seed a small starting graph (Colour -> Output) so the canvas isn't empty
    // on launch. The user can add/connect/delete nodes freely from here.
    int c = addNodeOfType("Colour", {40, 40});
    int o = addNodeOfType("Output", {360, 40});
    graph_.connect(c, 0, o, 0);
    loadPreferences();
    graph_.setPreferences(&prefs_);
}

Application::~Application() = default;

int Application::addNodeOfType(const std::string& type, glm::vec2 pos) {
    auto node = makeNode(type);
    if (!node) return -1;
    node->initGL();
    node->pos = pos;
    return graph_.addNode(std::move(node));
}

bool Application::saveProjectToFile(const std::string& path) {
    ProjectDoc d = captureProject(graph_);
    d.assetLibraryPath = currentLibraryPath_;
    std::ofstream f(path);
    if (!f) return false;
    f << serializeProject(d);
    if (!f) return false;
    if (!currentLibraryPath_.empty()) saveLibraryToFile(currentLibraryPath_);   // keep the bound library in sync
    return true;
}

bool Application::saveLibraryToFile(const std::string& path) {
    std::ofstream f(path);
    if (!f) return false;
    f << serializeLibrary(graph_.assets());
    return (bool)f;
}

bool Application::loadLibraryFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    return parseLibrary(ss.str(), graph_.assets());
}

void Application::saveLibraryAs() {
    std::string defName = currentLibraryPath_.empty() ? std::string("library.osslib")
                                                      : fileBaseName(currentLibraryPath_);
    std::string path = saveFileDialog("Save Asset Library", "Asset Library", {"osslib"},
                                      defName, prefs_.assetLibraryDir);
    if (path.empty()) return;
    path = ensureExtension(path, "osslib");
    if (saveLibraryToFile(path)) { currentLibraryPath_ = path; projectStatus_ = "library saved " + fileBaseName(path); }
    else                          projectStatus_ = "library save failed";
}

bool Application::saveLibraryOrPrompt() {
    if (currentLibraryPath_.empty()) {
        saveLibraryAs();
        return !currentLibraryPath_.empty();           // false if the user cancelled the prompt
    }
    if (saveLibraryToFile(currentLibraryPath_)) { projectStatus_ = "library saved " + fileBaseName(currentLibraryPath_); return true; }
    projectStatus_ = "library save failed";
    return false;
}

void Application::openLibraryDialog() {
    std::string path = openFileDialog("Open Asset Library", "Asset Library", {"osslib"}, prefs_.assetLibraryDir);
    if (path.empty()) return;
    if (loadLibraryFromFile(path)) { currentLibraryPath_ = path; projectStatus_ = "library opened " + fileBaseName(path); }
    else                            projectStatus_ = "library open failed";
}

void Application::loadPreferences() {
    std::ifstream f("preferences.oss");
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    parsePreferences(ss.str(), prefs_);    // bad/missing file -> prefs_ stays default
}

void Application::savePreferences() {
    std::ofstream f("preferences.oss");
    if (f) f << serializePreferences(prefs_);
}

bool Application::loadProjectFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    ProjectDoc d;
    if (!parseProject(ss.str(), d)) return false;
    restoreProject(d, graph_,
                   [](const std::string& t){ return makeNode(t); },
                   [](Node& n){ n.initGL(); });      // loads legacy embedded assets if present
    if (!d.assetLibraryPath.empty() && loadLibraryFromFile(d.assetLibraryPath)) {
        currentLibraryPath_ = d.assetLibraryPath;
    } else {
        if (!d.assetLibraryPath.empty())
            projectStatus_ = "library not found: " + fileBaseName(d.assetLibraryPath);
        currentLibraryPath_ = "";                    // unbound (missing reference, or legacy/none)
    }
    return true;
}

void Application::frame(float dt) {
    ProjectBarIO io;
    io.onSave   = [this]{ saveCurrentOrPrompt(); };
    io.onSaveAs = [this]{ saveProjectAs(); };
    io.onLoad   = [this]{ loadProjectDialog(); };
    io.onLibOpen   = [this]{ openLibraryDialog(); };
    io.onLibSave   = [this]{ saveLibraryOrPrompt(); };
    io.onLibSaveAs = [this]{ saveLibraryAs(); };
    io.onLibRemap = [this]{ assets_.openRemap(); };
    io.onResetLayout = [this]{ wantResetLayout_ = true; };
    io.status = projectStatus_;
    io.showPreferences = &showPreferences_;
    io.showAssets = &showAssets_;
    drawTransportBar(graph_.transport(), &io);   // top toolbar: tempo + play/stop/scrub

    // Host dockspace for the editor panels. DockSpaceOverViewport creates the node when
    // submitted, so a fresh run (no imgui.ini) shows an empty node -> build the default;
    // a restored-from-ini node is non-empty and is left alone. Reset forces a rebuild.
    ImGuiID dockId = beginDockHost();
    ImGuiDockNode* dockNode = ImGui::DockBuilderGetNode(dockId);
    if (wantResetLayout_ || dockNode == nullptr || dockNode->IsEmpty()) {
        buildDefaultDockLayout(dockId);
        wantResetLayout_ = false;
    }

    editor_.draw(graph_, [this](const std::string& t, glm::vec2 p){ return addNodeOfType(t, p); });
    automation_.draw(graph_);                // automation timeline window
    preferences_.draw(prefs_, [this]{ savePreferences(); }, &showPreferences_);
    assets_.draw(graph_.assets(), &showAssets_, prefs_.assetLibraryDir);
    syncEngine_.update(graph_.transport(), prefs_, dt);   // MIDI clock sync in/out
    graph_.evaluate(dt);                     // advances the transport by dt
}

// The first OutputNode's current texture, shown in the dedicated output window.
TexRef Application::outputTexture() const {
    for (auto& n : graph_.nodes())
        if (auto* o = dynamic_cast<OutputNode*>(n.get())) return o->current();
    return TexRef{};
}

void Application::saveProjectAs() {
    // A non-empty but unsaved library must become a file first, so the project can reference it.
    if (!graph_.assets().all().empty() && currentLibraryPath_.empty()) {
        if (!saveLibraryOrPrompt()) return;          // user cancelled the library Save-As -> abort
    }
    std::string defName = currentPath_.empty() ? std::string("project.oss")
                                               : fileBaseName(currentPath_);
    std::string path = saveFileDialog("Save Project", "Project", {"oss"}, defName, prefs_.projectsDir);
    if (path.empty()) return;                       // cancelled
    path = ensureExtension(path, "oss");
    if (saveProjectToFile(path)) { currentPath_ = path; projectStatus_ = "saved " + fileBaseName(path); }
    else                          projectStatus_ = "save failed";
}

void Application::saveCurrentOrPrompt() {
    // A non-empty but unsaved library must become a file first, so the project can reference it.
    if (!graph_.assets().all().empty() && currentLibraryPath_.empty()) {
        if (!saveLibraryOrPrompt()) return;          // user cancelled the library Save-As -> abort
    }
    if (currentPath_.empty()) { saveProjectAs(); return; }   // untitled -> prompt
    if (saveProjectToFile(currentPath_)) projectStatus_ = "saved " + fileBaseName(currentPath_);
    else                                 projectStatus_ = "save failed";
}

void Application::loadProjectDialog() {
    std::string path = openFileDialog("Load Project", "Project", {"oss"}, prefs_.projectsDir);
    if (path.empty()) return;                       // cancelled
    if (loadProjectFromFile(path)) { currentPath_ = path; projectStatus_ = "loaded " + fileBaseName(path); }
    else                            projectStatus_ = "load failed";
}

} // namespace oss
