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
    std::vector<float>  verts_;     // kBins * 3 floats: the spectrum as a vec3 line strip
    GLuint              specTex_ = 0;
    GLuint              vbo_     = 0;   // vertex buffer streamed on the geometry output
};

} // namespace oss
