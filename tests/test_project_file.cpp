#include <doctest/doctest.h>
#include "core/ProjectFile.h"
#include <string>

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
