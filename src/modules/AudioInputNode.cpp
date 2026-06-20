#include "modules/AudioInputNode.h"
#include "core/Preferences.h"
#include <soundio/soundio.h>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace oss {

AudioInputNode::AudioInputNode() : Node("Audio In"), block_(1 << 13, 0.0f) {
    addOutput("left",  PortType::Audio);
    addOutput("right", PortType::Audio);
    outL_.assign(block_.size(), 0.0f);
    outR_.assign(block_.size(), 0.0f);
}

AudioInputNode::~AudioInputNode() {
    closeStream();
    if (soundio_) soundio_destroy(soundio_);
}

void AudioInputNode::evaluate(EvalContext& ctx) {
    std::string want = ctx.prefs ? ctx.prefs->audioInputDeviceId : std::string();
    if (!ensureDevice(want)) {
        ctx.out<AudioRef>(0, AudioRef{});
        ctx.out<AudioRef>(1, AudioRef{});
        return;
    }
    soundio_flush_events(soundio_);

    // Drain whatever the capture thread has buffered, up to one block. `block_`
    // holds interleaved samples; trim to a whole number of frames, then
    // deinterleave into the two mono outputs (mono device duplicated to both).
    std::size_t n = ring_.pop(block_.data(), block_.size());
    n -= n % (std::size_t)channels_;
    std::size_t frames = n / (std::size_t)channels_;
    for (std::size_t f = 0; f < frames; ++f) {
        outL_[f] = block_[f * (std::size_t)channels_];
        outR_[f] = (channels_ == 2) ? block_[f * 2 + 1] : block_[f * (std::size_t)channels_];
    }
    ctx.out<AudioRef>(0, AudioRef{outL_.data(), frames, sampleRate_});
    ctx.out<AudioRef>(1, AudioRef{outR_.data(), frames, sampleRate_});
}

bool AudioInputNode::ensureDevice(const std::string& wantId) {
    if (!soundio_) {
        if (contextFailed_) return false;
        if (!openContext()) { contextFailed_ = true; return false; }
    }
    if (streamOpen_ && currentDeviceId_ == wantId) return true;
    closeStream();
    return openStream(wantId);
}

bool AudioInputNode::openContext() {
    soundio_ = soundio_create();
    if (!soundio_) return false;
    if (int err = soundio_connect(soundio_)) {
        std::fprintf(stderr, "[AudioIn] connect failed: %s\n", soundio_strerror(err));
        soundio_destroy(soundio_); soundio_ = nullptr;
        return false;
    }
    soundio_flush_events(soundio_);
    return true;
}

int AudioInputNode::findInputDeviceById(const std::string& id) {
    int n = soundio_input_device_count(soundio_);
    for (int i = 0; i < n; ++i) {
        SoundIoDevice* d = soundio_get_input_device(soundio_, i);
        bool match = d && !d->is_raw && d->id && id == d->id;
        if (d) soundio_device_unref(d);
        if (match) return i;
    }
    return -1;
}

bool AudioInputNode::openStream(const std::string& wantId) {
    soundio_flush_events(soundio_);
    int idx = wantId.empty() ? -1 : findInputDeviceById(wantId);
    if (idx < 0) idx = soundio_default_input_device_index(soundio_);
    if (idx < 0) { std::fprintf(stderr, "[AudioIn] no input device\n"); return false; }
    device_ = soundio_get_input_device(soundio_, idx);
    if (!device_) return false;

    sampleRate_ = soundio_device_nearest_sample_rate(device_, 48000);
    instream_ = soundio_instream_create(device_);
    if (!instream_) { closeStream(); return false; }
    instream_->format         = SoundIoFormatFloat32NE;
    instream_->sample_rate    = sampleRate_;
    instream_->read_callback   = &AudioInputNode::readCallback;
    instream_->error_callback  = &AudioInputNode::errorCallback;
    instream_->userdata        = this;
    instream_->name            = "shader-streamer";
    if (int err = soundio_instream_open(instream_)) {
        std::fprintf(stderr, "[AudioIn] open failed: %s\n", soundio_strerror(err));
        closeStream(); return false;
    }
    channels_ = instream_->layout.channel_count >= 2 ? 2 : 1;
    if (int err = soundio_instream_start(instream_)) {
        std::fprintf(stderr, "[AudioIn] start failed (mic access denied?): %s\n", soundio_strerror(err));
        closeStream(); return false;
    }
    currentDeviceId_ = wantId;
    streamOpen_ = true;
    std::fprintf(stderr, "[AudioIn] capturing from '%s': %d Hz, %d ch\n",
                 device_->name ? device_->name : "?", sampleRate_, channels_);
    return true;
}

void AudioInputNode::closeStream() {
    if (instream_) { soundio_instream_destroy(instream_); instream_ = nullptr; }
    if (device_)   { soundio_device_unref(device_);        device_   = nullptr; }
    streamOpen_ = false;
}

// Runs on libsoundio's real-time thread: no allocation, locks, or I/O here.
void AudioInputNode::readCallback(SoundIoInStream* is, int /*frameMin*/, int frameMax) {
    auto* self = static_cast<AudioInputNode*>(is->userdata);
    SoundIoChannelArea* areas = nullptr;
    int framesLeft = frameMax;

    while (framesLeft > 0) {
        int frameCount = framesLeft;
        if (soundio_instream_begin_read(is, &areas, &frameCount) != 0) return;
        if (frameCount <= 0) break;

        // areas == NULL means an overflow hole (dropped frames); skip its body
        // but still end_read to advance past it. Otherwise interleave the first
        // `channels_` channels and push into the ring buffer in stack-sized chunks.
        if (areas) {
            int ch = self->channels_;
            int done = 0;
            while (done < frameCount) {
                int chunk = frameCount - done;
                if (chunk > 512) chunk = 512;                // <= 512 frames -> <= 1024 floats
                float tmp[1024];
                for (int i = 0; i < chunk; ++i)
                    for (int c = 0; c < ch; ++c) {
                        const char* src = areas[c].ptr + (std::ptrdiff_t)areas[c].step * (done + i);
                        std::memcpy(&tmp[i * ch + c], src, sizeof(float));
                    }
                self->ring_.push(tmp, (std::size_t)chunk * ch);   // drops on overflow
                done += chunk;
            }
        }
        soundio_instream_end_read(is);
        framesLeft -= frameCount;
    }
}

// Swallow streaming errors so libsoundio's default handler can't abort the app.
void AudioInputNode::errorCallback(SoundIoInStream*, int) {}

} // namespace oss
