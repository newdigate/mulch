#include <doctest/doctest.h>
#include "core/AutomationStore.h"
#include "core/Graph.h"
#include "core/Node.h"
#include "core/Value.h"
#include <memory>
#include <variant>

using namespace oss;

namespace {
// A node with one Float input control (default 15, range [10,20]).
struct Knob : Node {
    Knob() : Node("Knob") { addInput("amt", PortType::Float, 15.0f, 10.0f, 20.0f); }
    void evaluate(EvalContext&) override {}
};
// A node whose only input is non-Float.
struct Switch : Node {
    Switch() : Node("Switch") { addInput("on", PortType::Bool, false); }
    void evaluate(EvalContext&) override {}
};
// A node with a Float output, to drive an edge into a Knob.
struct Src : Node {
    Src() : Node("Src") { addOutput("v", PortType::Float); }
    void evaluate(EvalContext& ctx) override { ctx.out<float>(0, 7.0f); }
};
} // namespace

TEST_CASE("add seeds the range and a bar-0 point at the control's current value") {
    Graph g; int id = g.addNode(std::make_unique<Knob>());
    AutomationStore store;
    auto* ch = store.add(g, id, 0);
    REQUIRE(ch);
    CHECK(ch->outMin == doctest::Approx(10.0f));
    CHECK(ch->outMax == doctest::Approx(20.0f));
    REQUIRE(ch->curve.points.size() == 1);
    CHECK(ch->curve.points[0].bar == doctest::Approx(0.0f));
    CHECK(ch->curve.points[0].value == doctest::Approx(0.5f));   // (15-10)/(20-10)
}

TEST_CASE("add is idempotent for the same node+port") {
    Graph g; int id = g.addNode(std::make_unique<Knob>());
    AutomationStore store;
    auto* a = store.add(g, id, 0);
    auto* b = store.add(g, id, 0);
    CHECK(a == b);
    CHECK(store.channels().size() == 1);
}

TEST_CASE("add rejects missing nodes, bad ports, and non-Float ports") {
    Graph g;
    int knob = g.addNode(std::make_unique<Knob>());
    int sw   = g.addNode(std::make_unique<Switch>());
    AutomationStore store;
    CHECK(store.add(g, 9999, 0) == nullptr);   // no such node
    CHECK(store.add(g, knob, 5) == nullptr);   // no such port
    CHECK(store.add(g, sw, 0)   == nullptr);   // Bool, not Float
    CHECK(store.channels().empty());
}

TEST_CASE("apply writes the scaled sampled value into the control") {
    Graph g; int id = g.addNode(std::make_unique<Knob>());
    AutomationStore store;
    auto* ch = store.add(g, id, 0);
    ch->curve.points = { {0.0f, 0.0f}, {4.0f, 1.0f} };
    ch->outMin = 100.0f; ch->outMax = 200.0f;
    g.transport().bpm = 120.0; g.transport().seconds = 4.0; g.transport().pause();  // bar 2
    store.apply(g, g.transport());
    CHECK(std::get<float>(g.findNode(id)->inputDefault(0)) == doctest::Approx(150.0f));
}

TEST_CASE("apply does not overwrite a connected input") {
    Graph g;
    int id  = g.addNode(std::make_unique<Knob>());
    int src = g.addNode(std::make_unique<Src>());
    REQUIRE(g.connect(src, 0, id, 0));
    AutomationStore store;
    auto* ch = store.add(g, id, 0);
    ch->curve.points = { {0.0f, 1.0f} };
    ch->outMin = 100.0f; ch->outMax = 200.0f;
    float before = std::get<float>(g.findNode(id)->inputDefault(0));
    store.apply(g, g.transport());
    CHECK(std::get<float>(g.findNode(id)->inputDefault(0)) == doctest::Approx(before));
}

TEST_CASE("remove and removeNode drop channels") {
    Graph g; int id = g.addNode(std::make_unique<Knob>());
    AutomationStore store;
    store.add(g, id, 0);
    store.remove(id, 0);
    CHECK(store.channels().empty());
    store.add(g, id, 0);
    store.removeNode(id);
    CHECK(store.channels().empty());
}

TEST_CASE("setLengthBars never goes below one bar") {
    AutomationStore store;
    store.setLengthBars(0.0f);
    CHECK(store.lengthBars() == doctest::Approx(1.0f));
    store.setLengthBars(16.0f);
    CHECK(store.lengthBars() == doctest::Approx(16.0f));
}

TEST_CASE("Graph owns the store and applies it during evaluate") {
    Graph g; int id = g.addNode(std::make_unique<Knob>());
    auto* ch = g.automation().add(g, id, 0);
    REQUIRE(ch);
    ch->curve.points = { {0.0f, 0.0f}, {4.0f, 1.0f} };
    ch->outMin = 100.0f; ch->outMax = 200.0f;
    g.transport().bpm = 120.0; g.transport().seconds = 4.0; g.transport().pause();  // bar 2
    g.evaluate(0.0f);
    CHECK(std::get<float>(g.findNode(id)->inputDefault(0)) == doctest::Approx(150.0f));
}

TEST_CASE("removeNode drops the node's UI automation channels") {
    Graph g; int id = g.addNode(std::make_unique<Knob>());
    g.automation().add(g, id, 0);
    REQUIRE(g.automation().channels().size() == 1);
    g.removeNode(id);
    CHECK(g.automation().channels().empty());
}
