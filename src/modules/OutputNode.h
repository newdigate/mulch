#pragma once
#include "core/Node.h"
#include "core/Value.h"

namespace oss {

// Sink: remembers its input texture so the Viewer can display it.
class OutputNode : public Node {
public:
    OutputNode() : Node("Output") { addInput("in", PortType::Texture, TexRef{}); }
    void evaluate(EvalContext& ctx) override { current_ = ctx.in<TexRef>(0); }
    TexRef current() const { return current_; }
private:
    TexRef current_{};
};

} // namespace oss
