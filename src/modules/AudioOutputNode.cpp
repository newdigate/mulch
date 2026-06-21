#include "modules/AudioOutputNode.h"
#include "core/Preferences.h"
#include "audio/AudioBlock.h"
#include <soundio/soundio.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace oss {

AudioOutputNode::AudioOutputNode() : Node("Audio Out") {
    addInput("left",  PortType::Audio, AudioRef{});
    addInput("right", PortType::Audio, AudioRef{});
}

AudioOutputNode::~AudioOutputNode() {
    closeStream();                       // destroys the stream (stops the RT callback) then unrefs the device
    if (soundio_) soundio_destroy(soundio_);
}

void AudioOutputNode::evaluate(EvalContext& ctx) {
    std::string want = ctx.prefs ? ctx.prefs->audioOutputDeviceId : std::string();
    int wantMs       = ctx.prefs ? ctx.prefs->audioBufferMs : 150;
    if (!ensureDevice(want, wantMs)) return;   // no device -> silent no-op
    soundio_flush_events(soundio_);        // pump device events (non-blocking)

    AudioRef l = ctx.in<AudioRef>(0);
    AudioRef r = ctx.in<AudioRef>(1);
    // Symmetric mirror: a single connected side feeds both speakers, so a lone
    // mono wire just works. The ring carries interleaved stereo (L,R,L,R).
    const AudioRef& effL = (l.samples && l.count > 0) ? l : r;
    const AudioRef& effR = (r.samples && r.count > 0) ? r : l;
    std::size_t nL = effL.samples ? effL.count : 0;
    std::size_t nR = effR.samples ? effR.count : 0;
    std::size_t n  = std::max(nL, nR);
    if (n == 0) return;                                  // nothing connected -> silence
    stereoScratch_.resize(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        stereoScratch_[i * 2]     = (i < nL) ? effL.samples[i] : 0.0f;
        stereoScratch_[i * 2 + 1] = (i < nR) ? effR.samples[i] : 0.0f;
    }
    if (ring_) ring_->push(stereoScratch_.data(), n * 2);   // overflow dropped, never blocks
}

bool AudioOutputNode::ensureDevice(const std::string& wantId, int wantBufferMs) {
    if (!soundio_) {
        if (contextFailed_) return false;
        if (!openContext()) { contextFailed_ = true; return false; }
    }
    if (streamOpen_ && currentDeviceId_ == wantId && currentBufferMs_ == wantBufferMs) return true;
    closeStream();
    return openStream(wantId, wantBufferMs);
}

bool AudioOutputNode::openContext() {
    soundio_ = soundio_create();
    if (!soundio_) return false;
    if (int err = soundio_connect(soundio_)) {
        std::fprintf(stderr, "[AudioOut] connect failed: %s\n", soundio_strerror(err));
        soundio_destroy(soundio_); soundio_ = nullptr;
        return false;
    }
    soundio_flush_events(soundio_);
    return true;
}

int AudioOutputNode::findOutputDeviceById(const std::string& id) {
    int n = soundio_output_device_count(soundio_);
    for (int i = 0; i < n; ++i) {
        SoundIoDevice* d = soundio_get_output_device(soundio_, i);
        bool match = d && !d->is_raw && d->id && id == d->id;
        if (d) soundio_device_unref(d);
        if (match) return i;
    }
    return -1;
}

bool AudioOutputNode::openStream(const std::string& wantId, int wantBufferMs) {
    soundio_flush_events(soundio_);
    int idx = wantId.empty() ? -1 : findOutputDeviceById(wantId);
    if (idx < 0) idx = soundio_default_output_device_index(soundio_);
    if (idx < 0) { std::fprintf(stderr, "[AudioOut] no output device\n"); return false; }
    device_ = soundio_get_output_device(soundio_, idx);
    if (!device_) return false;

    sampleRate_ = soundio_device_nearest_sample_rate(device_, 48000);
    ring_ = std::make_unique<SpscRingBuffer<float>>(audioRingFloats(wantBufferMs, sampleRate_));
    outstream_ = soundio_outstream_create(device_);
    if (!outstream_) { closeStream(); return false; }
    outstream_->format         = SoundIoFormatFloat32NE;
    outstream_->sample_rate    = sampleRate_;
    outstream_->write_callback = &AudioOutputNode::writeCallback;
    outstream_->error_callback = &AudioOutputNode::errorCallback;
    outstream_->userdata       = this;
    outstream_->name           = "shader-streamer";
    if (int err = soundio_outstream_open(outstream_)) {
        std::fprintf(stderr, "[AudioOut] open failed: %s\n", soundio_strerror(err));
        closeStream(); return false;
    }
    scratch_.assign(4096, 0.0f);
    if (int err = soundio_outstream_start(outstream_)) {
        std::fprintf(stderr, "[AudioOut] start failed: %s\n", soundio_strerror(err));
        closeStream(); return false;
    }
    currentDeviceId_ = wantId;
    currentBufferMs_ = wantBufferMs;
    streamOpen_ = true;
    std::fprintf(stderr, "[AudioOut] playing on '%s': %d Hz, %d ch\n",
                 device_->name ? device_->name : "?", sampleRate_, outstream_->layout.channel_count);
    return true;
}

void AudioOutputNode::closeStream() {
    if (outstream_) { soundio_outstream_destroy(outstream_); outstream_ = nullptr; }
    if (device_)    { soundio_device_unref(device_);         device_    = nullptr; }
    streamOpen_ = false;
}

// Runs on libsoundio's real-time thread: no allocation, locks, or I/O here.
void AudioOutputNode::writeCallback(SoundIoOutStream* os, int /*frameMin*/, int frameMax) {
    auto* self = static_cast<AudioOutputNode*>(os->userdata);
    if (!self->ring_) return;
    const SoundIoChannelLayout* layout = &os->layout;
    SoundIoChannelArea* areas = nullptr;
    int framesLeft = frameMax;             // fill as much as the device will accept

    while (framesLeft > 0) {
        int frameCount = framesLeft;
        if (soundio_outstream_begin_write(os, &areas, &frameCount) != 0) return;
        if (frameCount <= 0) break;

        // Pull interleaved stereo frames from the ring in scratch-sized chunks and
        // map L/R onto the device channels (ch0=L, ch1=R, extra channels = mix).
        // An empty ring yields silence.
        const int scratchFrames = (int)self->scratch_.size() / 2;
        int filled = 0;
        while (filled < frameCount) {
            int want = frameCount - filled;
            if (want > scratchFrames) want = scratchFrames;
            std::size_t got = self->ring_->pop(self->scratch_.data(), (std::size_t)want * 2);
            int gotFrames = (int)(got / 2);
            for (int i = 0; i < want; ++i) {
                float L = (i < gotFrames) ? self->scratch_[i * 2]     : 0.0f;
                float R = (i < gotFrames) ? self->scratch_[i * 2 + 1] : 0.0f;
                int frame = filled + i;
                for (int ch = 0; ch < layout->channel_count; ++ch) {
                    float v = (ch == 0) ? L : (ch == 1) ? R : 0.5f * (L + R);
                    char* dst = areas[ch].ptr + (std::ptrdiff_t)areas[ch].step * frame;
                    std::memcpy(dst, &v, sizeof(float));
                }
            }
            filled += want;
        }
        soundio_outstream_end_write(os);
        framesLeft -= frameCount;
    }
}

// Swallow streaming errors so libsoundio's default handler (which calls abort())
// can't take the whole app down. Empty on purpose -- runs on the RT thread.
void AudioOutputNode::errorCallback(SoundIoOutStream*, int) {}

} // namespace oss
