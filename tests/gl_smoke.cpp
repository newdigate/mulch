// Headless render smoke test: builds a Colour -> Output graph, renders one frame
// offscreen, reads back the output texture's centre pixel, and asserts it is the
// ColourNode's default orange. Requires a GL 4.1 context (a hidden GLFW window).
// CTest runs it with WORKING_DIRECTORY = repo root so "shaders/..." resolves.
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <vector>
#include <memory>
#include <glm/vec4.hpp>
#include "core/Graph.h"
#include "modules/ColourNode.h"
#include "modules/MixNode.h"
#include "modules/OutputNode.h"
#include "gfx/MeshLoader.h"
#include <meshoptimizer.h>
#include "modules/MeshLoaderNode.h"
#include "modules/ShadedRenderNode.h"
#include "modules/SineWaveNode.h"
#include "modules/SpectrographNode.h"
#include "modules/WireframeNode.h"
#include <chrono>
#include <thread>

using namespace oss;

static int fail(const char* msg) { std::fprintf(stderr, "gl_smoke FAIL: %s\n", msg); return 1; }

static void readCentre(TexRef tex, int& r, int& g, int& b, int& a) {
    std::vector<unsigned char> px((size_t)tex.w * tex.h * 4);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    size_t i = ((size_t)(tex.h/2) * tex.w + (tex.w/2)) * 4;
    r = px[i]; g = px[i+1]; b = px[i+2]; a = px[i+3];
}

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

    auto near = [](int v, int t){ return v >= t - 3 && v <= t + 3; };

    // --- Scenario 1: Colour -> Output (orange) ---
    {
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

        int r, gg, b, a;
        readCentre(tex, r, gg, b, a);
        std::fprintf(stderr, "gl_smoke centre pixel = (%d,%d,%d,%d), expected ~(255,128,25,255)\n", r, gg, b, a);

        if (!(near(r,255) && near(gg,128) && near(b,25) && near(a,255))) {
            glfwTerminate(); return fail("centre pixel not orange");
        }
        std::fprintf(stderr, "gl_smoke OK: Colour->Output pipeline rendered orange\n");
    }

    // --- Scenario 2: Colour(red) + Colour(blue) -> Mix(0.5) -> Output ---
    {
        Graph g2;
        auto red  = std::make_unique<ColourNode>();  red->inputDefault(0)  = glm::vec4(1,0,0,1);
        auto blue = std::make_unique<ColourNode>();   blue->inputDefault(0) = glm::vec4(0,0,1,1);
        auto mix  = std::make_unique<MixNode>();
        auto out2 = std::make_unique<OutputNode>();
        red->initGL(); blue->initGL(); mix->initGL(); out2->initGL();
        int rId = g2.addNode(std::move(red));
        int bId = g2.addNode(std::move(blue));
        int mId = g2.addNode(std::move(mix));
        int oId2 = g2.addNode(std::move(out2));
        if (!g2.connect(rId, 0, mId, 0)) { glfwTerminate(); return fail("connect red->mix.a"); }
        if (!g2.connect(bId, 0, mId, 1)) { glfwTerminate(); return fail("connect blue->mix.b"); }
        if (!g2.connect(mId, 0, oId2, 0)) { glfwTerminate(); return fail("connect mix->output"); }
        g2.evaluate(1.0f/60.0f);
        auto* o2 = dynamic_cast<OutputNode*>(g2.findNode(oId2));
        TexRef t2 = o2->current();
        if (!t2.id) { glfwTerminate(); return fail("mix output texture not produced"); }
        int r,gg,b,a; readCentre(t2, r, gg, b, a);
        std::fprintf(stderr, "gl_smoke mix pixel = (%d,%d,%d,%d), expected ~(128,0,128,255)\n", r,gg,b,a);
        if (!(near(r,128) && near(gg,0) && near(b,128) && near(a,255))) {
            glfwTerminate(); return fail("mix pixel wrong");
        }
        std::fprintf(stderr, "gl_smoke OK: Mix blends red+blue correctly\n");
    }

    // --- Scenario 3: Spectrograph -> Output (synth audio -> FFT -> bars) ---
    {
        Graph g3;
        auto spec = std::make_unique<SpectrographNode>();
        auto out3 = std::make_unique<OutputNode>();
        spec->initGL(); out3->initGL();
        int sId  = g3.addNode(std::move(spec));
        int oId3 = g3.addNode(std::move(out3));
        if (!g3.connect(sId, 0, oId3, 0)) { glfwTerminate(); return fail("connect spectrograph->output"); }
        for (int f = 0; f < 8; ++f) g3.evaluate(1.0f / 60.0f);   // fill the rolling window
        auto* o3 = dynamic_cast<OutputNode*>(g3.findNode(oId3));
        TexRef t3 = o3->current();
        if (!t3.id) { glfwTerminate(); return fail("spectrograph output texture not produced"); }
        std::vector<unsigned char> px((size_t)t3.w * t3.h * 4);
        glBindTexture(GL_TEXTURE_2D, t3.id);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        bool sawBg = false, sawBar = false;
        for (size_t i = 0; i < px.size(); i += 4) {
            int r = px[i], g = px[i+1], b = px[i+2];
            if (r < 40 && g < 40 && b < 40) sawBg = true;          // dark background
            if (g > 200 && r < 90 && b < 170) sawBar = true;        // bright green bar
            if (sawBg && sawBar) break;
        }
        std::fprintf(stderr, "gl_smoke spectrograph: sawBg=%d sawBar=%d\n", (int)sawBg, (int)sawBar);
        if (!(sawBg && sawBar)) { glfwTerminate(); return fail("spectrograph did not render bars"); }
        std::fprintf(stderr, "gl_smoke OK: Spectrograph rendered FFT bars\n");
    }

    // --- Scenario 4: SineWave -> Spectrograph -> Output (audio crosses an edge) ---
    // A connected sine must drive the spectrum. To prove the edge actually
    // carries audio (rather than the spectrograph silently using its internal
    // synth), render a second spectrograph with NO input and assert the two
    // spectra differ -- a single 4 kHz tone has a different shape than the
    // 220 Hz synth + harmonics.
    {
        Graph gc;  // connected: sine -> spectrograph -> output
        auto sine  = std::make_unique<SineWaveNode>();
        sine->inputDefault(0) = 4000.0f;   // distinct tone, well above the synth band
        auto specC = std::make_unique<SpectrographNode>();
        auto outC  = std::make_unique<OutputNode>();
        sine->initGL(); specC->initGL(); outC->initGL();   // sine initGL is a no-op
        int siId = gc.addNode(std::move(sine));
        int spId = gc.addNode(std::move(specC));
        int ocId = gc.addNode(std::move(outC));
        if (!gc.connect(siId, 0, spId, 0)) { glfwTerminate(); return fail("connect sine->spectrograph"); }
        if (!gc.connect(spId, 0, ocId, 0)) { glfwTerminate(); return fail("connect spectrograph->output"); }

        Graph gu;  // unconnected control: spectrograph(synth) -> output
        auto specU = std::make_unique<SpectrographNode>();
        auto outU  = std::make_unique<OutputNode>();
        specU->initGL(); outU->initGL();
        int spuId = gu.addNode(std::move(specU));
        int ouId  = gu.addNode(std::move(outU));
        if (!gu.connect(spuId, 0, ouId, 0)) { glfwTerminate(); return fail("connect synth-spectrograph->output"); }

        for (int f = 0; f < 8; ++f) { gc.evaluate(1.0f / 60.0f); gu.evaluate(1.0f / 60.0f); }

        TexRef tc = dynamic_cast<OutputNode*>(gc.findNode(ocId))->current();
        TexRef tu = dynamic_cast<OutputNode*>(gu.findNode(ouId))->current();
        if (!tc.id || !tu.id) { glfwTerminate(); return fail("sine/spectrograph textures not produced"); }

        std::vector<unsigned char> pc((size_t)tc.w * tc.h * 4), pu((size_t)tu.w * tu.h * 4);
        glBindTexture(GL_TEXTURE_2D, tc.id);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pc.data());
        glBindTexture(GL_TEXTURE_2D, tu.id);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pu.data());

        bool sawBar = false, differ = false;
        for (size_t i = 0; i < pc.size() && !(sawBar && differ); i += 4) {
            if (pc[i+1] > 200 && pc[i] < 90 && pc[i+2] < 170) sawBar = true;   // bright green bar
            if (pc[i] != pu[i] || pc[i+1] != pu[i+1] || pc[i+2] != pu[i+2]) differ = true;
        }
        std::fprintf(stderr, "gl_smoke sine->spectrograph: sawBar=%d differsFromSynth=%d\n",
                     (int)sawBar, (int)differ);
        if (!sawBar)  { glfwTerminate(); return fail("sine->spectrograph rendered no bars"); }
        if (!differ)  { glfwTerminate(); return fail("connected sine produced same spectrum as synth (edge not carrying audio)"); }
        std::fprintf(stderr, "gl_smoke OK: SineWave drives Spectrograph through a connection\n");
    }

    // --- Scenario 5: Spectrograph geometry -> Wireframe -> Output (vertex stream) ---
    // The spectrograph's 2nd output is a VBO of the spectrum as a 3D line strip;
    // the Wireframe node binds that buffer and draws it. Asserts the rendered
    // texture has both the dark background and bright-green line pixels, proving
    // the vertex buffer streamed across the edge and was drawn.
    {
        Graph g5;
        auto spec = std::make_unique<SpectrographNode>();
        auto wire = std::make_unique<WireframeNode>();
        auto out5 = std::make_unique<OutputNode>();
        spec->initGL(); wire->initGL(); out5->initGL();
        int spId = g5.addNode(std::move(spec));
        int wiId = g5.addNode(std::move(wire));
        int oId5 = g5.addNode(std::move(out5));
        if (!g5.connect(spId, 1, wiId, 0)) { glfwTerminate(); return fail("connect spectrograph.geometry->wireframe"); }
        if (!g5.connect(wiId, 0, oId5, 0)) { glfwTerminate(); return fail("connect wireframe->output"); }

        for (int f = 0; f < 8; ++f) g5.evaluate(1.0f / 60.0f);

        TexRef t5 = dynamic_cast<OutputNode*>(g5.findNode(oId5))->current();
        if (!t5.id) { glfwTerminate(); return fail("wireframe texture not produced"); }
        std::vector<unsigned char> px((size_t)t5.w * t5.h * 4);
        glBindTexture(GL_TEXTURE_2D, t5.id);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        bool sawBg = false, sawLine = false;
        for (size_t i = 0; i < px.size() && !(sawBg && sawLine); i += 4) {
            int r = px[i], g = px[i+1], b = px[i+2];
            if (r < 25 && g < 25 && b < 30)            sawBg = true;     // dark background
            if (g > 200 && r < 120 && b < 170)         sawLine = true;   // bright green line
        }
        std::fprintf(stderr, "gl_smoke wireframe: sawBg=%d sawLine=%d\n", (int)sawBg, (int)sawLine);
        if (!sawBg)   { glfwTerminate(); return fail("wireframe background not rendered"); }
        if (!sawLine) { glfwTerminate(); return fail("wireframe line strip not rendered"); }
        std::fprintf(stderr, "gl_smoke OK: Spectrograph geometry streamed to Wireframe and rendered\n");
    }

    // --- Scenario 6: Mesh Loader -> Wireframe -> Output (.obj and .gltf) ---
    // Loads a mesh file, streams its triangle edges as a GL_LINES vertex buffer,
    // and renders it through the Wireframe node. Asserts bright-green line pixels
    // appear, proving the file parsed, the buffer streamed, and it drew.
    {
        // The loader parses on a worker thread, so the geometry appears a few
        // frames after the first evaluate -- poll until it renders (or time out).
        // shaded=false wires the wireframe output (0) to a Wireframe node and
        // looks for green lines; shaded=true wires the shaded output (1) to a
        // Shaded Render node and looks for the lit (bluish) surface.
        auto renderMesh = [](const char* path, bool shaded) -> bool {
            Graph g;
            auto mesh = std::make_unique<MeshLoaderNode>();
            mesh->inputDefault(0) = std::string(path);   // file path (String input)
            std::unique_ptr<Node> render;
            if (shaded) render = std::make_unique<ShadedRenderNode>();
            else        render = std::make_unique<WireframeNode>();
            auto out = std::make_unique<OutputNode>();
            mesh->initGL(); render->initGL(); out->initGL();
            int mId = g.addNode(std::move(mesh));
            int rId = g.addNode(std::move(render));
            int oId = g.addNode(std::move(out));
            int srcPort = shaded ? 1 : 0;
            if (!g.connect(mId, srcPort, rId, 0) || !g.connect(rId, 0, oId, 0)) return false;
            auto* outNode = dynamic_cast<OutputNode*>(g.findNode(oId));

            for (int f = 0; f < 400; ++f) {
                g.evaluate(1.0f / 60.0f);
                TexRef t = outNode->current();
                if (t.id) {
                    std::vector<unsigned char> px((size_t)t.w * t.h * 4);
                    glBindTexture(GL_TEXTURE_2D, t.id);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                    for (size_t i = 0; i < px.size(); i += 4) {
                        int r = px[i], gg = px[i+1], b = px[i+2];
                        if (shaded) { if (b > 60 && b > r && gg > r) return true; }       // lit surface
                        else        { if (gg > 200 && r < 120 && b < 170) return true; }  // green wire
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        };
        if (!renderMesh("tests/assets/tetra.obj", false))     { glfwTerminate(); return fail(".obj mesh did not render a wireframe"); }
        std::fprintf(stderr, "gl_smoke OK: .obj mesh loaded (worker thread) and rendered as wireframe\n");
        if (!renderMesh("tests/assets/triangle.gltf", false)) { glfwTerminate(); return fail(".gltf mesh did not render a wireframe"); }
        std::fprintf(stderr, "gl_smoke OK: .gltf mesh loaded (worker thread) and rendered as wireframe\n");
        if (!renderMesh("tests/assets/tetra.obj", true))      { glfwTerminate(); return fail(".obj mesh did not render shaded"); }
        std::fprintf(stderr, "gl_smoke OK: mesh shaded output rendered as a lit surface\n");
    }

    // --- Scenario 7: loadMeshData reports success / failure for diagnostics ---
    {
        MeshData good = loadMeshData("tests/assets/tetra.obj", 1.0f);
        if (!good.ok || good.tris.empty()) { glfwTerminate(); return fail("loadMeshData should succeed for tetra.obj"); }
        MeshData missing = loadMeshData("tests/assets/does_not_exist.obj", 1.0f);
        if (missing.ok || missing.error.empty()) { glfwTerminate(); return fail("loadMeshData should fail with an error for a missing file"); }
        MeshData badType = loadMeshData("tests/assets/tetra.png", 1.0f);
        if (badType.ok || badType.error.empty()) { glfwTerminate(); return fail("loadMeshData should reject an unsupported extension"); }
        MeshData strip = loadMeshData("tests/assets/strip.gltf", 1.0f);   // TRIANGLE_STRIP -> 2 tris
        if (!strip.ok || strip.tris.size() != 2 * 18) { glfwTerminate(); return fail("loadMeshData should expand a TRIANGLE_STRIP gltf to 2 triangles"); }
        std::fprintf(stderr, "gl_smoke OK: loadMeshData reports errors (missing: \"%s\"), and expands strips\n", missing.error.c_str());
    }

    // --- Scenario 8: a meshopt-compressed gltf decodes (EXT_meshopt_compression) ---
    // Encode the tetra with meshopt, author a gltf that references the compressed
    // data via EXT_meshopt_compression, and confirm loadMeshData decodes it.
    {
        const float pos[12] = { 0,1,0,  -1,-1,1,  1,-1,1,  0,-1,-1 };
        const unsigned int ind[12] = { 0,1,2,  0,2,3,  0,3,1,  1,3,2 };

        std::vector<unsigned char> cpos(meshopt_encodeVertexBufferBound(4, 12));
        cpos.resize(meshopt_encodeVertexBuffer(cpos.data(), cpos.size(), pos, 4, 12));
        std::vector<unsigned char> cidx(meshopt_encodeIndexBufferBound(12, 4));
        cidx.resize(meshopt_encodeIndexBuffer(cidx.data(), cidx.size(), ind, 12));

        // One .bin: [48 zero pos-fallback][24 zero idx-fallback][cpos][cidx].
        std::vector<unsigned char> bin(48 + 24, 0);
        size_t posOff = bin.size(); bin.insert(bin.end(), cpos.begin(), cpos.end());
        size_t idxOff = bin.size(); bin.insert(bin.end(), cidx.begin(), cidx.end());
        if (FILE* f = std::fopen("build/_meshopt.bin", "wb")) { std::fwrite(bin.data(), 1, bin.size(), f); std::fclose(f); }

        std::string g =
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"buffers\":[{\"uri\":\"_meshopt.bin\",\"byteLength\":" + std::to_string(bin.size()) + "}],"
            "\"bufferViews\":["
              "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48,\"byteStride\":12,\"extensions\":{\"EXT_meshopt_compression\":"
                "{\"buffer\":0,\"byteOffset\":" + std::to_string(posOff) + ",\"byteLength\":" + std::to_string(cpos.size()) +
                ",\"byteStride\":12,\"count\":4,\"mode\":\"ATTRIBUTES\"}}},"
              "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":24,\"extensions\":{\"EXT_meshopt_compression\":"
                "{\"buffer\":0,\"byteOffset\":" + std::to_string(idxOff) + ",\"byteLength\":" + std::to_string(cidx.size()) +
                ",\"byteStride\":2,\"count\":12,\"mode\":\"TRIANGLES\"}}}],"
            "\"accessors\":["
              "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
              "{\"bufferView\":1,\"componentType\":5123,\"count\":12,\"type\":\"SCALAR\"}],"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"mode\":4}]}],"
            "\"extensionsUsed\":[\"EXT_meshopt_compression\"],\"extensionsRequired\":[\"EXT_meshopt_compression\"]}";
        if (FILE* f = std::fopen("build/_meshopt.gltf", "wb")) { std::fwrite(g.data(), 1, g.size(), f); std::fclose(f); }

        MeshData m = loadMeshData("build/_meshopt.gltf", 1.0f);
        if (!m.ok || m.tris.size() != 4 * 18) { glfwTerminate(); return fail("meshopt-compressed gltf did not decode to 4 triangles"); }
        std::fprintf(stderr, "gl_smoke OK: EXT_meshopt_compression gltf decoded to %d triangles\n", (int)(m.tris.size() / 18));
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
