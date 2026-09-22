#include "gfx/VideoEncoder.h"
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
}

namespace oss {

// FFmpeg's own message for a negative return code, so a failure names its cause (ENOSPC, EIO...)
// instead of just "failed". av_strerror falls back to the raw number for codes it does not know.
static std::string avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(code, buf, sizeof(buf)) < 0) return std::to_string(code);
    return buf;
}

VideoEncoder::~VideoEncoder() {
    if (opened_) { std::string e; close(e); }
    freeAll();
}

void VideoEncoder::freeAll() {
    if (sws_)    { sws_freeContext(sws_); sws_ = nullptr; }
    if (vframe_) av_frame_free(&vframe_);
    if (aframe_) av_frame_free(&aframe_);
    if (pkt_)    av_packet_free(&pkt_);
    if (vctx_)   avcodec_free_context(&vctx_);
    if (actx_)   avcodec_free_context(&actx_);
    if (oc_) {
        if (oc_->pb && !(oc_->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc_->pb);
        avformat_free_context(oc_);
        oc_ = nullptr;
    }
    vst_ = ast_ = nullptr;
}

bool VideoEncoder::open(const std::string& path, int width, int height, int fps,
                        int audioRate, int audioChannels, std::string& err) {
    // Both consumers (the Recorder and the offline --render CLI) otherwise get ~20 lines of
    // libx264/aac per-frame statistics dumped to stderr on every open -- harmless in the app's
    // own log but noise on the scripted path this CLI exists for. Process-wide and idempotent.
    av_log_set_level(AV_LOG_ERROR);
    width_ = width; height_ = height;
    writeErr_.clear(); writeFailed_ = false;   // no stale failure from an earlier attempt
    if (fps <= 0) fps = 60;
    if (audioChannels < 1) audioChannels = 1;
    if (audioChannels > 2) audioChannels = 2;

    avformat_alloc_output_context2(&oc_, nullptr, nullptr, path.c_str());
    if (!oc_) { err = "could not allocate output for " + path; return false; }

    // --- Video stream (H.264, falling back to MPEG-4) ---
    const AVCodec* vc = avcodec_find_encoder_by_name("libx264");
    if (!vc) vc = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!vc) vc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!vc) { err = "no video encoder available"; freeAll(); return false; }

    vst_  = avformat_new_stream(oc_, nullptr);
    vctx_ = avcodec_alloc_context3(vc);
    vctx_->width     = width;
    vctx_->height    = height;
    vctx_->pix_fmt   = AV_PIX_FMT_YUV420P;
    vctx_->time_base = AVRational{1, fps};
    vctx_->framerate = AVRational{fps, 1};
    vctx_->gop_size  = fps;
    vctx_->max_b_frames = 1;
    if (oc_->oformat->flags & AVFMT_GLOBALHEADER) vctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (vc->id == AV_CODEC_ID_H264) {
        av_opt_set(vctx_->priv_data, "preset", "veryfast", 0);   // real-time-ish
        av_opt_set(vctx_->priv_data, "crf",    "23",       0);
    }
    if (avcodec_open2(vctx_, vc, nullptr) < 0) { err = "could not open video encoder"; freeAll(); return false; }
    avcodec_parameters_from_context(vst_->codecpar, vctx_);
    vst_->time_base = vctx_->time_base;

    sws_ = sws_getContext(width, height, AV_PIX_FMT_RGBA,
                          width, height, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                          nullptr, nullptr, nullptr);
    if (!sws_) { err = "could not init colour converter"; freeAll(); return false; }

    vframe_ = av_frame_alloc();
    vframe_->format = AV_PIX_FMT_YUV420P;
    vframe_->width  = width;
    vframe_->height = height;
    if (av_frame_get_buffer(vframe_, 0) < 0) { err = "could not allocate video frame"; freeAll(); return false; }

    // --- Audio stream (AAC, optional) ---
    if (audioRate > 0) {
        const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (ac) {
            ast_  = avformat_new_stream(oc_, nullptr);
            actx_ = avcodec_alloc_context3(ac);
            actx_->sample_fmt  = AV_SAMPLE_FMT_FLTP;
            actx_->sample_rate = audioRate;
            actx_->bit_rate    = 128000;
            av_channel_layout_default(&actx_->ch_layout, audioChannels);   // mono or stereo
            actx_->time_base   = AVRational{1, audioRate};
            if (oc_->oformat->flags & AVFMT_GLOBALHEADER) actx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(actx_, ac, nullptr) == 0) {
                avcodec_parameters_from_context(ast_->codecpar, actx_);
                ast_->time_base = actx_->time_base;
                audioRate_      = audioRate;
                audioChannels_  = audioChannels;
                audioFrameSize_ = actx_->frame_size > 0 ? actx_->frame_size : 1024;
                aframe_ = av_frame_alloc();
                aframe_->format      = AV_SAMPLE_FMT_FLTP;
                aframe_->sample_rate = audioRate;
                aframe_->nb_samples  = audioFrameSize_;
                av_channel_layout_default(&aframe_->ch_layout, audioChannels);
                if (av_frame_get_buffer(aframe_, 0) < 0) { av_frame_free(&aframe_); avcodec_free_context(&actx_); ast_ = nullptr; }
            } else {
                avcodec_free_context(&actx_); ast_ = nullptr;
            }
        }
    }

    if (!(oc_->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&oc_->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
            err = "could not open file for writing: " + path; freeAll(); return false;
        }
    }
    if (avformat_write_header(oc_, nullptr) < 0) { err = "could not write header"; freeAll(); return false; }

    pkt_ = av_packet_alloc();
    lastVpts_ = -1;
    aCount_ = 0;
    afifo_.clear();
    opened_ = true;
    return true;
}

bool VideoEncoder::encodeWrite(AVCodecContext* ctx, AVStream* st, AVFrame* frame) {
    // Every failure below latches writeFailed_ as well as writeErr_: callers that drop the
    // per-call bool (RecorderNode::evaluate does, per frame) would otherwise finalise a file
    // that quietly lost frames and report it as saved. close() consults the latch.
    int s = avcodec_send_frame(ctx, frame);
    if (s < 0) { writeErr_ = avErr(s); writeFailed_ = true; return false; }
    for (;;) {
        int r = avcodec_receive_packet(ctx, pkt_);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) { writeErr_ = avErr(r); writeFailed_ = true; return false; }
        av_packet_rescale_ts(pkt_, ctx->time_base, st->time_base);
        pkt_->stream_index = st->index;
        // The muxer takes the packet's reference on success and blanks it, so the unref below is
        // a no-op there and the cleanup path on failure. The return MUST be propagated: this is
        // where a full disk (ENOSPC) or an I/O error surfaces, and swallowing it is how a
        // truncated file comes back reported as a clean one.
        int w = av_interleaved_write_frame(oc_, pkt_);
        av_packet_unref(pkt_);
        if (w < 0) { writeErr_ = avErr(w); writeFailed_ = true; return false; }
    }
    return true;
}

bool VideoEncoder::addVideoFrame(const std::uint8_t* rgba, double tSeconds) {
    if (!opened_) return false;
    if (av_frame_make_writable(vframe_) < 0) return false;

    // Flip vertically (negative stride from the last row): the texture is bottom-up.
    const uint8_t* src[4] = { rgba + (std::size_t)(height_ - 1) * width_ * 4, nullptr, nullptr, nullptr };
    int srcStride[4] = { -width_ * 4, 0, 0, 0 };
    sws_scale(sws_, src, srcStride, 0, height_, vframe_->data, vframe_->linesize);

    int64_t pts = (int64_t)std::llround(tSeconds * vctx_->time_base.den / vctx_->time_base.num);
    if (pts <= lastVpts_) pts = lastVpts_ + 1;     // strictly increasing
    lastVpts_ = pts;
    vframe_->pts = pts;
    return encodeWrite(vctx_, vst_, vframe_);
}

bool VideoEncoder::addAudio(const float* samples, int count) {
    if (!opened_ || !actx_ || count <= 0) return true;   // no audio stream -> ignore
    afifo_.insert(afifo_.end(), samples, samples + count);
    const int ch    = audioChannels_;
    const int chunk = audioFrameSize_ * ch;              // interleaved floats per frame
    while ((int)afifo_.size() >= chunk) {
        if (av_frame_make_writable(aframe_) < 0) return false;
        // Deinterleave the interleaved input into AAC's planar (FLTP) channels.
        for (int c = 0; c < ch; ++c) {
            float* plane = reinterpret_cast<float*>(aframe_->data[c]);
            for (int i = 0; i < audioFrameSize_; ++i) plane[i] = afifo_[(std::size_t)i * ch + c];
        }
        aframe_->pts = aCount_;
        aCount_ += audioFrameSize_;
        bool ok = encodeWrite(actx_, ast_, aframe_);
        afifo_.erase(afifo_.begin(), afifo_.begin() + chunk);   // drop it either way, so a
        if (!ok) return false;                                  // failure cannot grow the FIFO
    }
    return true;
}

// Every step runs even after an earlier one fails -- the trailer and freeAll() must happen either
// way or the file is left open and the contexts leak -- but the FIRST failure is what `err`
// reports and what makes this return false. A caller that ignores it publishes a file that was
// never finalised (or lost frames to a full disk) as a success.
bool VideoEncoder::close(std::string& err) {
    if (!opened_) return true;
    opened_ = false;
    bool ok = true;
    auto note = [&](const char* what) {
        if (ok) { ok = false; err = std::string(what) + (writeErr_.empty() ? "" : ": " + writeErr_); }
    };
    if (writeFailed_)                                  note("frames were lost during encoding");
    if (!encodeWrite(vctx_, vst_, nullptr))            note("flushing the video encoder failed");
    if (actx_ && !encodeWrite(actx_, ast_, nullptr))   note("flushing the audio encoder failed");
    int t = av_write_trailer(oc_);
    if (t < 0) { writeErr_ = avErr(t); note("writing the file trailer failed"); }
    freeAll();
    return ok;
}

} // namespace oss
