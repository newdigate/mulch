#include "modules/AudioInputNode.h"
#include <soundio/soundio.h>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace oss {

AudioInputNode::AudioInputNode() : Node("Audio In"), block_(1 << 13, 0.0f) {
    addOutput("audio", PortType::Audio);
}

AudioInputNode::~AudioInputNode() {
    // Destroy the stream FIRST so the real-time read callback stops before the
    // ring buffer it writes to is gone (members outlive this body).
    if (instream_) soundio_instream_destroy(instream_);
    if (device_)   soundio_device_unref(device_);
    if (soundio_)  soundio_destroy(soundio_);
}

void AudioInputNode::evaluate(EvalContext& ctx) {
    if (!ensureStarted()) { ctx.out<AudioRef>(0, AudioRef{}); return; }   // silence
    soundio_flush_events(soundio_);

    // Drain whatever the capture thread has buffered, up to one block. `block_`
    // holds interleaved samples; trim to a whole number of frames.
    std::size_t n = ring_.pop(block_.data(), block_.size());
    n -= n % (std::size_t)channels_;
    ctx.out<AudioRef>(0, AudioRef{block_.data(), n, sampleRate_, channels_});
}

bool AudioInputNode::ensureStarted() {
    return lazy_.ensure([this] { return openDevice(); });
}

bool AudioInputNode::openDevice() {
    soundio_ = soundio_create();
    if (!soundio_) return false;
    if (int err = soundio_connect(soundio_)) {
        std::fprintf(stderr, "[AudioIn] connect failed: %s\n", soundio_strerror(err));
        return false;
    }
    soundio_flush_events(soundio_);

    int idx = soundio_default_input_device_index(soundio_);
    if (idx < 0) { std::fprintf(stderr, "[AudioIn] no input device\n"); return false; }
    device_ = soundio_get_input_device(soundio_, idx);
    if (!device_) return false;

    sampleRate_ = soundio_device_nearest_sample_rate(device_, 48000);

    instream_ = soundio_instream_create(device_);
    if (!instream_) return false;
    instream_->format         = SoundIoFormatFloat32NE;
    instream_->sample_rate    = sampleRate_;
    instream_->read_callback  = &AudioInputNode::readCallback;
    instream_->error_callback = &AudioInputNode::errorCallback;
    instream_->userdata       = this;
    instream_->name           = "shader-streamer";

    if (int err = soundio_instream_open(instream_)) {
        std::fprintf(stderr, "[AudioIn] open failed: %s\n", soundio_strerror(err));
        return false;
    }
    channels_ = instream_->layout.channel_count >= 2 ? 2 : 1;   // capture up to stereo
    if (int err = soundio_instream_start(instream_)) {
        std::fprintf(stderr, "[AudioIn] start failed (mic access denied?): %s\n",
                     soundio_strerror(err));
        return false;
    }
    std::fprintf(stderr, "[AudioIn] capturing from default device: %d Hz, %d ch\n",
                 sampleRate_, channels_);
    return true;
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
