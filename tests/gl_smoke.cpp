// Headless render smoke test: builds a Colour -> Output graph, renders one frame
// offscreen, reads back the output texture's centre pixel, and asserts it is the
// ColourNode's default orange. Requires a GL 4.1 context (a hidden GLFW window).
// CTest runs it with WORKING_DIRECTORY = repo root so "shaders/..." resolves.
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <vector>
#include <memory>
#include "core/Graph.h"
#include "modules/ColourNode.h"
#include "modules/OutputNode.h"

using namespace oss;

static int fail(const char* msg) { std::fprintf(stderr, "gl_smoke FAIL: %s\n", msg); return 1; }

int main() {
    if (!glfwInit()) return fail("glfwInit");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "gl_smoke", nullptr, nullptr);
    if (!win) { glfwTerminate(); return fail("createWindow (no offscreen GL context)"); }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) { glfwTerminate(); return fail("gladLoadGL"); }

    Graph g;
    auto colour = std::make_unique<ColourNode>();
    auto output = std::make_unique<OutputNode>();
    colour->initGL();
    output->initGL();
    int cId = g.addNode(std::move(colour));
    int oId = g.addNode(std::move(output));
    if (!g.connect(cId, 0, oId, 0)) { glfwTerminate(); return fail("connect Colour->Output"); }

    g.evaluate(1.0f / 60.0f);

    auto* out = dynamic_cast<OutputNode*>(g.findNode(oId));
    if (!out) { glfwTerminate(); return fail("findNode Output"); }
    TexRef tex = out->current();
    if (tex.id == 0 || tex.w <= 0 || tex.h <= 0) { glfwTerminate(); return fail("output texture not produced"); }

    std::vector<unsigned char> px((size_t)tex.w * tex.h * 4);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

    int cx = tex.w / 2, cy = tex.h / 2;
    size_t i = ((size_t)cy * tex.w + cx) * 4;
    int r = px[i], gg = px[i + 1], b = px[i + 2], a = px[i + 3];
    std::fprintf(stderr, "gl_smoke centre pixel = (%d,%d,%d,%d), expected ~(255,128,25,255)\n", r, gg, b, a);

    auto near = [](int v, int t){ return v >= t - 3 && v <= t + 3; };
    int rc = 0;
    if (!(near(r,255) && near(gg,128) && near(b,25) && near(a,255))) rc = fail("centre pixel not orange");
    else std::fprintf(stderr, "gl_smoke OK: Colour->Output pipeline rendered orange\n");

    glfwDestroyWindow(win);
    glfwTerminate();
    return rc;
}
