#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include "core/Node.h"
#include "core/Value.h"
#include "core/VertexShaders.h"

namespace oss {

// Emits a preset vertex shader on its Shader output. Wire into a Deform node's `shader`
// input. GL-free -- it just emits a GLSL source string.
class VertexShaderNode : public Node {
public:
    VertexShaderNode() : Node("Vertex Shader") {
        addChoiceInput("preset", vertexShaderLabels(), 0);
        addOutput("shader", PortType::Shader);
    }
    void evaluate(EvalContext& ctx) override {
        int n = (int)vertexShaderLabels().size();
        preset_ = std::clamp((int)std::lround(ctx.in<float>(0)), 0, n - 1);
        ctx.out<ShaderRef>(0, ShaderRef{ vertexShaderSource(preset_) });
    }
    std::string statusLine() const override { return vertexShaderLabels()[(std::size_t)preset_]; }

private:
    int preset_ = 0;
};

} // namespace oss
