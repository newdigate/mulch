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

    // Drain whatever the capture thread has buffered, up to one block.
    std::size_t n = ring_.pop(block_.data(), block_.size());
    ctx.out<AudioRef>(0, AudioRef{block_.data(), n, sampleRate_});
}

bool AudioInputNode::ensureStarted() {
    if (initTried_) return ok_;
    initTried_ = true;

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
    if (int err = soundio_instream_start(instream_)) {
        std::fprintf(stderr, "[AudioIn] start failed (mic access denied?): %s\n",
                     soundio_strerror(err));
        return false;
    }
    std::fprintf(stderr, "[AudioIn] capturing from default device: %d Hz\n", sampleRate_);
    ok_ = true;
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
        // but still end_read to advance past it. Otherwise downmix to mono by
        // taking channel 0 and push into the ring buffer in stack-sized chunks.
        if (areas) {
            int done = 0;
            while (done < frameCount) {
                int chunk = frameCount - done;
                if (chunk > 1024) chunk = 1024;
                float tmp[1024];
                for (int i = 0; i < chunk; ++i) {
                    const char* src = areas[0].ptr + (std::ptrdiff_t)areas[0].step * (done + i);
                    std::memcpy(&tmp[i], src, sizeof(float));
                }
                self->ring_.push(tmp, (std::size_t)chunk);   // drops on overflow
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
