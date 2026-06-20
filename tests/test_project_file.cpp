#include <doctest/doctest.h>
#include "core/ProjectFile.h"
#include <string>
#include "core/Graph.h"
#include "core/Node.h"
#include <memory>

using namespace oss;

TEST_CASE("serializeProject / parseProject round-trip a ProjectDoc") {
    ProjectDoc d;
    d.bpm = 140.0; d.beatsPerBar = 3; d.looping = true;
    d.loopStartBar = 1.0; d.loopEndBar = 5.0; d.lengthBars = 12.0f;
    DocNode n1; n1.id = 1; n1.x = 40; n1.y = 60; n1.type = "Audio Out";
    n1.inputs.push_back({0, Value(220.0f)});
    n1.inputs.push_back({1, Value(true)});
    n1.inputs.push_back({2, Value(glm::vec4(0.1f, 0.2f, 0.3f, 1.0f))});
    n1.inputs.push_back({3, Value(std::string("a path with spaces.wav"))});
    n1.state = "0,0.2:1:|x";
    d.nodes.push_back(n1);
    d.connections.push_back({1, 0, 2, 1});
    DocAuto a; a.nodeId = 1; a.port = 0; a.outMin = -1.0f; a.outMax = 2.0f;
    a.curve.points = {{0.0f, 0.5f}, {4.0f, 0.9f}};
    d.autos.push_back(a);

    std::string text = serializeProject(d);
    ProjectDoc r;
    REQUIRE(parseProject(text, r));
    CHECK(r.bpm == doctest::Approx(140.0));
    CHECK(r.beatsPerBar == 3);
    CHECK(r.looping == true);
    CHECK(r.lengthBars == doctest::Approx(12.0f));
    REQUIRE(r.nodes.size() == 1);
    CHECK(r.nodes[0].type == "Audio Out");
    CHECK(r.nodes[0].state == "0,0.2:1:|x");
    REQUIRE(r.nodes[0].inputs.size() == 4);
    CHECK(std::get<float>(r.nodes[0].inputs[0].value) == doctest::Approx(220.0f));
    CHECK(std::get<bool>(r.nodes[0].inputs[1].value) == true);
    CHECK(std::get<glm::vec4>(r.nodes[0].inputs[2].value).y == doctest::Approx(0.2f));
    CHECK(std::get<std::string>(r.nodes[0].inputs[3].value) == "a path with spaces.wav");
    REQUIRE(r.connections.size() == 1);
    CHECK(r.connections[0].dstPort == 1);
    REQUIRE(r.autos.size() == 1);
    CHECK(r.autos[0].outMax == doctest::Approx(2.0f));
    REQUIRE(r.autos[0].curve.points.size() == 2);
    CHECK(r.autos[0].curve.points[1].value == doctest::Approx(0.9f));
}

TEST_CASE("parseProject rejects a bad header") {
    ProjectDoc d;
    CHECK_FALSE(parseProject("not-an-oss-file\nnode 1 0 0\n", d));
    CHECK_FALSE(parseProject("", d));
}

TEST_CASE("string values with embedded newline survive via escaping") {
    ProjectDoc d;
    DocNode n; n.id = 1; n.x = 0; n.y = 0; n.type = "Text";
    n.inputs.push_back({0, Value(std::string("line1\nline2"))});
    d.nodes.push_back(n);
    ProjectDoc r;
    REQUIRE(parseProject(serializeProject(d), r));
    CHECK(std::get<std::string>(r.nodes[0].inputs[0].value) == "line1\nline2");
}

namespace {
struct StubA : Node {
    StubA() : Node("StubA") {
        addInput("f", PortType::Float, 1.0f, 0.0f, 10.0f);
        addInput("b", PortType::Bool, false);
        addInput("c", PortType::Colour, glm::vec4(0.1f, 0.2f, 0.3f, 1.0f));
        addInput("s", PortType::String, std::string("hi"));
        addOutput("out", PortType::Float);
    }
    void evaluate(EvalContext&) override {}
};
struct StubB : Node {
    std::string st = "default";
    StubB() : Node("StubB") { addInput("x", PortType::Float, 0.0f); addOutput("o", PortType::Float); }
    void evaluate(EvalContext&) override {}
    std::string saveState() const override { return st; }
    void loadState(const std::string& s) override { st = s; }
};
std::unique_ptr<Node> stubFactory(const std::string& t) {
    if (t == "StubA") return std::make_unique<StubA>();
    if (t == "StubB") return std::make_unique<StubB>();
    return nullptr;
}
Node* byName(Graph& g, const std::string& n) {
    for (auto& np : g.nodes()) if (np->name() == n) return np.get();
    return nullptr;
}
}

TEST_CASE("captureProject -> serialize -> parse -> restoreProject round-trips a graph") {
    Graph g;
    int a = g.addNode(std::make_unique<StubA>());
    int b = g.addNode(std::make_unique<StubB>());
    g.findNode(a)->pos = glm::vec2(120.0f, 240.0f);
    g.findNode(a)->inputDefault(0) = Value(7.5f);
    g.findNode(a)->inputDefault(3) = Value(std::string("file path.wav"));
    static_cast<StubB*>(g.findNode(b))->st = "kept-state";
    g.connect(a, 0, b, 0);
    g.transport().bpm = 99.0;
    UiAutomationChannel* ch = g.automation().add(g, a, 0);
    REQUIRE(ch != nullptr);
    ch->curve.points = {{0.0f, 0.25f}, {2.0f, 0.75f}};
    ch->outMin = -2.0f; ch->outMax = 3.0f;

    std::string text = saveProject(g);

    Graph g2;
    REQUIRE(loadProject(text, g2, stubFactory, [](Node&){}));
    REQUIRE(g2.nodes().size() == 2);
    Node* a2 = byName(g2, "StubA");
    Node* b2 = byName(g2, "StubB");
    REQUIRE(a2); REQUIRE(b2);
    CHECK(a2->pos.x == doctest::Approx(120.0f));
    CHECK(std::get<float>(a2->inputDefault(0)) == doctest::Approx(7.5f));
    CHECK(std::get<std::string>(a2->inputDefault(3)) == "file path.wav");
    CHECK(static_cast<StubB*>(b2)->st == "kept-state");
    REQUIRE(g2.connections().size() == 1);
    CHECK(g2.connections()[0].srcNode == a2->id());
    CHECK(g2.connections()[0].dstNode == b2->id());
    CHECK(g2.transport().bpm == doctest::Approx(99.0));
    REQUIRE(g2.automation().channels().size() == 1);
    const UiAutomationChannel& c2 = g2.automation().channels()[0];
    CHECK(c2.nodeId == a2->id());
    CHECK(c2.outMax == doctest::Approx(3.0f));
    REQUIRE(c2.curve.points.size() == 2);
    CHECK(c2.curve.points[1].value == doctest::Approx(0.75f));
}

TEST_CASE("restoreProject skips unknown node types and remaps the rest") {
    ProjectDoc d;
    DocNode bad; bad.id = 1; bad.x = 0; bad.y = 0; bad.type = "DoesNotExist";
    DocNode ok;  ok.id  = 2; ok.x  = 0; ok.y  = 0; ok.type  = "StubA";
    d.nodes = {bad, ok};
    d.connections.push_back({1, 0, 2, 0});   // references the skipped node -> dropped
    Graph g;
    restoreProject(d, g, stubFactory, [](Node&){});
    CHECK(g.nodes().size() == 1);            // only StubA
    CHECK(g.connections().empty());          // the dangling connection was skipped
}
