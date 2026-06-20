#pragma once
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "audio/SpscRingBuffer.h"

// libsoundio types, kept opaque so <soundio/soundio.h> stays out of this header.
struct SoundIo;
struct SoundIoDevice;
struct SoundIoInStream;

namespace oss {

// Audio source: captures audio from the system's default input device
// (microphone / line-in) via libsoundio and publishes it as two mono AudioRef
// outputs (left, right) -- a stereo device is deinterleaved, a mono device is
// duplicated to both outputs. GL-free.
//
// Threading: libsoundio's real-time read callback pushes captured samples into a
// lock-free ring buffer; the graph (main thread) drains them each evaluate().
// The device is opened lazily on first evaluate(); if none can be opened -- e.g.
// no input device, or microphone access is denied -- the node emits silence.
class AudioInputNode : public Node {
public:
    AudioInputNode();
    ~AudioInputNode() override;
    void evaluate(EvalContext& ctx) override;

private:
    bool ensureDevice(const std::string& wantId);
    bool openContext();
    bool openStream(const std::string& wantId);
    void closeStream();
    int  findInputDeviceById(const std::string& id);
    static void readCallback(SoundIoInStream* is, int frameMin, int frameMax);
    static void errorCallback(SoundIoInStream* is, int err);

    SoundIo*         soundio_  = nullptr;
    SoundIoDevice*   device_   = nullptr;
    SoundIoInStream* instream_ = nullptr;

    SpscRingBuffer<float> ring_{1 << 14};   // RT producer (capture) -> graph consumer
    std::vector<float>    block_;           // interleaved capture (main thread only)
    std::vector<float>    outL_, outR_;     // deinterleaved mono left / right
    int  sampleRate_ = 48000;
    int  channels_   = 1;        // captured channel count (1 or 2)
    std::string currentDeviceId_;
    bool streamOpen_    = false;
    bool contextFailed_ = false;
};

} // namespace oss
