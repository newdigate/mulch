#include "audio/AudioFile.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace oss {

AudioClip decodeAudioFile(const std::string& path) {
    AudioClip clip;
    const int OUT_RATE = 48000, OUT_CH = 2;
    clip.sampleRate = OUT_RATE;
    clip.channels   = OUT_CH;

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        clip.error = "could not open file"; return clip;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        clip.error = "could not read stream info"; avformat_close_input(&fmt); return clip;
    }
    int as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (as < 0) { clip.error = "no audio stream"; avformat_close_input(&fmt); return clip; }

    AVStream* st = fmt->streams[as];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) { clip.error = "unsupported audio codec"; avformat_close_input(&fmt); return clip; }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        clip.error = "could not open audio decoder";
        avcodec_free_context(&ctx); avformat_close_input(&fmt); return clip;
    }

    SwrContext* swr = nullptr;                       // set up lazily on the first frame
    AVChannelLayout outLayout; av_channel_layout_default(&outLayout, OUT_CH);
    AVFrame*  frame = av_frame_alloc();
    AVPacket* pkt   = av_packet_alloc();
    std::vector<float> scratch;

    auto drain = [&]() {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (!swr) {
                AVChannelLayout inLayout;
                if (frame->ch_layout.nb_channels > 0) av_channel_layout_copy(&inLayout, &frame->ch_layout);
                else                                   av_channel_layout_default(&inLayout, 2);
                swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, OUT_RATE,
                                    &inLayout, (AVSampleFormat)frame->format, frame->sample_rate, 0, nullptr);
                av_channel_layout_uninit(&inLayout);
                if (!swr || swr_init(swr) < 0) { if (swr) swr_free(&swr); av_frame_unref(frame); continue; }
            }
            int outN = swr_get_out_samples(swr, frame->nb_samples);   // per channel
            if (outN > 0) {
                if ((int)scratch.size() < outN * OUT_CH) scratch.resize((std::size_t)outN * OUT_CH);
                uint8_t* outp = (uint8_t*)scratch.data();
                int got = swr_convert(swr, &outp, outN,
                                      (const uint8_t**)frame->extended_data, frame->nb_samples);
                if (got > 0) clip.samples.insert(clip.samples.end(), scratch.data(),
                                                 scratch.data() + (std::size_t)got * OUT_CH);
            }
            av_frame_unref(frame);
        }
    };

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == as) { avcodec_send_packet(ctx, pkt); drain(); }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, nullptr);               // flush
    drain();

    av_channel_layout_uninit(&outLayout);
    if (swr) swr_free(&swr);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);

    clip.ok = !clip.samples.empty();
    if (!clip.ok && clip.error.empty()) clip.error = "no audio decoded";
    return clip;
}

} // namespace oss
