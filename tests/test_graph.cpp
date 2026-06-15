#include <doctest/doctest.h>
#include "core/Node.h"

using namespace oss;

namespace {
// A GL-free test node: outputs a constant float.
struct ConstFloat : Node {
    float v;
    explicit ConstFloat(float val) : Node("const"), v(val) { addOutput("out", PortType::Float); }
    void evaluate(EvalContext& ctx) override { ctx.out<float>(0, v); }
};
// Sums two float inputs.
struct AddFloats : Node {
    AddFloats() : Node("add") {
        addInput("a", PortType::Float, 0.0f);
        addInput("b", PortType::Float, 0.0f);
        addOutput("sum", PortType::Float);
    }
    void evaluate(EvalContext& ctx) override { ctx.out<float>(0, ctx.in<float>(0) + ctx.in<float>(1)); }
};
} // namespace

TEST_CASE("Node exposes declared ports") {
    AddFloats n;
    REQUIRE(n.inputs().size() == 2);
    REQUIRE(n.outputs().size() == 1);
    CHECK(n.inputs()[0].name == "a");
    CHECK(n.inputs()[0].direction == Direction::Input);
    CHECK(n.inputs()[0].type == PortType::Float);
    CHECK(n.outputs()[0].name == "sum");
}

TEST_CASE("EvalContext resolves inputs and writes outputs") {
    AddFloats n;
    std::vector<Value> inputs{ Value{2.0f}, Value{3.0f} };
    std::vector<Value> outputs(1);
    EvalContext ctx{ inputs, outputs, 0.016f };
    n.evaluate(ctx);
    CHECK(std::get<float>(outputs[0]) == doctest::Approx(5.0f));
}
