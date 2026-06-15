#pragma once
#include <glad/gl.h>
#include <vector>
#include "gfx/ShaderNode.h"
#include "audio/SignalGenerator.h"

namespace oss {

class SpectrographNode : public ShaderNode {
public:
    SpectrographNode();
    ~SpectrographNode() override;
    void initGL() override;
    void evaluate(EvalContext& ctx) override;

protected:
    void setUniforms(EvalContext& ctx) override;

private:
    static constexpr int kWindow = 1024;
    static constexpr int kBins   = kWindow / 2;  // 512
    SignalGenerator     gen_;
    std::vector<float>  window_;    // rolling time-domain window
    std::vector<float>  spectrum_;  // normalized magnitudes (kBins)
    GLuint              specTex_ = 0;
};

} // namespace oss
