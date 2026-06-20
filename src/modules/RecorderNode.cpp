#include "modules/RecorderNode.h"
#include "core/Value.h"
#include <algorithm>
#include <cstdio>

namespace oss {

RecorderNode::RecorderNode() : Node("Recorder") {
    addInput("video",  PortType::Texture, TexRef{});
    addInput("left",   PortType::Audio,   AudioRef{});
    addInput("right",  PortType::Audio,   AudioRef{});
    addInput("record", PortType::Bool,    false);
    addInput("file",   PortType::String,  std::string("recording.mp4"));
    addOutput("video", PortType::Texture);   // mirrors input 0
    addOutput("left",  PortType::Audio);     // mirrors input 1
    addOutput("right", PortType::Audio);     // mirrors input 2
}

RecorderNode::~RecorderNode() {
    if (recording_) stop();
}

void RecorderNode::evaluate(EvalContext& ctx) {
    TexRef   vin   = ctx.in<TexRef>(0);
    AudioRef lin   = ctx.in<AudioRef>(1);
    AudioRef rin   = ctx.in<AudioRef>(2);
    bool     rec   = ctx.in<bool>(3);
    const std::string& file = ctx.in<std::string>(4);

    // Pass video + audio straight through so the node is transparent in the graph.
    ctx.out<TexRef>(0, vin);
    ctx.out<AudioRef>(1, lin);
    ctx.out<AudioRef>(2, rin);

    // Symmetric mirror so a lone mono wire records on both channels.
    const AudioRef& effL = (lin.samples && lin.count > 0) ? lin : rin;
    const AudioRef& effR = (rin.samples && rin.count > 0) ? rin : lin;
    bool haveAudio = (effL.samples && effL.sampleRate > 0);
    int  sr = haveAudio ? effL.sampleRate : 0;

    if (rec && !recording_)  start(file, vin, sr);
    if (!rec && recording_)  stop();

    if (recording_ && enc_) {
        recordTime_ += ctx.dt;
        if (vin.id && vin.w == encW_ && vin.h == encH_) {
            pixbuf_.resize((std::size_t)encW_ * encH_ * 4);
            glBindTexture(GL_TEXTURE_2D, vin.id);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixbuf_.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            enc_->addVideoFrame(pixbuf_.data(), recordTime_);
            ++frames_;
        }
        std::size_t nL = effL.samples ? effL.count : 0;
        std::size_t nR = effR.samples ? effR.count : 0;
        std::size_t n  = std::max(nL, nR);
        if (n > 0) {
            audioScratch_.resize(n * 2);
            for (std::size_t i = 0; i < n; ++i) {
                audioScratch_[i * 2]     = (i < nL) ? effL.samples[i] : 0.0f;
                audioScratch_[i * 2 + 1] = (i < nR) ? effR.samples[i] : 0.0f;
            }
            enc_->addAudio(audioScratch_.data(), (int)(n * 2));
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "REC %5.1fs  %ld frames", recordTime_, frames_);
        status_ = buf;
    }
}

void RecorderNode::start(const std::string& file, const TexRef& vin, int sampleRate) {
    if (!vin.id || vin.w <= 0 || vin.h <= 0) { status_ = "waiting for video input"; return; }

    enc_ = std::make_unique<VideoEncoder>();
    encW_ = vin.w; encH_ = vin.h;
    int arate = sampleRate;                       // 0 -> no audio track
    int achan = sampleRate > 0 ? 2 : 0;           // recorded interleaved stereo

    std::string err;
    if (enc_->open(file, encW_, encH_, 60, arate, achan, err)) {
        recording_ = true; recordTime_ = 0.0; frames_ = 0; file_ = file;
        status_ = "recording...";
        std::fprintf(stderr, "[Recorder] recording %s (%dx%d, %s)\n", file.c_str(), encW_, encH_,
                     achan == 2 ? "stereo" : "video only");
    } else {
        status_ = "record failed: " + err;
        std::fprintf(stderr, "[Recorder] %s\n", status_.c_str());
        enc_.reset();
    }
}

void RecorderNode::stop() {
    if (enc_) { std::string err; enc_->close(err); enc_.reset(); }
    recording_ = false;
    status_ = file_.empty() ? "idle" : ("saved " + file_);
    std::fprintf(stderr, "[Recorder] saved %s (%ld frames)\n", file_.c_str(), frames_);
}

} // namespace oss
