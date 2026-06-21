#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/AsyncLoader.h"
#include "audio/AudioFile.h"
#include "audio/AudioBlock.h"

namespace oss {

// Plays an audio file (named by its string input) as 48 kHz, deinterleaved to two
// mono outputs (left, right). The file is
// decoded into memory on a worker thread; once loaded, a source-time playhead is
// advanced by rate*dt each frame and the output block is read from the clip with
// linear interpolation. The signed `rate` gives variable speed and, when
// negative, reverse playback (it reads the clip backwards). `play` pauses; `loop`
// wraps at the ends. GL-free.
class AudioPlayerNode : public Node {
public:
    AudioPlayerNode();
    void evaluate(EvalContext& ctx) override;
    std::string statusLine() const override { return status_; }

    // Test/inspection accessors.
    double   playhead() const { return playhead_; }
    AudioRef leftOut()  const { return AudioRef{outL_.data(), (std::size_t)lastN_, sampleRate_}; }
    AudioRef rightOut() const { return AudioRef{outR_.data(), (std::size_t)lastN_, sampleRate_}; }

private:
    void emitAudio(EvalContext& ctx, double t0, double t1);
    void updateStatus(bool play, float rate);

    AsyncLoader<AudioClip> loader_;            // worker-thread decode, keyed on path
    AudioClip   clip_;
    bool        haveClip_ = false;
    std::string status_;
    int         sampleRate_ = 48000;
    double      playhead_   = 0.0;             // source position in seconds
    double      duration_   = 0.0;

    std::vector<float> outL_, outR_;           // mono left / right (kAudioMaxBlock each)
    int         lastN_ = 0;                    // samples emitted last evaluate
};

} // namespace oss
