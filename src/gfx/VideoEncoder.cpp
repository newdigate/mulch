#include "gfx/VideoEncoder.h"
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
}

namespace oss {

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
    width_ = width; height_ = height;
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
    if (avcodec_send_frame(ctx, frame) < 0) return false;
    for (;;) {
        int r = avcodec_receive_packet(ctx, pkt_);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) return false;
        av_packet_rescale_ts(pkt_, ctx->time_base, st->time_base);
        pkt_->stream_index = st->index;
        av_interleaved_write_frame(oc_, pkt_);
        av_packet_unref(pkt_);
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
        encodeWrite(actx_, ast_, aframe_);
        afifo_.erase(afifo_.begin(), afifo_.begin() + chunk);
    }
    return true;
}

bool VideoEncoder::close(std::string& err) {
    (void)err;
    if (!opened_) return true;
    opened_ = false;
    encodeWrite(vctx_, vst_, nullptr);            // flush video
    if (actx_) encodeWrite(actx_, ast_, nullptr); // flush audio
    av_write_trailer(oc_);
    freeAll();
    return true;
}

} // namespace oss
