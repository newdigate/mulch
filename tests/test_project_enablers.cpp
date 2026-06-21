#include <doctest/doctest.h>
#include "core/AutoCurve.h"
#include "core/Graph.h"
#include "modules/AutomationNode.h"
#include <memory>

using namespace oss;

TEST_CASE("encodeCurve / decodeCurve round-trip") {
    AutoCurve c;
    c.points = {{0.0f, 0.2f}, {2.0f, 0.8f}, {4.0f, 0.5f}};
    AutoCurve r = decodeCurve(encodeCurve(c));
    REQUIRE(r.points.size() == 3);
    CHECK(r.points[1].bar == doctest::Approx(2.0f));
    CHECK(r.points[1].value == doctest::Approx(0.8f));
    CHECK(decodeCurve(encodeCurve(AutoCurve{})).points.empty());   // empty round-trips

    AutoCurve h;
    AutoPoint hp{1.0f, 0.4f}; hp.outDBar = 0.25f; hp.outDValue = -0.1f;
    h.points = { hp, {3.0f, 0.6f} };
    AutoCurve hr = decodeCurve(encodeCurve(h));
    REQUIRE(hr.points.size() == 2);
    CHECK(hr.points[0].outDBar   == doctest::Approx(0.25f));
    CHECK(hr.points[0].outDValue == doctest::Approx(-0.1f));
}

TEST_CASE("AutomationNode saveState / loadState round-trips curves + ranges") {
    AutomationNode a;
    a.channel(0).push_back({0.0f, 0.2f});
    a.channel(0).push_back({3.0f, 0.9f});
    a.setOutRange(0, -1.0f, 2.0f);
    a.channel(2).push_back({1.0f, 0.5f});
    std::string s = a.saveState();

    AutomationNode b;
    b.loadState(s);
    REQUIRE(b.channel(0).size() == 2);
    CHECK(b.channel(0)[1].bar == doctest::Approx(3.0f));
    CHECK(b.channel(0)[1].value == doctest::Approx(0.9f));
    CHECK(b.outMin(0) == doctest::Approx(-1.0f));
    CHECK(b.outMax(0) == doctest::Approx(2.0f));
    REQUIRE(b.channel(2).size() == 1);
    CHECK(b.channel(1).empty());
}

namespace { struct ClearStub : Node { ClearStub() : Node("ClearStub") { addOutput("o", PortType::Float); } void evaluate(EvalContext&) override {} }; }

TEST_CASE("Graph::clear empties the graph but keeps ids monotonic") {
    Graph g;
    int a = g.addNode(std::make_unique<ClearStub>());
    g.addNode(std::make_unique<ClearStub>());
    g.automation().setLengthBars(16.0f);
    g.clear();
    CHECK(g.nodes().empty());
    CHECK(g.connections().empty());
    CHECK(g.automation().channels().empty());
    int b = g.addNode(std::make_unique<ClearStub>());
    CHECK(b > a);                                   // ids never reused
}
