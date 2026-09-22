#include "app/OfflineRenderer.h"
#include <chrono>
#include <cstdio>
#include "core/Graph.h"
#include "gfx/GLUtil.h"
#include "modules/AudioOutputNode.h"
#include "modules/OutputNode.h"

namespace oss {

// Fullscreen-triangle blit into the render FBO. NO vertical flip: FBO textures are bottom-up
// and VideoEncoder::addVideoFrame expects bottom-up rows (it flips for encoding), exactly like
// the Recorder's direct read-back. (The Output window's blit flips because it draws to a screen.)
static const char* kBlitVS = R"(#version 410 core
out vec2 vUV;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    vUV = (p + 1.0) * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
static const char* kBlitFS = R"(#version 410 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
void main() { FragColor = texture(uTex, vUV); }
)";

double OfflineRenderer::now() {
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double>(clk::now().time_since_epoch()).count();
}

OfflineRenderer::~OfflineRenderer() {
    cancel();
    if (blitProg_) glDeleteProgram(blitProg_);
}

bool OfflineRenderer::start(Graph& g, const RenderSettings& s, std::string& err) {
    // A second start() while a job is running is rejected without disturbing THAT job's
    // progress()/active() -- unlike the failures below, there is no stale "previous job" for
    // progress_ to describe here; it is already reporting the live one. (Routing this through
    // the same reset-progress_ path as the failures below would set phase = Failed on a job
    // that is still genuinely running, which -- now that active() is derived from phase --
    // would make active() false out from under it while nothing had actually stopped it.)
    if (active()) { err = "a render is already running"; return false; }

    // Any failure from here on means no earlier attempt's progress_ should linger: reset it to
    // Failed so progress() and `err` never disagree about the latest attempt. outPath is kept
    // (not just wiped by Progress{}) so a failure dialog still has a path to name.
    auto reject = [&](const std::string& why) {
        progress_ = Progress{};
        progress_.phase   = Phase::Failed;
        progress_.status  = why;
        progress_.outPath = s.outPath;
        err = why;
        return false;
    };

    OutputNode* out = nullptr; AudioOutputNode* aout = nullptr;
    for (const auto& n : g.nodes()) {
        if (!out)  out  = dynamic_cast<OutputNode*>(n.get());
        if (!aout) aout = dynamic_cast<AudioOutputNode*>(n.get());
    }
    if (!validateRenderSettings(s, out != nullptr, err)) return reject(err);

    // GL objects live in the editor context (current on the graph thread). The FBO is
    // re-created per job at the render size; Framebuffer::create reports completeness itself.
    if (!blitProg_) { blitProg_ = linkProgram(kBlitVS, kBlitFS); fsq_.create(); }
    if (!fbo_.create(s.width, s.height)) {
        return reject("could not create a " + std::to_string(s.width) + "x" + std::to_string(s.height)
                     + " framebuffer");
    }

    graph_      = &g;
    livePrefs_  = g.preferences();     // the ONE source for both the render copy below and the restore
    settings_   = s;
    outputId_   = out->id();           // validated non-null above
    audioOutId_ = aout ? aout->id() : 0;
    savedTransport_ = g.transport();
    renderPrefs_ = livePrefs_ ? *livePrefs_ : Preferences{};
    renderPrefs_.textureWidth  = s.width;
    renderPrefs_.textureHeight = s.height;
    g.setPreferences(&renderPrefs_);
    g.setOffline(true);

    Transport& t = g.transport();
    t.externalClock = true;          // advance() becomes a no-op; we place the position ourselves
    t.playing       = true;          // synced nodes run
    t.looping       = false;         // linear start -> finish
    renderClock_    = t;             // pin the WHOLE armed clock; evaluateFrame re-asserts it verbatim
    secondsPerBar_  = t.secondsPerBar();
    prerollFrames_  = prerollFrameCount(s, secondsPerBar_);
    totalFrames_    = renderFrameCount(s, secondsPerBar_);
    k_              = -prerollFrames_;
    enc_.reset();
    audioRate_ = 0;

    progress_ = Progress{};
    progress_.phase        = prerollFrames_ > 0 ? Phase::Preroll : Phase::Rendering;
    progress_.prerollTotal = prerollFrames_;
    progress_.framesTotal  = totalFrames_;
    progress_.outPath      = s.outPath;
    startTime_ = captureStartTime_ = now();
    loadWaitStart_ = -1.0;
    std::fprintf(stderr, "[Render] %s: bars %.2f-%.2f, %lld frames at %d fps, %dx%d, pre-roll %lld\n",
                 s.outPath.c_str(), s.startBar, s.endBar, totalFrames_, s.fps, s.width, s.height, prerollFrames_);
    return true;
}

void OfflineRenderer::finish(Phase outcome, std::string status) {
    if (!active()) return;
    if (enc_) {
        std::string e;
        if (!enc_->close(e) && outcome == Phase::Done) {
            // A file that failed to finalise is not a success, however many frames were
            // captured -- an unflushed/untrailered mp4 is unplayable, and the CLI's exit code
            // (phase == Done ? 0 : 1) is the only signal a batch pipeline gets.
            outcome = Phase::Failed;
            status  = "could not finalise " + settings_.outPath + ": " + e;
        }
        enc_.reset();
    }
    graph_->setOffline(false);
    graph_->setPreferences(livePrefs_);
    graph_->transport() = savedTransport_;
    graph_ = nullptr;                  // guard against any accidental post-job use
    progress_.phase          = outcome;
    progress_.status         = status;
    progress_.elapsedSeconds = now() - startTime_;
    std::fprintf(stderr, "[Render] %s\n", status.c_str());
}

void OfflineRenderer::cancel() {
    if (!active()) return;
    finish(Phase::Cancelled, "cancelled after " + std::to_string(progress_.framesDone) + " frames");
}

bool OfflineRenderer::anyNodeLoading(std::string& who) const {
    for (const auto& n : graph_->nodes())
        if (n->loading()) { who = n->name() + " #" + std::to_string(n->id()); return true; }
    return false;
}

void OfflineRenderer::evaluateFrame(long long k) {
    Transport& t = graph_->transport();
    t = renderClock_;                                  // re-assert the WHOLE armed clock verbatim
    t.seconds = renderFrameSeconds(settings_, secondsPerBar_, k);
    graph_->evaluate(1.0f / (float)settings_.fps);
}

bool OfflineRenderer::openEncoder() { return false; }   // Task 7
bool OfflineRenderer::capture(long long) { return false; }   // Task 7
bool OfflineRenderer::step(double)  { return false; }   // Task 7

} // namespace oss
