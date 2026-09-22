#include "app/OfflineRenderer.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include "core/Graph.h"
#include "core/PathUtil.h"
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

// Re-resolve the sinks by id each time rather than caching a Node*: Graph::clear() (a project
// load) destroys every node, so a pointer held across frames could dangle. Ids are never reused
// (Graph::addNode counts up and clear() keeps nextId_ monotonic), so a findNode hit can only be
// the original node -- the dynamic_cast cannot be fooled by a recycled id.
static OutputNode* outputNodeOf(Graph* g, int id) {
    return g ? dynamic_cast<OutputNode*>(g->findNode(id)) : nullptr;
}
static AudioOutputNode* audioOutNodeOf(Graph* g, int id) {
    return (g && id) ? dynamic_cast<AudioOutputNode*>(g->findNode(id)) : nullptr;
}

bool OfflineRenderer::openEncoder() {
    // Audio is recorded only if it is connected at the first captured frame (the Recorder's rule).
    AudioOutputNode* aout = audioOutNodeOf(graph_, audioOutId_);
    audioRate_ = (aout && !aout->lastBlock().empty()) ? aout->lastSampleRate() : 0;
    enc_ = std::make_unique<VideoEncoder>();
    std::string err;
    if (!enc_->open(settings_.outPath, settings_.width, settings_.height, settings_.fps,
                    audioRate_, audioRate_ > 0 ? 2 : 0, err)) {
        enc_.reset();
        finish(Phase::Failed, "could not open " + settings_.outPath + ": " + err);
        return false;
    }
    progress_.audio = audioRate_ > 0;
    return true;
}

bool OfflineRenderer::capture(long long k) {
    // 1. Blit the Output node's texture into the render-sized FBO (stretched, like the Output
    //    window). No texture -> black, counted, never skipped.
    // Re-resolved by id every frame: Graph::clear() (a project load) can free the node under us,
    // and the failure has to funnel through finish() like every other one -- see step()'s INVARIANT.
    OutputNode* out = outputNodeOf(graph_, outputId_);
    if (!out) {
        finish(Phase::Failed, "the Output node disappeared mid-render");
        return false;
    }
    TexRef src = out->current();
    fbo_.bind();                                          // FBO + viewport
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
    if (src.id) {
        glUseProgram(blitProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src.id);
        glUniform1i(glGetUniformLocation(blitProg_, "uTex"), 0);
        fsq_.draw();
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ++progress_.blackFrames;
    }
    // 2. Read back (bottom-up rows, what the encoder wants).
    pixels_.resize((std::size_t)settings_.width * settings_.height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, settings_.width, settings_.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
    Framebuffer::unbind();

    // 3. Encode: pts is exactly k (the encoder rounds t*fps).
    if (!enc_ && !openEncoder()) return false;
    enc_->addVideoFrame(pixels_.data(), (double)k / (double)settings_.fps);

    // 4. Audio: pad with silence / trim to exactly sampleRate/fps frames so the audio clock
    //    (sample count) can never drift from the video clock.
    if (progress_.audio) {
        AudioOutputNode* aout = audioOutNodeOf(graph_, audioOutId_);
        // An Audio Out that vanished mid-render (Graph::clear()) encodes as silence rather than
        // desyncing the track -- the sample count per frame is what keeps the two clocks locked.
        static const std::vector<float> kNoAudio;
        const std::vector<float>& blk = aout ? aout->lastBlock() : kNoAudio;
        const std::size_t want = (std::size_t)audioSamplesPerFrame(audioRate_, settings_.fps);
        const std::size_t have = blk.size() / 2;
        if (have != want) ++progress_.resizedAudioFrames;
        audioScratch_.assign(want * 2, 0.0f);
        const std::size_t n = std::min(have, want);
        std::copy(blk.begin(), blk.begin() + (std::ptrdiff_t)(n * 2), audioScratch_.begin());
        enc_->addAudio(audioScratch_.data(), (int)(want * 2));
    }
    return true;
}

bool OfflineRenderer::step(double budgetSeconds) {
    if (!active()) return false;
    // INVARIANT: the phase (which active() derives from) and the graph's offline flag are set
    // together in start() and cleared together in finish(), the ONLY place the phase leaves a
    // running state. Do not add an early return from step() that bypasses finish(): a stuck-true
    // offline flag silently mutes Audio Out, MIDI Out and the Recorder for the rest of the
    // session, with no error and no way back short of restarting. Every failure path here
    // funnels through finish(Phase::Failed, ...).
    const double t0 = now();
    while (active()) {
        // Loader gate: yield while any node is still loading (re-checked on the next call).
        std::string who;
        if (anyNodeLoading(who)) {
            const double n = now();
            if (loadWaitStart_ < 0.0) loadWaitStart_ = n;
            if (n - loadWaitStart_ > loadTimeout_) {
                finish(Phase::Failed, "timed out waiting for " + who + " to load");
                return false;
            }
            progress_.status = "waiting for " + who;
            progress_.elapsedSeconds = n - startTime_;
            return true;
        }
        loadWaitStart_ = -1.0;
        progress_.status.clear();

        if (k_ == 0) captureStartTime_ = now();
        evaluateFrame(k_);
        if (k_ >= 0 && !capture(k_)) return false;    // capture() already finished(Failed)
        if (k_ < 0) progress_.prerollDone = k_ + prerollFrames_ + 1;
        else        progress_.framesDone  = k_ + 1;
        ++k_;

        const double n = now();
        progress_.phase = k_ < 0 ? Phase::Preroll : Phase::Rendering;
        progress_.elapsedSeconds = n - startTime_;
        if (progress_.framesDone > 0 && n > captureStartTime_)
            progress_.speed = (double)progress_.framesDone / (n - captureStartTime_);

        if (k_ >= totalFrames_) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "rendered %s (%lld frames, %.1f s%s)",
                          fileBaseName(settings_.outPath).c_str(), totalFrames_,
                          (double)totalFrames_ / settings_.fps, progress_.audio ? "" : ", video only");
            std::string msg = buf;
            if (progress_.blackFrames)        msg += ", " + std::to_string(progress_.blackFrames) + " black frames";
            if (progress_.resizedAudioFrames) msg += ", " + std::to_string(progress_.resizedAudioFrames) + " audio blocks resized";
            finish(Phase::Done, msg);
            return false;
        }
        if (n - t0 >= budgetSeconds) break;             // budget spent; at least one frame was rendered
    }
    return active();
}

} // namespace oss
