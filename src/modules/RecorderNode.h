#pragma once
#include <glad/gl.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "core/Node.h"
#include "gfx/VideoEncoder.h"

namespace oss {

// Inline audio+video recorder. It passes its video (input 0) and audio (input 1)
// straight through to the matching outputs, so it can sit between two modules
// without changing the signal. While the `record` toggle is on it also writes the
// passing video frames and audio to a movie file (`file`, e.g. an .mp4): each
// frame it reads back the input texture's pixels and feeds them, plus the audio
// block, to a VideoEncoder. Audio is recorded only if it is connected when
// recording starts.
class RecorderNode : public Node {
public:
    RecorderNode();
    ~RecorderNode() override;
    void evaluate(EvalContext& ctx) override;
    std::string statusLine() const override { return status_; }

private:
    void start(const std::string& file, const TexRef& vin, const AudioRef& ain);
    void stop();

    std::unique_ptr<VideoEncoder> enc_;
    bool        recording_  = false;
    double      recordTime_ = 0.0;
    long        frames_     = 0;
    int         encW_ = 0, encH_ = 0;          // dimensions recording was opened at
    std::string file_;
    std::string status_ = "idle";
    std::vector<std::uint8_t> pixbuf_;          // texture read-back scratch
};

} // namespace oss
