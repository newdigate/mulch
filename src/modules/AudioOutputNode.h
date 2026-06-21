#pragma once
#include <string>
#include <vector>
#include <memory>
#include "core/Node.h"
#include "core/Value.h"
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
    bool ensureDevice(const std::string& wantId, int wantBufferMs);   // open context (once) + ensure the right stream
    bool openContext();
    bool openStream(const std::string& wantId, int wantBufferMs);
    void closeStream();
    int  findOutputDeviceById(const std::string& id);
    static void writeCallback(SoundIoOutStream* os, int frameMin, int frameMax);
    static void errorCallback(SoundIoOutStream* os, int err);

    SoundIo*          soundio_   = nullptr;
    SoundIoDevice*    device_    = nullptr;
    SoundIoOutStream* outstream_ = nullptr;

    std::unique_ptr<SpscRingBuffer<float>> ring_;   // sized from Preferences::audioBufferMs at open
    std::vector<float>    scratch_;
    std::vector<float>    stereoScratch_;
    int  sampleRate_ = 48000;
    std::string currentDeviceId_;       // id the stream is currently open on ("" = default)
    int  currentBufferMs_ = -1;          // buffer ms the stream is currently open with
    bool streamOpen_    = false;
    bool contextFailed_ = false;        // no audio context -> stay a silent no-op
};

} // namespace oss
