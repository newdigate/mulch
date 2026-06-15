#include <doctest/doctest.h>
#include <algorithm>
#include <memory>
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

#include "core/Graph.h"

namespace {
struct CaptureFloat : Node {
    float captured = -999.0f;
    CaptureFloat() : Node("capture") { addInput("in", PortType::Float, 0.0f); }
    void evaluate(EvalContext& ctx) override { captured = ctx.in<float>(0); }
};
struct ColourSink : Node {           // input-only Colour node, for type-mismatch tests
    ColourSink() : Node("csink") { addInput("c", PortType::Colour, glm::vec4(0)); }
    void evaluate(EvalContext&) override {}
};
} // namespace

TEST_CASE("connect accepts matching types and rejects mismatches") {
    Graph g;
    int c = g.addNode(std::make_unique<ConstFloat>(1.0f));
    int a = g.addNode(std::make_unique<AddFloats>());
    int s = g.addNode(std::make_unique<ColourSink>());

    CHECK(g.connect(c, 0, a, 0) == true);          // Float -> Float OK
    CHECK(g.connect(c, 0, s, 0) == false);         // Float -> Colour rejected
    CHECK(g.connect(c, 0, a, 0) == false);         // input already connected
    CHECK(g.connect(c, 0, a, 5) == false);         // bad port index
}

TEST_CASE("connect rejects edges that would create a cycle") {
    Graph g;
    int a = g.addNode(std::make_unique<AddFloats>());
    int b = g.addNode(std::make_unique<AddFloats>());
    CHECK(g.connect(a, 0, b, 0) == true);
    CHECK(g.connect(b, 0, a, 0) == false);         // would form a cycle
}

TEST_CASE("topologicalOrder places sources before consumers") {
    Graph g;
    int a = g.addNode(std::make_unique<ConstFloat>(1.0f));
    int b = g.addNode(std::make_unique<AddFloats>());
    int c = g.addNode(std::make_unique<AddFloats>());
    g.connect(a, 0, b, 0);
    g.connect(b, 0, c, 0);
    auto order = g.topologicalOrder();
    REQUIRE(order.size() == 3);
    auto idx = [&](int id){ return std::find(order.begin(), order.end(), id) - order.begin(); };
    CHECK(idx(a) < idx(b));
    CHECK(idx(b) < idx(c));
}

TEST_CASE("evaluate propagates values along edges in topological order") {
    Graph g;
    int a   = g.addNode(std::make_unique<ConstFloat>(2.0f));
    int b   = g.addNode(std::make_unique<ConstFloat>(3.0f));
    int sum = g.addNode(std::make_unique<AddFloats>());
    int cap = g.addNode(std::make_unique<CaptureFloat>());
    g.connect(a, 0, sum, 0);
    g.connect(b, 0, sum, 1);
    g.connect(sum, 0, cap, 0);
    g.evaluate(0.016f);
    auto* capture = dynamic_cast<CaptureFloat*>(g.findNode(cap));
    REQUIRE(capture != nullptr);
    CHECK(capture->captured == doctest::Approx(5.0f));
}

TEST_CASE("evaluate uses the port default for an unconnected input") {
    Graph g;
    int a   = g.addNode(std::make_unique<ConstFloat>(7.0f));
    int sum = g.addNode(std::make_unique<AddFloats>());   // input b left unconnected -> default 0
    int cap = g.addNode(std::make_unique<CaptureFloat>());
    g.connect(a, 0, sum, 0);
    g.connect(sum, 0, cap, 0);
    g.evaluate(0.016f);
    auto* capture = dynamic_cast<CaptureFloat*>(g.findNode(cap));
    REQUIRE(capture != nullptr);
    CHECK(capture->captured == doctest::Approx(7.0f));    // 7 + default 0
}
