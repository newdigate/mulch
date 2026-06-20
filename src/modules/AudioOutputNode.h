#pragma once
#include <vector>
#include "core/Node.h"
#include "core/Value.h"
#include "core/LazyInit.h"
#include "audio/SpscRingBuffer.h"

// libsoundio types, kept opaque so <soundio/soundio.h> stays out of this header.
struct SoundIo;
struct SoundIoDevice;
struct SoundIoOutStream;

namespace oss {

// Audio sink: plays its connected audio input through the system's default
// output device via libsoundio. GL-free.
//
// Threading: the graph (main thread) pushes the frame's samples into a lock-free
// ring buffer in evaluate(); libsoundio drains that buffer into the device from
// its own real-time callback thread. The device is opened lazily on first
// evaluate(); if none can be opened the node becomes a silent no-op.
class AudioOutputNode : public Node {
public:
    AudioOutputNode();
    ~AudioOutputNode() override;
    void evaluate(EvalContext& ctx) override;

private:
    bool ensureStarted();                                   // lazy device open
    bool openDevice();
    static void writeCallback(SoundIoOutStream* os, int frameMin, int frameMax);
    static void errorCallback(SoundIoOutStream* os, int err);

    SoundIo*          soundio_   = nullptr;
    SoundIoDevice*    device_    = nullptr;
    SoundIoOutStream* outstream_ = nullptr;

    SpscRingBuffer<float> ring_{1 << 14};   // interleaved stereo floats; producer/consumer
    std::vector<float>    scratch_;         // preallocated; touched only on RT thread
    std::vector<float>    stereoScratch_;   // main thread: interleave left+right before push

    LazyInit lazy_;
    int  sampleRate_ = 48000;
};

} // namespace oss
