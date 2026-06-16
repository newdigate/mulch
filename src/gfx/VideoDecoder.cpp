#include "gfx/VideoDecoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace oss {

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close() {
    if (sws_)   { sws_freeContext(sws_); sws_ = nullptr; }
    if (swr_)   { swr_free(&swr_); }
    if (vctx_)  avcodec_free_context(&vctx_);
    if (actx_)  avcodec_free_context(&actx_);
    if (pkt_)   av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (fmt_)   avformat_close_input(&fmt_);
    vstream_ = astream_ = -1;
    width_ = height_ = 0;
    duration_ = vTimeBase_ = aTimeBase_ = 0.0;
    eof_ = false;
}

bool VideoDecoder::open(const std::string& path, std::string& err) {
    close();

    if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) {
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
            } else {
                avcodec_free_context(&actx_); actx_ = nullptr; astream_ = -1;
            }
        } else {
            astream_ = -1;
        }
    }

    duration_ = (fmt_->duration > 0) ? (double)fmt_->duration / AV_TIME_BASE : 0.0;

    frame_ = av_frame_alloc();
    pkt_   = av_packet_alloc();
    eof_   = false;
    return true;
}

void VideoDecoder::seek(double t) {
    if (!fmt_) return;
    if (t < 0.0) t = 0.0;
    int64_t ts = (int64_t)(t / (vTimeBase_ > 0 ? vTimeBase_ : 1.0));
    // BACKWARD lands on the keyframe at or before ts -- exactly the GOP start the
    // caller decodes forward from to rebuild its sliding window.
    av_seek_frame(fmt_, vstream_, ts, AVSEEK_FLAG_BACKWARD);
    if (vctx_) avcodec_flush_buffers(vctx_);
    if (actx_) avcodec_flush_buffers(actx_);
    eof_ = false;
}

// Receive every audio frame the decoder currently has buffered, resample each to
// 48 kHz mono float, and append to `audio`. Sets audioStart{T,Valid} the first
// time audio lands in a fresh buffer so the caller has a source-time anchor.
void VideoDecoder::drainAudio(std::vector<float>& audio, double& audioStartT, bool& audioStartValid) {
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
        if (!audioStartValid) {
            audioStartT = (frame_->pts != AV_NOPTS_VALUE) ? frame_->pts * aTimeBase_ : 0.0;
            audioStartValid = true;
        }
        int outCount = swr_get_out_samples(swr_, frame_->nb_samples);
        if (outCount > 0) {
            if ((int)aScratch_.size() < outCount) aScratch_.resize(outCount);
            uint8_t* outptr = (uint8_t*)aScratch_.data();
            int got = swr_convert(swr_, &outptr, outCount,
                                  (const uint8_t**)frame_->extended_data, frame_->nb_samples);
            if (got > 0) audio.insert(audio.end(), aScratch_.data(), aScratch_.data() + got);
        }
        av_frame_unref(frame_);
    }
}

bool VideoDecoder::decodeFrame(VideoFrame& out, std::vector<float>& audio,
                               double& audioStartT, bool& audioStartValid) {
    if (!fmt_ || !vctx_) return false;

    while (true) {
        int r = avcodec_receive_frame(vctx_, frame_);
        if (r == 0) {
            // Flip vertically (negative stride from the last row) so the buffer is
            // bottom-up, matching how the rest of the app's textures are oriented.
            uint8_t* dst[4] = { rgba_.data() + (std::size_t)(height_ - 1) * width_ * 4,
                                nullptr, nullptr, nullptr };
            int dstStride[4] = { -width_ * 4, 0, 0, 0 };
            sws_scale(sws_, frame_->data, frame_->linesize, 0, height_, dst, dstStride);

            int64_t ts = (frame_->best_effort_timestamp != AV_NOPTS_VALUE)
                       ? frame_->best_effort_timestamp : frame_->pts;
            out.t      = (ts != AV_NOPTS_VALUE) ? ts * vTimeBase_ : 0.0;
            out.width  = width_;
            out.height = height_;
            out.rgba   = rgba_.data();
            av_frame_unref(frame_);
            return true;
        }
        if (r == AVERROR_EOF) return false;   // fully drained
        if (eof_) return false;               // flushed already; nothing more coming

        // r == AVERROR(EAGAIN): feed the decoder more packets.
        int rr = av_read_frame(fmt_, pkt_);
        if (rr < 0) {                          // end of input -> flush decoders
            eof_ = true;
            avcodec_send_packet(vctx_, nullptr);
            if (actx_) { avcodec_send_packet(actx_, nullptr); drainAudio(audio, audioStartT, audioStartValid); }
            continue;                          // loop to receive any buffered video frames
        }
        if (pkt_->stream_index == vstream_) {
            avcodec_send_packet(vctx_, pkt_);
        } else if (actx_ && pkt_->stream_index == astream_) {
            avcodec_send_packet(actx_, pkt_);
            drainAudio(audio, audioStartT, audioStartValid);
        }
        av_packet_unref(pkt_);
    }
}

} // namespace oss
