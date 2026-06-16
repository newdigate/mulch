#include "modules/RecorderNode.h"
#include "core/Value.h"
#include <cstdio>

namespace oss {

RecorderNode::RecorderNode() : Node("Recorder") {
    addInput("video",  PortType::Texture, TexRef{});
    addInput("audio",  PortType::Audio,   AudioRef{});
    addInput("record", PortType::Bool,    false);
    addInput("file",   PortType::String,  std::string("recording.mp4"));
    addOutput("video", PortType::Texture);   // mirrors input 0
    addOutput("audio", PortType::Audio);     // mirrors input 1
}

RecorderNode::~RecorderNode() {
    if (recording_) stop();
}

void RecorderNode::evaluate(EvalContext& ctx) {
    TexRef   vin  = ctx.in<TexRef>(0);
    AudioRef ain  = ctx.in<AudioRef>(1);
    bool     rec  = ctx.in<bool>(2);
    const std::string& file = ctx.in<std::string>(3);

    // Pass video + audio straight through so the node is transparent in the graph.
    ctx.out<TexRef>(0, vin);
    ctx.out<AudioRef>(1, ain);

    if (rec && !recording_)  start(file, vin, ain);
    if (!rec && recording_)  stop();

    if (recording_ && enc_) {
        recordTime_ += ctx.dt;
        // Capture the frame: read the input texture's pixels (bottom-up) and encode.
        if (vin.id && vin.w == encW_ && vin.h == encH_) {
            pixbuf_.resize((std::size_t)encW_ * encH_ * 4);
            glBindTexture(GL_TEXTURE_2D, vin.id);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixbuf_.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            enc_->addVideoFrame(pixbuf_.data(), recordTime_);
            ++frames_;
        }
        if (ain.samples && ain.count > 0) enc_->addAudio(ain.samples, (int)ain.count);

        char buf[96];
        std::snprintf(buf, sizeof(buf), "REC %5.1fs  %ld frames", recordTime_, frames_);
        status_ = buf;
    }
}

void RecorderNode::start(const std::string& file, const TexRef& vin, const AudioRef& ain) {
    if (!vin.id || vin.w <= 0 || vin.h <= 0) { status_ = "waiting for video input"; return; }

    enc_ = std::make_unique<VideoEncoder>();
    encW_ = vin.w; encH_ = vin.h;
    int arate = (ain.samples && ain.sampleRate > 0) ? ain.sampleRate : 0;   // 0 -> no audio track

    std::string err;
    if (enc_->open(file, encW_, encH_, 60, arate, err)) {
        recording_ = true; recordTime_ = 0.0; frames_ = 0; file_ = file;
        status_ = "recording...";
        std::fprintf(stderr, "[Recorder] recording %s (%dx%d, %s)\n",
                     file.c_str(), encW_, encH_, arate > 0 ? "with audio" : "video only");
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
