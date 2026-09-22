#pragma once
#include <glad/gl.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "core/OfflineRender.h"
#include "core/Preferences.h"
#include "core/Transport.h"
#include "gfx/Framebuffer.h"
#include "gfx/FullscreenPass.h"
#include "gfx/VideoEncoder.h"

namespace oss {

class Graph;
class OutputNode;
class AudioOutputNode;

// The offline render job. It poses as the transport's EXTERNAL CLOCK (the mechanism MIDI sync
// uses): every frame it places the transport at startBar*secondsPerBar + k/fps, evaluates the
// graph with a fixed dt = 1/fps, and -- after the discarded pre-roll -- blits the first Output
// node's texture through its own render-sized FBO into a VideoEncoder together with the first
// Audio Out node's stereo block (padded/trimmed to exactly sampleRate/fps frames, so audio can
// never drift from video). Between frames it waits while any node reports loading().
//
// start() snapshots the Transport and the graph's Preferences pointer, swaps in a copy with the
// render size (ShaderNodes recreate their FBOs from it), and sets the graph offline; finish()
// (success, failure, cancel, destructor -- exactly once per job) restores all three and closes
// the encoder. Node-internal state is NOT restored: the graph is left where the render ended.
//
// Driven incrementally: Application::frame calls step(budget) while active() so ImGui can draw
// progress; the --render CLI calls step() in a plain loop. Runs on the graph thread with the
// editor GL context current (its FBO/VAO/program live there).
class OfflineRenderer {
public:
    enum class Phase { Idle, Preroll, Rendering, Done, Failed, Cancelled };

    struct Progress {
        Phase  phase = Phase::Idle;
        long long prerollDone = 0, prerollTotal = 0;
        long long framesDone  = 0, framesTotal  = 0;   // captured frames
        long long blackFrames = 0;                     // frames with no Output texture (captured black)
        long long resizedAudioFrames = 0;              // frames whose audio block was padded/trimmed
        bool   audio = false;                          // the file has an audio track
        double elapsedSeconds = 0.0;                   // wall time since start()
        double speed = 0.0;                            // captured frames per wall second
        std::string outPath;
        std::string status;                            // "waiting for X", the outcome line, or the error
    };

    OfflineRenderer() = default;
    ~OfflineRenderer();                                // cancel() if active (a valid partial file remains)
    OfflineRenderer(const OfflineRenderer&) = delete;
    OfflineRenderer& operator=(const OfflineRenderer&) = delete;

    // Validate, find the Output / Audio Out nodes, create the blit FBO at the render size,
    // snapshot state, arm the clock. False + `err` on a bad setting, no Output node, or an FBO
    // the driver refuses. `livePrefs` may be null (nodes then fall back to their canvas size).
    bool start(Graph& g, const Preferences* livePrefs, const RenderSettings& s, std::string& err);

    // Render frames until `budgetSeconds` of wall time have elapsed. Renders at least one frame
    // per call (so progress is guaranteed, even with budget 0) UNLESS a node reports loading(),
    // in which case it yields without rendering and re-checks on the next call. Returns
    // active(); the call that renders the last frame performs finish().
    bool step(double budgetSeconds);

    void cancel();                                     // close the encoder (partial file plays), restore state
    bool active() const { return active_; }
    const Progress& progress() const { return progress_; }   // valid after the job ends too

    // Test seam: how long to wait on loading() nodes before failing (default kRenderLoadTimeoutSeconds).
    void setLoadTimeoutSeconds(double s) { loadTimeout_ = s; }

private:
    bool anyNodeLoading(std::string& who) const;
    void evaluateFrame(long long k);
    bool capture(long long k);                         // false after finish(Failed)
    bool openEncoder();
    void finish(Phase outcome, const std::string& status);
    static double now();

    Graph*             graph_     = nullptr;
    const Preferences* livePrefs_ = nullptr;
    Preferences        renderPrefs_;
    Transport          savedTransport_;
    RenderSettings     settings_;
    OutputNode*        output_    = nullptr;
    AudioOutputNode*   audioOut_  = nullptr;

    std::unique_ptr<VideoEncoder> enc_;
    Framebuffer        fbo_;                           // the render-sized blit target
    FullscreenPass     fsq_;
    GLuint             blitProg_ = 0;
    std::vector<std::uint8_t> pixels_;
    std::vector<float>        audioScratch_;
    int                audioRate_ = 0;

    long long k_ = 0;                                  // next frame index (negative = pre-roll)
    long long prerollFrames_ = 0, totalFrames_ = 0;
    double secondsPerBar_ = 2.0;
    double startTime_ = 0.0, captureStartTime_ = 0.0;
    double loadWaitStart_ = -1.0;                      // wall time the current loader wait began (-1 = none)
    double loadTimeout_ = kRenderLoadTimeoutSeconds;
    bool   active_ = false;
    Progress progress_;
};

} // namespace oss
