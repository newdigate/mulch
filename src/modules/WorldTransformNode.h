#pragma once
#include "core/Node.h"
#include "core/Value.h"

namespace oss {

// A shared world transform source: integrates a single rotation `rate` (rad/s,
// signed) into an angle and publishes it as a Transform on output 0. Wire its
// output into the `transform` input of several Wireframe / Shaded Render nodes so
// they all rotate by the same angle and stay aligned (e.g. when blending a
// model's wireframe and shaded views of the same geometry). GL-free.
class WorldTransformNode : public Node {
public:
    WorldTransformNode() : Node("World Transform") {
        addInput("rate", PortType::Float, 0.5f, -2.0f, 2.0f);    // yaw spin rate (rad/s)
        addInput("pitch", PortType::Float, 0.0f, -1.5f, 1.5f);   // pitch tilt angle (radians)
        addOutput("transform", PortType::Transform);
    }

    void evaluate(EvalContext& ctx) override {
        angle_ += ctx.dt * ctx.in<float>(0);
        ctx.out<Transform>(0, Transform{angle_, ctx.in<float>(1), true});
    }

private:
    float angle_ = 0.0f;
};

} // namespace oss
