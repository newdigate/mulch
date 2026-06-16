#include <doctest/doctest.h>
#include "modules/AutomationNode.h"
#include "core/Graph.h"
#include "core/Node.h"
#include <memory>

using namespace oss;

TEST_CASE("an empty channel samples to zero") {
    AutomationNode a;
    CHECK(a.sample(0, 0.0f) == doctest::Approx(0.0f));
    CHECK(a.sample(0, 3.5f) == doctest::Approx(0.0f));
}

TEST_CASE("a piecewise-linear curve interpolates and holds at the ends") {
    AutomationNode a;
    a.channel(0) = { {0.0f, 0.0f}, {2.0f, 1.0f}, {4.0f, 0.0f} };   // a triangle
    CHECK(a.sample(0, 0.0f) == doctest::Approx(0.0f));
    CHECK(a.sample(0, 1.0f) == doctest::Approx(0.5f));   // halfway up
    CHECK(a.sample(0, 2.0f) == doctest::Approx(1.0f));   // peak
    CHECK(a.sample(0, 3.0f) == doctest::Approx(0.5f));   // halfway down
    CHECK(a.sample(0, 4.0f) == doctest::Approx(0.0f));
    CHECK(a.sample(0, 5.0f) == doctest::Approx(0.0f));   // past the end -> hold last
    CHECK(a.sample(0, -1.0f) == doctest::Approx(0.0f));  // before the start -> hold first
}

TEST_CASE("channels default to the stream-parameter category") {
    AutomationNode a;
    CHECK(a.category(0) == AutoCategory::StreamParam);
    a.setCategory(0, AutoCategory::UiControl);
    CHECK(a.category(0) == AutoCategory::UiControl);
    CHECK(a.category(1) == AutoCategory::StreamParam);   // others unaffected
}

TEST_CASE("setLengthBars never goes below one bar") {
    AutomationNode a;
    a.setLengthBars(0.0f);
    CHECK(a.lengthBars() == doctest::Approx(1.0f));
    a.setLengthBars(16.0f);
    CHECK(a.lengthBars() == doctest::Approx(16.0f));
}

TEST_CASE("Graph::evaluate samples the curve at the transport position and scales it") {
    // A node that records the float it was handed.
    struct Probe : Node {
        Probe() : Node("Probe") { addInput("in", PortType::Float, 0.0f); }
        void evaluate(EvalContext& ctx) override { v = ctx.in<float>(0); }
        float v = -1.0f;
    };

    Graph g;
    auto a = std::make_unique<AutomationNode>();
    AutomationNode* ap = a.get();
    ap->setLengthBars(4.0f);
    ap->channel(0) = { {0.0f, 0.0f}, {4.0f, 1.0f} };   // ramps 0 -> 1 over 4 bars
    ap->setOutRange(0, 100.0f, 200.0f);                // and scales to [100, 200]

    auto pr = std::make_unique<Probe>();
    Probe* prp = pr.get();
    int aId = g.addNode(std::move(a));
    int pId = g.addNode(std::move(pr));
    REQUIRE(g.connect(aId, 0, pId, 0));

    // 120 BPM -> 2 s/bar. Park the transport at bar 2 (paused so evaluate's
    // advance(dt) is a no-op) and check the probe sees the scaled mid value.
    g.transport().bpm = 120.0;
    g.transport().seconds = 4.0;     // beats = 8, bars = 2 (of 4)
    g.transport().pause();
    g.evaluate(0.5f);

    CHECK(g.transport().bars() == doctest::Approx(2.0));
    CHECK(prp->v == doctest::Approx(150.0f));   // sample 0.5 scaled into [100,200]
}
