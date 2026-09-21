#pragma once
#include <glad/gl.h>
#include <random>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/PresetPlaylist.h"
#include "gfx/Framebuffer.h"
#include "gfx/ProjectMApi.h"

namespace oss {

// Runs the projectM (Milkdrop-compatible) visualizer: audio in -> texture out. libprojectM 4.2+ is
// loaded at runtime through ProjectMApi; when it is unavailable the node is inert (black texture +
// a status line) so projects that use it still open. The `preset` file is the single source of
// truth; its folder is the playlist for the prev/next/random buttons and the bar-synced step
// (PresetSelector). `texture in` is burned into projectM's canvas while `burn` > 0.5.
class ProjectMNode : public Node {
public:
    enum In { kLeft = 0, kRight, kTexIn, kBurn, kPreset, kBlend, kBlendTime,
              kBeatSens, kMesh, kSync, kBars, kShuffle };

    ProjectMNode();
    ~ProjectMNode() override;      // destroys the projectM instance (editor GL context current)

    void initGL() override;
    void evaluate(EvalContext& ctx) override;
    std::string statusLine() const override { return status_; }

    int         buttonCount() const override { return 3; }
    std::string buttonLabel(int i) const override;
    void        onButtonPressed(int i) override { pendingButton_ = i; }

private:
    void ensureInstance(int w, int h);              // lazy: also covers a library found after startup
    void pushParams(EvalContext& ctx);
    void updateSearchPaths(const EvalContext& ctx);
    void clearToBlack();
    static void onSwitchFailed(const char* file, const char* message, void* self);

    Framebuffer      fbo_;
    PmHandle         pm_ = nullptr;
    bool             createFailed_ = false;
    PresetSelector   sel_;
    std::minstd_rand rng_{std::random_device{}()};  // the random button only; not reproducible
    int              pendingButton_ = -1;
    double           time_ = 0.0;                   // accumulated ctx.dt -> projectm_set_frame_time
    float            lastBlendTime_ = -1.0f, lastBeatSens_ = -1.0f;
    int              lastMesh_ = -1;
    bool             pathsSet_ = false;
    std::string      loadedPath_, searchDir_, searchTex_;
    std::vector<float> pcm_;                        // interleaved LRLR scratch
    std::string      status_, failMsg_;
};

} // namespace oss
