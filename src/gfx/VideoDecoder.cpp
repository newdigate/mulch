#include "gfx/VideoDecoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace oss {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// FFmpeg polls this during blocking I/O; returning 1 aborts the read.
int abortRequested(void* opaque) {
    const auto* flag = static_cast<const std::atomic<bool>*>(opaque);
    return flag && flag->load(std::memory_order_relaxed) ? 1 : 0;
}

// sws_scale_frame() writes only into reference-counted frames, so convert() wraps the caller's
// buffer in a reference whose free callback leaves the memory alone.
void keepBuffer(void*, std::uint8_t*) {}

} // namespace

DecodedFrame::~DecodedFrame() { reset(); }

DecodedFrame::DecodedFrame(DecodedFrame&& o) noexcept : t(o.t), frame_(o.frame_) { o.frame_ = nullptr; }

DecodedFrame& DecodedFrame::operator=(DecodedFrame&& o) noexcept {
    if (this != &o) {
        reset();
        t = o.t;
        frame_ = o.frame_;
        o.frame_ = nullptr;
    }
    return *this;
}

void DecodedFrame::reset() {
    if (frame_) av_frame_free(&frame_);
}

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close() {
    first_.reset();
    clearQueue();
    if (sws_)      { sws_freeContext(sws_); sws_ = nullptr; }
    if (swsThr_)   { sws_freeContext(swsThr_); swsThr_ = nullptr; }
    swsThrFmt_ = -1;
    if (swr_)      { swr_free(&swr_); }
    if (vctx_)     avcodec_free_context(&vctx_);
    if (actx_)     avcodec_free_context(&actx_);
    if (pkt_)      av_packet_free(&pkt_);
    if (frame_)    av_frame_free(&frame_);
    if (dstFrame_) av_frame_free(&dstFrame_);
    if (fmt_)      avformat_close_input(&fmt_);
    vstream_ = astream_ = -1;
    width_ = height_ = 0;
    audioChannels_ = 0;
    duration_ = vTimeBase_ = aTimeBase_ = startT_ = 0.0;
    resetStreamState();
}

void VideoDecoder::resetStreamState() {
    demuxEof_ = vflushed_ = false;
    demuxedT_ = audioEndT_ = -kInf;
    pendingAudio_.clear();
    pendingAudioStart_ = 0.0;
}

void VideoDecoder::clearQueue() {
    for (AVPacket* p : vq_) av_packet_free(&p);
    vq_.clear();
    queuedBytes_ = 0;
}

bool VideoDecoder::open(const std::string& path, std::string& err, const std::atomic<bool>* abort) {
    close();

    fmt_ = avformat_alloc_context();
    if (!fmt_) { err = "out of memory"; return false; }
    if (abort) {
        fmt_->interrupt_callback.callback = abortRequested;
        fmt_->interrupt_callback.opaque   = const_cast<std::atomic<bool>*>(abort);
    }
    if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) {   // frees fmt_ on failure
        err = "could not open file"; return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        err = "could not read stream info"; close(); return false;
    }

    vstream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vstream_ < 0) { err = "no video stream"; close(); return false; }
    astream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);  // may be < 0

    // --- Video decoder ---
    AVStream* vs = fmt_->streams[vstream_];
    const AVCodec* vcodec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!vcodec) { err = "unsupported video codec"; close(); return false; }
    vctx_ = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(vctx_, vs->codecpar);
    vctx_->thread_count = 0;   // automatic: a decode thread per core (libavcodec defaults to one)
    if (avcodec_open2(vctx_, vcodec, nullptr) < 0) {
        err = "could not open video decoder"; close(); return false;
    }
    width_     = vctx_->width;
    height_    = vctx_->height;
    vTimeBase_ = av_q2d(vs->time_base);
    if (width_ <= 0 || height_ <= 0) { err = "video has no dimensions"; close(); return false; }

    sws_ = sws_getContext(width_, height_, vctx_->pix_fmt,
                          width_, height_, AV_PIX_FMT_RGBA,
                          SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) { err = "could not init colour converter"; close(); return false; }
    rgba_.assign((std::size_t)width_ * height_ * 4, 0);

    // --- Audio decoder (optional; swr is set up lazily on the first frame so it
    //     matches the decoder's real output format) ---
    if (astream_ >= 0) {
        AVStream* as = fmt_->streams[astream_];
        const AVCodec* acodec = avcodec_find_decoder(as->codecpar->codec_id);
        if (acodec) {
            actx_ = avcodec_alloc_context3(acodec);
            avcodec_parameters_to_context(actx_, as->codecpar);
            if (avcodec_open2(actx_, acodec, nullptr) == 0) {
                aTimeBase_ = av_q2d(as->time_base);
                audioChannels_ = as->codecpar->ch_layout.nb_channels;
            } else {
                avcodec_free_context(&actx_); actx_ = nullptr; astream_ = -1;
            }
        } else {
            astream_ = -1;
        }
    }

    duration_ = (fmt_->duration > 0) ? (double)fmt_->duration / AV_TIME_BASE : 0.0;
    // MPEG-TS and MPEG-PS demuxers add every packet they read while searching for a seek position to the
    // index, flagged as a keyframe: those entries say nothing about where keyframes are.
    const char* format = fmt_->iformat && fmt_->iformat->name ? fmt_->iformat->name : "";
    indexUsable_ = std::strncmp(format, "mpegts", 6) != 0 && std::strcmp(format, "mpeg") != 0;

    frame_    = av_frame_alloc();
    dstFrame_ = av_frame_alloc();
    pkt_      = av_packet_alloc();
    resetStreamState();

    // Some demuxers read their keyframe index only when first asked to seek (Matroska and WebM cues):
    // ask now, at the start, so the index is there from the first lap.
    seek(0.0);

    // Count time from the first frame (see the class comment). The duration spans from the start of
    // the container's clock, so shorten it by however much later than that the first frame comes. (An
    // FLV's duration counts from 0 instead: its laps end a B-frame delay late -- the last frame is held
    // a frame or two longer, never lost.)
    DecodedFrame f;
    if (decodeNext(f)) {
        startT_ = f.t;
        f.t = 0.0;
        first_ = std::move(f);
        const double clockStart = fmt_->start_time != AV_NOPTS_VALUE ? (double)fmt_->start_time / AV_TIME_BASE : 0.0;
        if (duration_ > 0.0) duration_ = std::max(0.0, duration_ - (startT_ - clockStart));
    }
    return true;
}

double VideoDecoder::frameDuration() const {
    if (!fmt_ || vstream_ < 0) return 1.0 / 30.0;
    const AVStream* vs = fmt_->streams[vstream_];
    AVRational r = vs->avg_frame_rate;
    if (r.num <= 0 || r.den <= 0) r = vs->r_frame_rate;
    if (r.num <= 0 || r.den <= 0) return 1.0 / 30.0;
    return (double)r.den / (double)r.num;
}

bool VideoDecoder::nextKeyframeAfter(double t, double& key) const {
    if (!fmt_ || vstream_ < 0 || vTimeBase_ <= 0.0) return false;
    if (!indexUsable_) { key = kInf; return true; }
    AVStream* vs = fmt_->streams[vstream_];
    if (avformat_index_get_entries_count(vs) <= 0) return false;
    const int64_t ts = (int64_t)std::floor((t + startT_) / vTimeBase_) + 1;   // strictly after t
    const AVIndexEntry* e = avformat_index_get_entry_from_timestamp(vs, ts, 0);   // keyframe, >= ts
    key = e ? e->timestamp * vTimeBase_ - startT_ : kInf;
    return true;
}

void VideoDecoder::seek(double t) {
    if (!fmt_) return;
    first_.reset();
    // At or before 0, the very start of the file rather than the first frame's time: a demuxer that
    // searches by timestamp (MPEG-TS) can overshoot that by a keyframe interval.
    const double src = t > 0.0 ? t + startT_ : 0.0;
    int64_t ts = (int64_t)(src / (vTimeBase_ > 0 ? vTimeBase_ : 1.0));
    // BACKWARD lands on the keyframe at or before ts -- exactly the GOP start the
    // caller decodes forward from. If that fails (e.g. a stream it cannot seek), restart
    // from the beginning: the caller then decodes forward to its target -- slow, but right.
    if (av_seek_frame(fmt_, vstream_, ts, AVSEEK_FLAG_BACKWARD) < 0)
        av_seek_frame(fmt_, vstream_, 0, AVSEEK_FLAG_BACKWARD);
    if (vctx_) avcodec_flush_buffers(vctx_);
    if (actx_) avcodec_flush_buffers(actx_);
    clearQueue();
    resetStreamState();
}

// Read one packet: a video packet joins the queue, an audio packet is decoded into the pending
// buffer. False at the end of the input, after flushing the audio decoder.
bool VideoDecoder::readPacket() {
    if (demuxEof_) return false;
    if (av_read_frame(fmt_, pkt_) < 0) {
        demuxEof_ = true;
        if (actx_) { avcodec_send_packet(actx_, nullptr); drainAudio(); }
        return false;
    }
    const AVStream* st = fmt_->streams[pkt_->stream_index];
    const int64_t ts = pkt_->pts != AV_NOPTS_VALUE ? pkt_->pts : pkt_->dts;
    if (ts != AV_NOPTS_VALUE) demuxedT_ = std::max(demuxedT_, ts * av_q2d(st->time_base));
    if (pkt_->stream_index == vstream_) {
        if (AVPacket* q = av_packet_alloc()) {
            av_packet_move_ref(q, pkt_);
            queuedBytes_ += (std::size_t)q->size;
            vq_.push_back(q);
            return true;
        }
    } else if (actx_ && pkt_->stream_index == astream_) {
        avcodec_send_packet(actx_, pkt_);
        drainAudio();
    }
    av_packet_unref(pkt_);
    return true;
}

// Receive every audio frame the decoder has buffered, resample each to 48 kHz mono float, and
// append it to the pending buffer, noting the source time of the buffer's first sample.
void VideoDecoder::drainAudio() {
    if (!actx_) return;
    while (avcodec_receive_frame(actx_, frame_) == 0) {
        if (!swr_) {
            AVChannelLayout outLayout; av_channel_layout_default(&outLayout, 1);  // mono
            AVChannelLayout inLayout;
            if (frame_->ch_layout.nb_channels > 0) av_channel_layout_copy(&inLayout, &frame_->ch_layout);
            else                                   av_channel_layout_default(&inLayout, 1);
            int rc = swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_FLT, kOutRate,
                                         &inLayout, (AVSampleFormat)frame_->format,
                                         frame_->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&outLayout);
            av_channel_layout_uninit(&inLayout);
            if (rc < 0 || !swr_ || swr_init(swr_) < 0) {
                if (swr_) swr_free(&swr_);
                av_frame_unref(frame_);
                continue;   // can't resample this frame; skip it
            }
        }
        const double start = frame_->pts != AV_NOPTS_VALUE ? frame_->pts * aTimeBase_
                           : (std::isfinite(audioEndT_) ? audioEndT_ : 0.0);
        int outCount = swr_get_out_samples(swr_, frame_->nb_samples);
        if (outCount > 0) {
            if ((int)aScratch_.size() < outCount) aScratch_.resize(outCount);
            uint8_t* outptr = (uint8_t*)aScratch_.data();
            int got = swr_convert(swr_, &outptr, outCount,
                                  (const uint8_t**)frame_->extended_data, frame_->nb_samples);
            if (got > 0) {
                if (pendingAudio_.empty()) pendingAudioStart_ = start;
                pendingAudio_.insert(pendingAudio_.end(), aScratch_.data(), aScratch_.data() + got);
                audioEndT_ = start + (double)got / kOutRate;
            }
        }
        av_frame_unref(frame_);
    }
}

void VideoDecoder::pumpAudio(double t) {
    if (!fmt_ || !actx_) return;
    while (audioSettledUpTo() < t && queuedBytes_ < kMaxQueuedBytes && readPacket()) {}
}

double VideoDecoder::audioSettledUpTo() const {
    if (!actx_ || demuxEof_) return kInf;
    return std::max(audioEndT_, demuxedT_ - kAudioSettleSlack) - startT_;
}

bool VideoDecoder::takeAudio(std::vector<float>& out, double& startT) {
    out.clear();
    if (pendingAudio_.empty()) return false;
    out.swap(pendingAudio_);
    startT = pendingAudioStart_ - startT_;
    if (startT < 0.0) {                            // from before the first frame: the clip starts later
        const std::size_t skip = std::min(out.size(), (std::size_t)std::llround(-startT * kOutRate));
        out.erase(out.begin(), out.begin() + (std::ptrdiff_t)skip);
        startT = std::max(0.0, startT + (double)skip / kOutRate);
    }
    return !out.empty();
}

bool VideoDecoder::decodeNext(DecodedFrame& out) {
    out.reset();
    if (first_.valid()) { out = std::move(first_); return true; }
    if (!fmt_ || !vctx_) return false;

    while (true) {
        const int r = avcodec_receive_frame(vctx_, frame_);
        if (r == 0) {
            const int64_t ts = (frame_->best_effort_timestamp != AV_NOPTS_VALUE)
                             ? frame_->best_effort_timestamp : frame_->pts;
            out.t = ((ts != AV_NOPTS_VALUE) ? ts * vTimeBase_ : 0.0) - startT_;
            out.frame_ = av_frame_alloc();
            if (!out.frame_) { av_frame_unref(frame_); return false; }
            av_frame_move_ref(out.frame_, frame_);
            return true;
        }
        if (r == AVERROR_EOF || vflushed_) return false;   // fully drained / nothing more coming

        // r == AVERROR(EAGAIN): feed the decoder its next video packet, reading more as needed.
        while (vq_.empty() && readPacket()) {}
        if (vq_.empty()) {                                  // end of input: flush the decoder
            avcodec_send_packet(vctx_, nullptr);
            vflushed_ = true;
            continue;                                       // receive the frames it still holds
        }
        AVPacket* p = vq_.front();
        vq_.pop_front();
        queuedBytes_ -= (std::size_t)p->size;
        avcodec_send_packet(vctx_, p);
        av_packet_free(&p);
    }
}

bool VideoDecoder::convert(const DecodedFrame& f, std::uint8_t* dst, int dstStride) {
    const AVFrame* src = f.frame_;
    if (!src || !dst || src->width != width_ || src->height != height_) return false;
    if (!swsThr_ || swsThrFmt_ != src->format) {
        if (swsThr_) sws_freeContext(swsThr_);
        swsThr_ = sws_alloc_context();
        if (!swsThr_) return false;
        av_opt_set_int(swsThr_, "srcw", width_, 0);
        av_opt_set_int(swsThr_, "srch", height_, 0);
        av_opt_set_int(swsThr_, "src_format", src->format, 0);
        av_opt_set_int(swsThr_, "dstw", width_, 0);
        av_opt_set_int(swsThr_, "dsth", height_, 0);
        av_opt_set_int(swsThr_, "dst_format", AV_PIX_FMT_RGBA, 0);
        av_opt_set_int(swsThr_, "sws_flags", SWS_BILINEAR, 0);
        av_opt_set_int(swsThr_, "threads", 0, 0);   // automatic: a slice thread per core
        if (sws_init_context(swsThr_, nullptr, nullptr) < 0) {
            sws_freeContext(swsThr_); swsThr_ = nullptr; return false;
        }
        swsThrFmt_ = src->format;
    }
    av_frame_unref(dstFrame_);
    dstFrame_->format = AV_PIX_FMT_RGBA;
    dstFrame_->width  = width_;
    dstFrame_->height = height_;
    dstFrame_->buf[0] = av_buffer_create(dst, (std::size_t)dstStride * height_, keepBuffer, nullptr, 0);
    if (!dstFrame_->buf[0]) return false;
    dstFrame_->data[0]     = dst;
    dstFrame_->linesize[0] = dstStride;
    const int r = sws_scale_frame(swsThr_, dstFrame_, src);
    av_frame_unref(dstFrame_);   // drops our reference; keepBuffer leaves the memory alone
    return r >= 0;
}

bool VideoDecoder::decodeFrame(VideoFrame& out, std::vector<float>& audio,
                               double& audioStartT, bool& audioStartValid) {
    DecodedFrame f;
    const bool ok = decodeNext(f);
    double start = 0.0;
    if (takeAudio(legacyAudio_, start)) {
        if (!audioStartValid) { audioStartT = start; audioStartValid = true; }
        audio.insert(audio.end(), legacyAudio_.begin(), legacyAudio_.end());
    }
    if (!ok) return false;

    // Flip vertically (negative stride from the last row) so the buffer is bottom-up, matching how
    // the rest of the app's textures are oriented.
    uint8_t* dst[4] = { rgba_.data() + (std::size_t)(height_ - 1) * width_ * 4,
                        nullptr, nullptr, nullptr };
    int dstStride[4] = { -width_ * 4, 0, 0, 0 };
    sws_scale(sws_, f.frame_->data, f.frame_->linesize, 0, height_, dst, dstStride);
    out.t      = f.t;
    out.width  = width_;
    out.height = height_;
    out.rgba   = rgba_.data();
    return true;
}

} // namespace oss
