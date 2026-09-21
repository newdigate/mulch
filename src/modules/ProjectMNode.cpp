#include "modules/ProjectMNode.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include "core/PathUtil.h"
#include "core/Preferences.h"
#include "gfx/Canvas.h"
#include "gfx/GLStateGuard.h"

namespace oss {

namespace {
// projectM resolves its GL entry points through this; GLFW knows the current context's loader.
void* glLoadProc(const char* name, void* /*userData*/) {
    return reinterpret_cast<void*>(glfwGetProcAddress(name));
}

// projectM renders from whatever GL state it is handed: it sets no viewport for a burn and no blend
// state for a copy, and nothing of scissor/cull/depth (its whole source tree only ever touches
// GL_BLEND and GL_LINE_SMOOTH). In this app "whatever it is handed" is what the previous node left,
// so put the enables GLStateGuard saves into GL's defaults -- the state every standalone projectM
// host renders from. Call INSIDE a GLStateGuard scope, which restores them.
void enterForeignDefaults() {
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
}
} // namespace

ProjectMNode::ProjectMNode() : Node("projectM") {
    addInput("left",  PortType::Audio,   AudioRef{});
    addInput("right", PortType::Audio,   AudioRef{});
    addInput("texture in", PortType::Texture, TexRef{});
    addInput("burn", PortType::Float, 0.0f, 0.0f, 1.0f);              // gate: > 0.5 burns every frame
    addAssetInput("preset", AssetType::Preset);
    addInput("blend", PortType::Bool, true);                          // smooth transition vs hard cut
    addInput("blend time", PortType::Float, 3.0f, 0.0f, 10.0f);       // seconds
    addInput("beat sensitivity", PortType::Float, 1.0f, 0.0f, 2.0f);
    addIntInput("mesh", 48, 8, 128);                                  // columns; rows = 3/4
    addInput("sync", PortType::Bool, false);
    addIntInput("bars", 4, 1, 64);
    addInput("shuffle", PortType::Bool, false);
    addOutput("texture", PortType::Texture);
}

ProjectMNode::~ProjectMNode() {
    if (pm_) { GLStateGuard guard; ProjectMApi::instance().destroy(pm_); }
}

std::string ProjectMNode::buttonLabel(int i) const {
    static const char* kLabels[3] = {"prev", "next", "random"};
    return (i >= 0 && i < 3) ? kLabels[i] : "";
}

void ProjectMNode::initGL() {
    fbo_.create(kCanvasW, kCanvasH);
    clearToBlack();
    ensureInstance(kCanvasW, kCanvasH);
}

void ProjectMNode::clearToBlack() {
    GLStateGuard guard;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_.id());
    glDisable(GL_SCISSOR_TEST);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    glClearBufferfv(GL_COLOR, 0, black);            // doesn't touch the global clear colour
}

void ProjectMNode::ensureInstance(int w, int h) {
    ProjectMApi& api = ProjectMApi::instance();
    if (pm_ || createFailed_ || !api.available()) return;
    GLStateGuard guard;
    enterForeignDefaults();
    pm_ = api.create(&glLoadProc, nullptr);
    if (!pm_) { createFailed_ = true; return; }
    api.setWindowSize(pm_, (std::size_t)w, (std::size_t)h);
    api.setPresetLocked(pm_, true);                 // the node decides every preset change
    api.setAspectCorrection(pm_, true);
    api.setFps(pm_, 60);
    api.setSwitchFailedCallback(pm_, &ProjectMNode::onSwitchFailed, this);
}

void ProjectMNode::onSwitchFailed(const char* file, const char* message, void* self) {
    auto* n = static_cast<ProjectMNode*>(self);
    n->failMsg_ = "failed: " + fileBaseName(file ? file : "") + ": " + (message ? message : "");
}

void ProjectMNode::pushParams(EvalContext& ctx) {
    ProjectMApi& api = ProjectMApi::instance();
    float blendTime = ctx.in<float>(kBlendTime);
    if (blendTime != lastBlendTime_) { lastBlendTime_ = blendTime; api.setSoftCutDuration(pm_, (double)blendTime); }
    float beat = ctx.in<float>(kBeatSens);
    if (beat != lastBeatSens_) { lastBeatSens_ = beat; api.setBeatSensitivity(pm_, beat); }
    int cols = std::min(128, std::max(8, (int)std::lround(ctx.in<float>(kMesh))));
    if (cols != lastMesh_) {
        lastMesh_ = cols;
        int rows = std::max(2, (int)std::lround(cols * 0.75));
        GLStateGuard guard;                         // a mesh resize reallocates GL buffers
        enterForeignDefaults();
        api.setMeshSize(pm_, (std::size_t)cols, (std::size_t)rows);
    }
}

void ProjectMNode::updateSearchPaths(const EvalContext& ctx) {
    std::string dir = parentDir(sel_.current());
    std::string tex = ctx.prefs ? ctx.prefs->projectMTexturesDir : std::string();
    if (pathsSet_ && dir == searchDir_ && tex == searchTex_) return;
    pathsSet_ = true; searchDir_ = dir; searchTex_ = tex;
    std::vector<const char*> paths;
    if (!searchDir_.empty()) paths.push_back(searchDir_.c_str());
    if (!searchTex_.empty()) paths.push_back(searchTex_.c_str());
    const char* none = nullptr;
    ProjectMApi::instance().setTextureSearchPaths(pm_, paths.empty() ? &none : paths.data(), paths.size());
}

void ProjectMNode::evaluate(EvalContext& ctx) {
    ProjectMApi& api = ProjectMApi::instance();
    const int w = ctx.prefs ? ctx.prefs->textureWidth  : kCanvasW;
    const int h = ctx.prefs ? ctx.prefs->textureHeight : kCanvasH;
    const bool resized = (fbo_.width() != w || fbo_.height() != h);
    if (resized) { fbo_.create(w, h); clearToBlack(); }

    // Preset selection runs even when inert, so the field and buttons behave the same everywhere.
    PresetSelectorInput in;
    in.incoming    = ctx.in<std::string>(kPreset);
    in.button      = pendingButton_;  pendingButton_ = -1;
    in.sync        = ctx.in<bool>(kSync);
    in.playing     = ctx.transport && ctx.transport->playing;
    in.bars        = ctx.transport ? ctx.transport->bars() : 0.0;
    in.everyNBars  = std::max(1, (int)std::lround(ctx.in<float>(kBars)));
    in.shuffle     = ctx.in<bool>(kShuffle);
    in.randomValue = (unsigned)rng_();
    if (sel_.update(in)) inputDefault(kPreset) = Value(sel_.current());   // the field shows what plays

    ensureInstance(w, h);
    if (!pm_) {
        status_ = createFailed_ ? std::string("projectM failed to initialise") : api.statusText();
        ctx.out<TexRef>(0, TexRef{ fbo_.texture(), fbo_.width(), fbo_.height() });
        return;
    }
    if (resized) api.setWindowSize(pm_, (std::size_t)w, (std::size_t)h);

    pushParams(ctx);
    updateSearchPaths(ctx);                          // before the load, so its textures resolve

    if (sel_.current() != loadedPath_) {             // also true right after a lazy create
        loadedPath_ = sel_.current();
        failMsg_.clear();
        GLStateGuard guard;
        enterForeignDefaults();
        api.loadPresetFile(pm_, loadedPath_.empty() ? "idle://" : loadedPath_.c_str(), ctx.in<bool>(kBlend));
    }

    // Audio: two mono edges -> interleaved LRLR. A lone connected side is mirrored (as the Recorder).
    AudioRef l = ctx.in<AudioRef>(kLeft), r = ctx.in<AudioRef>(kRight);
    const AudioRef& effL = (l.samples && l.count > 0) ? l : r;
    const AudioRef& effR = (r.samples && r.count > 0) ? r : l;
    if (effL.samples && effL.count > 0 && effR.samples) {
        std::size_t n = std::min(effL.count, effR.count);
        pcm_.resize(n * 2);
        for (std::size_t i = 0; i < n; ++i) { pcm_[2 * i] = effL.samples[i]; pcm_[2 * i + 1] = effR.samples[i]; }
        api.pcmAddFloat(pm_, pcm_.data(), (unsigned int)n, kPmStereo);
    }

    {
        GLStateGuard guard;
        enterForeignDefaults();
        TexRef tin = ctx.in<TexRef>(kTexIn);
        if (ctx.in<float>(kBurn) > 0.5f && tin.id != 0) {
            // The burn draws a full-NDC quad into the preset's framebuffer and sets neither a
            // viewport nor a blend state of its own (ProjectM::BurnInTexture -> CopyTexture::Copy),
            // so it inherits whatever the node before us left and would land in a corner of the
            // canvas. Pin both: the full canvas, and STRAIGHT-alpha blending, so an opaque texture
            // replaces the canvas and a transparent one composites over it. No flip is needed --
            // projectM's copy mesh maps v=1 to NDC +y, which is a GL bottom-up texture's top row.
            glViewport(0, 0, w, h);
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            api.burnTexture(pm_, tin.id, 0, 0, w, h);
            glDisable(GL_BLEND);
        }
        api.setFrameTime(pm_, time_);                // the app's clock, 0.0 on the first frame
        api.renderFrameFbo(pm_, fbo_.id());
    }
    time_ += ctx.dt;

    if (!failMsg_.empty()) {
        status_ = failMsg_;
    } else if (sel_.current().empty()) {
        status_ = "projectM " + api.versionText() + " \xC2\xB7 no preset";
    } else {
        std::string name = fileBaseName(sel_.current());
        if (name.size() > 5) name.resize(name.size() - 5);                 // drop ".milk"
        status_ = "projectM " + api.versionText() + " \xC2\xB7 " + name;
        if (sel_.index() >= 0)
            status_ += " (" + std::to_string(sel_.index() + 1) + "/" + std::to_string(sel_.count()) + ")";
    }
    ctx.out<TexRef>(0, TexRef{ fbo_.texture(), fbo_.width(), fbo_.height() });
}

} // namespace oss
