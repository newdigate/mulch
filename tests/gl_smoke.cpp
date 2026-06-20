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
#include "modules/CompositorNode.h"
#include "core/BlendModes.h"
#include "modules/OutputNode.h"
#include "gfx/MeshLoader.h"
#include <meshoptimizer.h>
#include "modules/MeshLoaderNode.h"
#include "modules/ShadedRenderNode.h"
#include "modules/SineWaveNode.h"
#include "modules/AudioMixerNode.h"
#include "modules/AudioPlayerNode.h"
#include "modules/RecorderNode.h"
#include "modules/SpectrographNode.h"
#include "audio/AudioFile.h"
#include "modules/TextNode.h"
#include "modules/VideoPlayerNode.h"
#include "modules/WireframeNode.h"
#include "modules/PitchGraphNode.h"
#include "modules/WorldTransformNode.h"
#include "modules/SkyboxNode.h"
#include "modules/DeformNode.h"
#include "modules/VertexShaderNode.h"
#include "core/VertexShaders.h"
#include "gfx/VideoDecoder.h"
#include "gfx/VideoEncoder.h"
#include "gfx/TextGeometry.h"
#include <chrono>
#include <cmath>
#include <thread>

using namespace oss;

static int fail(const char* msg) { std::fprintf(stderr, "gl_smoke FAIL: %s\n", msg); return 1; }

// 2D point-in-triangle (sign-of-cross-products).
static bool pointInTri(float px, float py, float ax, float ay,
                       float bx, float by, float cx, float cy) {
    float d1 = (px-bx)*(ay-by) - (ax-bx)*(py-by);
    float d2 = (px-cx)*(by-cy) - (bx-cx)*(py-cy);
    float d3 = (px-ax)*(cy-ay) - (cx-ax)*(py-ay);
    bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}
// Is (x,y) covered by any front-facing (normal.z>0) triangle of the text mesh?
static bool coveredByFront(const TextGeometry& g, float x, float y) {
    for (size_t i = 0; i + 17 < g.tris.size(); i += 18) {   // 3 verts * 6 floats
        if (g.tris[i + 5] <= 0.0f) continue;                // front faces only
        if (pointInTri(x, y, g.tris[i], g.tris[i+1],
                       g.tris[i+6], g.tris[i+7], g.tris[i+12], g.tris[i+13]))
            return true;
    }
    return false;
}

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
        MeshData v1 = loadMeshData("tests/assets/v1.gltf", 1.0f);         // glTF 1.0 -> clear error
        if (v1.ok || v1.error.find("1.0") == std::string::npos) { glfwTerminate(); return fail("loadMeshData should flag glTF 1.0 with a clear message"); }
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

    // --- Scenario 9: a Draco-compressed gltf decodes (KHR_draco_mesh_compression) ---
    // tetra.drc was produced by draco_encoder from tetra.obj; tinygltf decodes it
    // via the linked draco library (TINYGLTF_ENABLE_DRACO).
    {
        MeshData d = loadMeshData("tests/assets/tetra_draco.gltf", 1.0f);
        if (!d.ok || d.tris.size() != 4 * 18) { glfwTerminate(); return fail("Draco-compressed gltf did not decode to 4 triangles"); }
        std::fprintf(stderr, "gl_smoke OK: KHR_draco_mesh_compression gltf decoded to %d triangles\n",
                     (int)(d.tris.size() / 18));
    }

    // --- Scenario 10: Video Player decodes a file to texture + audio ---
    // Decodes tests/assets/test.mp4 (a 128x96 colour pattern with a 330 Hz tone),
    // first through the bare VideoDecoder, then through the VideoPlayerNode wired
    // to an Output -- checking the picture becomes a non-black texture, the node
    // emits non-silent audio, the playhead advances forward, and a negative rate
    // walks it backwards (reverse playback).
    {
        // (a) VideoDecoder produces video frames and resampled audio directly.
        VideoDecoder dec;
        std::string err;
        if (!dec.open("tests/assets/test.mp4", err)) {
            glfwTerminate(); return fail(("video open failed: " + err).c_str());
        }
        if (dec.width() != 128 || dec.height() != 96) { glfwTerminate(); return fail("video dimensions wrong"); }
        if (!dec.hasAudio()) { glfwTerminate(); return fail("test video should have an audio track"); }

        VideoFrame vf;
        std::vector<float> audio; double aStart = 0.0; bool aValid = false;
        int frames = 0;
        while (frames < 8 && dec.decodeFrame(vf, audio, aStart, aValid)) ++frames;
        if (frames == 0) { glfwTerminate(); return fail("decoded no video frames"); }
        bool audioNonZero = false;
        for (float s : audio) if (s > 0.01f || s < -0.01f) { audioNonZero = true; break; }
        if (audio.empty() || !audioNonZero) { glfwTerminate(); return fail("decoded no (non-silent) audio"); }
        std::fprintf(stderr, "gl_smoke OK: VideoDecoder decoded %d frames + %zu audio samples\n",
                     frames, audio.size());

        // (b) VideoPlayerNode -> Output: forward play, non-black texture + audio.
        Graph g;
        auto vid = std::make_unique<VideoPlayerNode>();
        vid->inputDefault(0) = std::string("tests/assets/test.mp4");   // file
        vid->inputDefault(3) = false;                                   // loop off (deterministic)
        auto out = std::make_unique<OutputNode>();
        vid->initGL(); out->initGL();
        int vId = g.addNode(std::move(vid));
        int oId = g.addNode(std::move(out));
        if (!g.connect(vId, 0, oId, 0)) { glfwTerminate(); return fail("connect Video->Output"); }
        auto* outNode = dynamic_cast<OutputNode*>(g.findNode(oId));
        auto* vidNode = dynamic_cast<VideoPlayerNode*>(g.findNode(vId));

        // Traverse most of the clip so the sliding window has to extend forward
        // across several keyframes, accumulating "did we ever see picture/audio".
        bool sawColour = false, sawNodeAudio = false;
        for (int f = 0; f < 12; ++f) {
            g.evaluate(1.0f / 10.0f);   // ~10 fps clip, 12 frames ~= 1.2s
            AudioRef na = vidNode->audioOut();
            for (std::size_t i = 0; i < na.count; ++i)
                if (na.samples[i] > 0.01f || na.samples[i] < -0.01f) { sawNodeAudio = true; break; }
            TexRef t = outNode->current();
            if (t.id) {
                std::vector<unsigned char> px((size_t)t.w * t.h * 4);
                glBindTexture(GL_TEXTURE_2D, t.id);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                for (size_t i = 0; i < px.size(); i += 4)
                    if (px[i] > 30 || px[i+1] > 30 || px[i+2] > 30) { sawColour = true; break; }
            }
        }
        if (!sawColour)    { glfwTerminate(); return fail("video produced no visible texture"); }
        if (!sawNodeAudio) { glfwTerminate(); return fail("video node emitted no audio"); }

        double fwd = vidNode->playhead();
        if (!(fwd > 0.5)) { glfwTerminate(); return fail("playhead did not advance through the clip on forward play"); }

        // (c) reverse: a negative rate walks the playhead backwards, forcing the
        // window to re-seek to an earlier keyframe and rebuild.
        vidNode->inputDefault(1) = -1.0f;   // rate
        double before = vidNode->playhead();
        for (int f = 0; f < 5; ++f) g.evaluate(1.0f / 10.0f);
        double after = vidNode->playhead();
        if (!(after < before)) { glfwTerminate(); return fail("playhead did not move backwards on reverse play"); }
        std::fprintf(stderr, "gl_smoke OK: VideoPlayer rendered + audio, advanced to %.2fs, reversed %.2f->%.2f\n",
                     fwd, before, after);
    }

    // --- Scenario 11: Text 2D / Text 3D -> geometry -> renderers ---
    // buildTextGeometry turns a string into filled glyph triangles (+ outline
    // lines); flat text faces +Z, extruded text adds back/side faces with other
    // normals. Then the Text nodes stream those buffers into Shaded Render and
    // Wireframe, which must produce lit and green-line pixels respectively.
    {
        TextGeometry flat = buildTextGeometry("A", OSS_DEFAULT_FONT, 1.0f, 0.0f);
        if (!flat.ok || flat.tris.empty() || flat.lines.empty()) {
            glfwTerminate(); return fail("flat text geometry is empty");
        }
        bool flatAllFront = true;
        for (size_t i = 0; i + 5 < flat.tris.size(); i += 6)
            if (flat.tris[i + 5] < 0.9f) { flatAllFront = false; break; }   // normal.z
        if (!flatAllFront) { glfwTerminate(); return fail("flat text normals should all face +Z"); }

        TextGeometry solid = buildTextGeometry("A", OSS_DEFAULT_FONT, 1.0f, 0.3f);
        if (!solid.ok || solid.tris.size() <= flat.tris.size()) {
            glfwTerminate(); return fail("extruded text should add geometry over flat");
        }
        bool sawBackOrSide = false;
        for (size_t i = 0; i + 5 < solid.tris.size(); i += 6) {
            float nx = solid.tris[i + 3], ny = solid.tris[i + 4], nz = solid.tris[i + 5];
            if (nz < -0.5f || std::fabs(nx) + std::fabs(ny) > 0.5f) { sawBackOrSide = true; break; }
        }
        if (!sawBackOrSide) { glfwTerminate(); return fail("extruded text has no back/side faces"); }
        std::fprintf(stderr, "gl_smoke OK: text geometry flat=%zu tris, solid=%zu tris\n",
                     flat.tris.size() / 18, solid.tris.size() / 18);

        // Holes must actually be cut: the centre of 'o' (its counter) stays empty,
        // while the centre of 'I' (a solid bar) is filled. Both are centred on the
        // origin, so (0,0) is the glyph centre.
        TextGeometry o = buildTextGeometry("o", OSS_DEFAULT_FONT, 1.0f, 0.0f);
        TextGeometry bar = buildTextGeometry("I", OSS_DEFAULT_FONT, 1.0f, 0.0f);
        if (!o.ok || !bar.ok) { glfwTerminate(); return fail("text geometry for o/I failed"); }
        if (coveredByFront(o, 0.0f, 0.0f))   { glfwTerminate(); return fail("'o' centre should be a hole, not filled"); }
        if (!coveredByFront(bar, 0.0f, 0.0f)) { glfwTerminate(); return fail("'I' centre should be filled"); }
        std::fprintf(stderr, "gl_smoke OK: glyph holes are cut ('o' counter empty, 'I' filled)\n");

        // Text 3D -> Shaded Render -> Output: a lit (bluish) surface.
        {
            Graph g;
            auto txt = std::make_unique<Text3DNode>();
            txt->inputDefault(0) = std::string("3D");
            auto shade = std::make_unique<ShadedRenderNode>();
            auto out = std::make_unique<OutputNode>();
            txt->initGL(); shade->initGL(); out->initGL();
            int tId = g.addNode(std::move(txt));
            int sId = g.addNode(std::move(shade));
            int oId = g.addNode(std::move(out));
            if (!g.connect(tId, 1, sId, 0) || !g.connect(sId, 0, oId, 0)) {
                glfwTerminate(); return fail("connect Text3D->Shaded->Output");
            }
            auto* on = dynamic_cast<OutputNode*>(g.findNode(oId));
            bool lit = false;
            for (int f = 0; f < 5 && !lit; ++f) {
                g.evaluate(1.0f / 60.0f);
                TexRef t = on->current();
                if (t.id) {
                    std::vector<unsigned char> px((size_t)t.w * t.h * 4);
                    glBindTexture(GL_TEXTURE_2D, t.id);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                    for (size_t i = 0; i < px.size(); i += 4) {
                        int r = px[i], gg = px[i+1], b = px[i+2];
                        if (b > 60 && b > r && gg > r) { lit = true; break; }
                    }
                }
            }
            if (!lit) { glfwTerminate(); return fail("Text 3D did not render a lit surface"); }
        }

        // Text 2D -> Wireframe -> Output: green outline lines.
        {
            Graph g;
            auto txt = std::make_unique<Text2DNode>();
            txt->inputDefault(0) = std::string("2D");
            auto wire = std::make_unique<WireframeNode>();
            auto out = std::make_unique<OutputNode>();
            txt->initGL(); wire->initGL(); out->initGL();
            int tId = g.addNode(std::move(txt));
            int wId = g.addNode(std::move(wire));
            int oId = g.addNode(std::move(out));
            if (!g.connect(tId, 0, wId, 0) || !g.connect(wId, 0, oId, 0)) {
                glfwTerminate(); return fail("connect Text2D->Wireframe->Output");
            }
            auto* on = dynamic_cast<OutputNode*>(g.findNode(oId));
            bool green = false;
            for (int f = 0; f < 5 && !green; ++f) {
                g.evaluate(1.0f / 60.0f);
                TexRef t = on->current();
                if (t.id) {
                    std::vector<unsigned char> px((size_t)t.w * t.h * 4);
                    glBindTexture(GL_TEXTURE_2D, t.id);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                    for (size_t i = 0; i < px.size(); i += 4) {
                        int r = px[i], gg = px[i+1], b = px[i+2];
                        if (gg > 200 && r < 120 && b < 170) { green = true; break; }
                    }
                }
            }
            if (!green) { glfwTerminate(); return fail("Text 2D did not render outline lines"); }
        }
        std::fprintf(stderr, "gl_smoke OK: Text 2D (wireframe) + Text 3D (shaded) rendered\n");
    }

    // --- Scenario 12: Recorder / VideoEncoder write a decodable movie ---
    {
        // (a) VideoEncoder round-trip: encode synthetic frames + a tone, then
        // decode the file back with VideoDecoder and confirm it round-trips.
        const char* path = "build/_rec_rt.mp4";
        const int W = 64, H = 48, FPS = 30, SR = 48000;
        {
            VideoEncoder enc;
            std::string err;
            if (!enc.open(path, W, H, FPS, SR, 1, err)) {   // mono
                glfwTerminate(); return fail(("encoder open failed: " + err).c_str());
            }
            std::vector<unsigned char> frame((size_t)W * H * 4, 0);
            std::vector<float> aud(SR / FPS);
            double phase = 0.0;
            for (int f = 0; f < 15; ++f) {
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                    size_t i = ((size_t)y * W + x) * 4;
                    frame[i]   = (unsigned char)((x * 4 + f * 8) & 255);
                    frame[i+1] = (unsigned char)((y * 5) & 255);
                    frame[i+2] = (unsigned char)((f * 15) & 255);
                    frame[i+3] = 255;
                }
                enc.addVideoFrame(frame.data(), f / (double)FPS);
                for (float& s : aud) { s = 0.5f * std::sin((float)phase); phase += 2*3.14159265f*440.0f/SR; }
                enc.addAudio(aud.data(), (int)aud.size());
            }
            std::string e2; enc.close(e2);
        }
        VideoDecoder dec;
        std::string err;
        if (!dec.open(path, err)) { glfwTerminate(); return fail(("decode encoded file failed: " + err).c_str()); }
        if (dec.width() != W || dec.height() != H) { glfwTerminate(); return fail("encoded video has wrong dimensions"); }
        if (!dec.hasAudio()) { glfwTerminate(); return fail("encoded file has no audio stream"); }
        if (dec.audioChannels() != 1) { glfwTerminate(); return fail("mono encode should yield a 1-channel file"); }
        VideoFrame vf; std::vector<float> audio; double aS = 0; bool aV = false; int got = 0;
        while (got < 5 && dec.decodeFrame(vf, audio, aS, aV)) ++got;
        if (got == 0) { glfwTerminate(); return fail("encoded file decoded no frames"); }
        bool nz = false; for (float s : audio) if (s > 0.01f || s < -0.01f) { nz = true; break; }
        if (!nz) { glfwTerminate(); return fail("encoded audio is silent"); }
        std::fprintf(stderr, "gl_smoke OK: VideoEncoder mono round-trip (%d frames, audio) decodes\n", got);

        // Stereo round-trip: encode interleaved L/R and confirm the file is 2-channel.
        {
            const char* sp = "build/_rec_stereo.mp4";
            {
                VideoEncoder enc; std::string e;
                if (!enc.open(sp, W, H, FPS, SR, 2, e)) { glfwTerminate(); return fail(("stereo encoder open: " + e).c_str()); }
                std::vector<unsigned char> frame((size_t)W * H * 4, 200);
                std::vector<float> st((SR / FPS) * 2);
                double pl = 0, pr = 0;
                for (int f = 0; f < 15; ++f) {
                    enc.addVideoFrame(frame.data(), f / (double)FPS);
                    for (size_t i = 0; i < st.size(); i += 2) {
                        st[i]   = 0.5f * std::sin((float)pl); pl += 2*3.14159265f*440.0f/SR;   // L
                        st[i+1] = 0.5f * std::sin((float)pr); pr += 2*3.14159265f*880.0f/SR;   // R
                    }
                    enc.addAudio(st.data(), (int)st.size());
                }
                std::string e2; enc.close(e2);
            }
            VideoDecoder ds; std::string e;
            if (!ds.open(sp, e)) { glfwTerminate(); return fail(("decode stereo file: " + e).c_str()); }
            if (ds.audioChannels() != 2) { glfwTerminate(); return fail("stereo encode should yield a 2-channel file"); }
            std::fprintf(stderr, "gl_smoke OK: VideoEncoder stereo round-trip yields a 2-channel file\n");
        }

        // (b) RecorderNode: passes video through unchanged, and records a file.
        Graph g;
        auto col = std::make_unique<ColourNode>();
        auto recN = std::make_unique<RecorderNode>();
        recN->inputDefault(3) = std::string("build/_rec_node.mp4");   // file
        auto out = std::make_unique<OutputNode>();
        col->initGL(); recN->initGL(); out->initGL();
        int cId = g.addNode(std::move(col));
        int rId = g.addNode(std::move(recN));
        int oId = g.addNode(std::move(out));
        if (!g.connect(cId, 0, rId, 0) || !g.connect(rId, 0, oId, 0)) {
            glfwTerminate(); return fail("connect Colour->Recorder->Output");
        }
        auto* on = dynamic_cast<OutputNode*>(g.findNode(oId));
        auto* rn = dynamic_cast<RecorderNode*>(g.findNode(rId));

        g.evaluate(1.0f / 60.0f);   // not recording -> pure pass-through
        TexRef passed = on->current();
        if (!passed.id) { glfwTerminate(); return fail("recorder did not pass video through"); }
        int r, gg, b, a; readCentre(passed, r, gg, b, a);
        if (!(near(r,255) && near(gg,128) && near(b,25))) {
            glfwTerminate(); return fail("passed-through texture is not the Colour output");
        }
        int vw = passed.w, vh = passed.h;

        rn->inputDefault(2) = true;                          // record on
        for (int f = 0; f < 12; ++f) g.evaluate(1.0f / 60.0f);
        rn->inputDefault(2) = false;                         // record off -> finalise file
        g.evaluate(1.0f / 60.0f);

        VideoDecoder dec2;
        std::string err2;
        if (!dec2.open("build/_rec_node.mp4", err2)) { glfwTerminate(); return fail(("recorded file did not open: " + err2).c_str()); }
        if (dec2.width() != vw || dec2.height() != vh) { glfwTerminate(); return fail("recorded video has wrong dimensions"); }
        VideoFrame vf2; std::vector<float> au2; double s2 = 0; bool v2 = false; int got2 = 0;
        while (got2 < 3 && dec2.decodeFrame(vf2, au2, s2, v2)) ++got2;
        if (got2 == 0) { glfwTerminate(); return fail("recorded file decoded no frames"); }
        std::fprintf(stderr, "gl_smoke OK: Recorder passed video through and wrote a decodable %dx%d mp4\n", vw, vh);

        // (c) End-to-end: two sines panned through the Mixer feed the Recorder's
        // audio; the recorded file must contain an audio track. (The Mixer now emits
        // mono on port 0; a later task rewires this to record stereo from L/R.)
        {
            Graph gs;
            auto col2 = std::make_unique<ColourNode>();
            auto s1 = std::make_unique<SineWaveNode>(); s1->inputDefault(0) = 300.0f;
            auto s2 = std::make_unique<SineWaveNode>(); s2->inputDefault(0) = 600.0f;
            auto mix = std::make_unique<AudioMixerNode>();
            mix->inputDefault(2) = -1.0f;   // pan 1 -> hard left
            mix->inputDefault(5) =  1.0f;   // pan 2 -> hard right
            auto rec2 = std::make_unique<RecorderNode>();
            rec2->inputDefault(3) = std::string("build/_rec_stereo_node.mp4");
            col2->initGL(); s1->initGL(); s2->initGL(); mix->initGL(); rec2->initGL();
            int cId2 = gs.addNode(std::move(col2));
            int s1Id = gs.addNode(std::move(s1));
            int s2Id = gs.addNode(std::move(s2));
            int mId  = gs.addNode(std::move(mix));
            int r2Id = gs.addNode(std::move(rec2));
            if (!gs.connect(s1Id, 0, mId, 0) || !gs.connect(s2Id, 0, mId, 3) ||
                !gs.connect(cId2, 0, r2Id, 0) || !gs.connect(mId, 0, r2Id, 1)) {
                glfwTerminate(); return fail("connect stereo record graph");
            }
            auto* rn2 = dynamic_cast<RecorderNode*>(gs.findNode(r2Id));
            rn2->inputDefault(2) = true;                          // record on
            for (int f = 0; f < 16; ++f) gs.evaluate(1.0f / 60.0f);
            rn2->inputDefault(2) = false;                        // record off -> finalise
            gs.evaluate(1.0f / 60.0f);

            VideoDecoder d3; std::string e3;
            if (!d3.open("build/_rec_stereo_node.mp4", e3)) { glfwTerminate(); return fail(("stereo recording did not open: " + e3).c_str()); }
            if (!d3.hasAudio() || d3.audioChannels() < 1) { glfwTerminate(); return fail("graph recording should have an audio track"); }
            VideoFrame vf3; std::vector<float> au3; double s3 = 0; bool v3 = false; int got3 = 0;
            while (got3 < 3 && d3.decodeFrame(vf3, au3, s3, v3)) ++got3;
            if (got3 == 0) { glfwTerminate(); return fail("stereo recording decoded no frames"); }
            std::fprintf(stderr, "gl_smoke OK: Sine->Mixer(pan)->Recorder wrote a movie with audio\n");
        }
    }

    // --- Scenario 13: Audio File player (stereo, forward + reverse) ---
    {
        // (a) decode the whole file to interleaved 48 kHz stereo.
        AudioClip clip = decodeAudioFile("tests/assets/test.mp4");
        if (!clip.ok || clip.channels != 2 || clip.frames() == 0) {
            glfwTerminate(); return fail(("decodeAudioFile failed: " + clip.error).c_str());
        }
        bool clipNz = false;
        for (float s : clip.samples) if (s > 0.01f || s < -0.01f) { clipNz = true; break; }
        if (!clipNz) { glfwTerminate(); return fail("decoded audio clip is silent"); }
        std::fprintf(stderr, "gl_smoke OK: decodeAudioFile -> %zu stereo frames\n", clip.frames());

        // A different container/codec (.mp3) decodes through the same path -- the
        // player inherits FFmpeg's format coverage (mp3, wav, flac, ogg, m4a, ...).
        AudioClip mp3 = decodeAudioFile("tests/assets/tone.mp3");
        if (!mp3.ok || mp3.channels != 2 || mp3.frames() == 0) {
            glfwTerminate(); return fail(("decodeAudioFile failed for mp3: " + mp3.error).c_str());
        }
        std::fprintf(stderr, "gl_smoke OK: decodeAudioFile loads .mp3 -> %zu stereo frames\n", mp3.frames());

        // (b) the node plays it (decoded on a worker thread): stereo + advancing.
        Graph g;
        auto ap = std::make_unique<AudioPlayerNode>();
        ap->inputDefault(0) = std::string("tests/assets/test.mp4");
        int aId = g.addNode(std::move(ap));
        auto* an = dynamic_cast<AudioPlayerNode*>(g.findNode(aId));

        bool sawAudio = false;
        for (int f = 0; f < 400 && !sawAudio; ++f) {     // poll while the worker decodes
            g.evaluate(1.0f / 60.0f);
            AudioRef oL = an->leftOut();
            AudioRef oR = an->rightOut();
            if (oL.count > 0 && oR.count > 0)
                for (std::size_t i = 0; i < oL.count; ++i)
                    if (std::fabs(oL.samples[i]) > 0.01f || std::fabs(oR.samples[i]) > 0.01f) { sawAudio = true; break; }
            if (!sawAudio) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!sawAudio) { glfwTerminate(); return fail("audio player produced no audio"); }

        for (int f = 0; f < 10; ++f) g.evaluate(1.0f / 60.0f);   // play forward a bit
        double fwd = an->playhead();
        if (!(fwd > 0.0)) { glfwTerminate(); return fail("audio playhead did not advance forward"); }

        an->inputDefault(1) = -1.0f;                     // rate: reverse
        an->inputDefault(3) = false;                     // loop off (deterministic)
        double before = an->playhead();
        for (int f = 0; f < 5; ++f) g.evaluate(1.0f / 60.0f);
        double after = an->playhead();
        if (!(after < before)) { glfwTerminate(); return fail("audio playhead did not move backwards on reverse"); }
        std::fprintf(stderr, "gl_smoke OK: Audio File played stereo, advanced %.2fs then reversed %.2f->%.2f\n",
                     fwd, before, after);
    }

    // --- Scenario 14: a shared World Transform aligns two renderers ---
    // The same triangle is streamed as lines to Wireframe and as triangles to
    // Shaded Render, both driven by one World Transform. With a shared rotation
    // and matching cameras the two views register: the wireframe outline's centroid
    // lands inside the shaded fill's bounding box.
    {
        const float tris[] = {
            -0.6f, -0.4f, 0.0f,  0,0,1,
             0.6f, -0.4f, 0.0f,  0,0,1,
             0.0f,  0.7f, 0.0f,  0,0,1,
        };
        const float lines[] = {
            -0.6f,-0.4f,0,  0.6f,-0.4f,0,
             0.6f,-0.4f,0,  0.0f, 0.7f,0,
             0.0f, 0.7f,0, -0.6f,-0.4f,0,
        };
        GLuint trisVbo = 0, linesVbo = 0;
        glGenBuffers(1, &trisVbo);  glBindBuffer(GL_ARRAY_BUFFER, trisVbo);  glBufferData(GL_ARRAY_BUFFER, sizeof(tris),  tris,  GL_STATIC_DRAW);
        glGenBuffers(1, &linesVbo); glBindBuffer(GL_ARRAY_BUFFER, linesVbo); glBufferData(GL_ARRAY_BUFFER, sizeof(lines), lines, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        Graph g;
        auto wt = std::make_unique<WorldTransformNode>();
        wt->inputDefault(0) = 0.0f;     // rate 0 -> angle stays 0 (deterministic)
        auto wire = std::make_unique<WireframeNode>();
        wire->inputDefault(0) = VertexRef{linesVbo, 6, Primitive::Lines, VertexFormat::Pos3};
        auto shade = std::make_unique<ShadedRenderNode>();
        shade->inputDefault(0) = VertexRef{trisVbo, 3, Primitive::Triangles, VertexFormat::Pos3Normal3};
        auto outW = std::make_unique<OutputNode>();
        auto outS = std::make_unique<OutputNode>();
        wire->initGL(); shade->initGL(); outW->initGL(); outS->initGL();
        int wtId = g.addNode(std::move(wt));
        int wId  = g.addNode(std::move(wire));
        int sId  = g.addNode(std::move(shade));
        int owId = g.addNode(std::move(outW));
        int osId = g.addNode(std::move(outS));
        if (!g.connect(wtId, 0, wId, 2) || !g.connect(wtId, 0, sId, 3) ||   // shared transform
            !g.connect(wId, 0, owId, 0) || !g.connect(sId, 0, osId, 0)) {
            glfwTerminate(); return fail("connect shared-transform graph");
        }
        g.evaluate(1.0f / 60.0f);

        TexRef tw = dynamic_cast<OutputNode*>(g.findNode(owId))->current();
        TexRef ts = dynamic_cast<OutputNode*>(g.findNode(osId))->current();
        std::vector<unsigned char> pw((size_t)tw.w * tw.h * 4), ps((size_t)ts.w * ts.h * 4);
        glBindTexture(GL_TEXTURE_2D, tw.id); glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pw.data());
        glBindTexture(GL_TEXTURE_2D, ts.id); glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, ps.data());

        double gsx = 0, gsy = 0; long gn = 0;                         // wireframe green centroid
        int litMinX = 1e9, litMinY = 1e9, litMaxX = -1, litMaxY = -1; // shaded lit bbox
        long ln = 0;
        for (int y = 0; y < tw.h; ++y) for (int x = 0; x < tw.w; ++x) {
            size_t i = ((size_t)y * tw.w + x) * 4;
            if (pw[i+1] > 200 && pw[i] < 120 && pw[i+2] < 170) { gsx += x; gsy += y; ++gn; }
            int r = ps[i], gg = ps[i+1], b = ps[i+2];
            if (b > 60 && b > r && gg > r) { litMinX = std::min(litMinX,x); litMaxX = std::max(litMaxX,x);
                                             litMinY = std::min(litMinY,y); litMaxY = std::max(litMaxY,y); ++ln; }
        }
        if (gn == 0) { glfwTerminate(); return fail("shared-transform wireframe drew nothing"); }
        if (ln == 0) { glfwTerminate(); return fail("shared-transform shaded drew nothing"); }
        double gcx = gsx / gn, gcy = gsy / gn;
        bool aligned = gcx >= litMinX - 5 && gcx <= litMaxX + 5 && gcy >= litMinY - 5 && gcy <= litMaxY + 5;
        if (!aligned) { glfwTerminate(); return fail("renderers not aligned: wireframe centroid outside shaded bbox"); }
        std::fprintf(stderr, "gl_smoke OK: shared World Transform aligns Wireframe + Shaded (centroid %.0f,%.0f in bbox)\n", gcx, gcy);

        glDeleteBuffers(1, &trisVbo); glDeleteBuffers(1, &linesVbo);
    }

    // --- Scenario 15: Compositor blends two colours; shader matches the C++ reference ---
    // Feed two solid colours into the Compositor and assert the rendered centre pixel
    // matches blendPixel() for one mode per code path: Multiply (separable), Hue
    // (non-separable setSat/setLum), XOR (bitwise). The reference is computed on the
    // 8-bit-quantised inputs (what the textures actually carry) so only output rounding
    // can differ; the near() tolerance is +/-3.
    {
        auto quant = [](glm::vec3 c) {
            return glm::vec3(std::round(c.x*255.0f)/255.0f,
                             std::round(c.y*255.0f)/255.0f,
                             std::round(c.z*255.0f)/255.0f);
        };
        auto check = [&](int mode, glm::vec3 ca, glm::vec3 cb) -> bool {
            Graph g;
            auto a = std::make_unique<ColourNode>(); a->inputDefault(0) = glm::vec4(ca, 1.0f);
            auto b = std::make_unique<ColourNode>(); b->inputDefault(0) = glm::vec4(cb, 1.0f);
            auto comp = std::make_unique<CompositorNode>();
            comp->inputDefault(2) = (float)mode;   // mode
            comp->inputDefault(3) = 1.0f;          // opacity
            auto out = std::make_unique<OutputNode>();
            a->initGL(); b->initGL(); comp->initGL(); out->initGL();
            int aId = g.addNode(std::move(a));
            int bId = g.addNode(std::move(b));
            int cId = g.addNode(std::move(comp));
            int oId = g.addNode(std::move(out));
            if (!g.connect(aId,0,cId,0) || !g.connect(bId,0,cId,1) || !g.connect(cId,0,oId,0)) return false;
            g.evaluate(1.0f/60.0f);
            TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
            if (!t.id) return false;
            int r, gg, bb, aa; readCentre(t, r, gg, bb, aa);
            glm::vec3 e = blendPixel(mode, quant(ca), quant(cb));
            int er = (int)std::lround(e.x*255.0f), eg = (int)std::lround(e.y*255.0f), eb = (int)std::lround(e.z*255.0f);
            std::fprintf(stderr, "gl_smoke compositor mode %d: got (%d,%d,%d) expected (%d,%d,%d)\n",
                         mode, r, gg, bb, er, eg, eb);
            return near(r,er) && near(gg,eg) && near(bb,eb);
        };
        glm::vec3 ca(0.2f, 0.5f, 0.8f), cb(0.9f, 0.3f, 0.1f);   // distinct channels (no setSat ties)
        if (!check(5,  ca, cb)) { glfwTerminate(); return fail("Compositor Multiply mismatch vs reference"); }
        if (!check(16, ca, cb)) { glfwTerminate(); return fail("Compositor Hue mismatch vs reference"); }
        if (!check(22, ca, cb)) { glfwTerminate(); return fail("Compositor XOR mismatch vs reference"); }
        std::fprintf(stderr, "gl_smoke OK: Compositor shader matches blendPixel (Multiply/Hue/XOR)\n");
    }

    // --- Scenario 16: Wireframe draws a per-vertex-coloured line (Pos3Color3) ---
    // A hand-built coloured VBO (a red horizontal line) fed to the Wireframe node must
    // render RED, not the node's default green -- proving the Pos3Color3 colored path.
    {
        const float verts[] = {
            -0.5f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,   // (x,y,z, r,g,b)
             0.5f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,
        };
        GLuint vbo = 0;
        glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        Graph g;
        auto wire = std::make_unique<WireframeNode>();
        wire->inputDefault(0) = VertexRef{vbo, 2, Primitive::Lines, VertexFormat::Pos3Color3};
        wire->inputDefault(1) = 0.0f;   // spin off -> static
        auto out = std::make_unique<OutputNode>();
        wire->initGL(); out->initGL();
        int wId = g.addNode(std::move(wire));
        int oId = g.addNode(std::move(out));
        if (!g.connect(wId, 0, oId, 0)) { glfwTerminate(); return fail("connect colour-wire->output"); }
        g.evaluate(1.0f/60.0f);
        TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
        if (!t.id) { glfwTerminate(); return fail("coloured wireframe texture not produced"); }
        std::vector<unsigned char> px((size_t)t.w * t.h * 4);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        bool sawRed = false;
        for (size_t i = 0; i < px.size(); i += 4)
            if (px[i] > 150 && px[i+1] < 80 && px[i+2] < 80) { sawRed = true; break; }
        glDeleteBuffers(1, &vbo);
        if (!sawRed) { glfwTerminate(); return fail("coloured wireframe did not render a red line (Pos3Color3 path)"); }
        std::fprintf(stderr, "gl_smoke OK: Wireframe renders per-vertex colour (Pos3Color3)\n");
    }

    // --- Scenario 17: Pitch Graph turns MIDI into a coloured pitch-vs-time graph ---
    // Feed three note-ons (pitch classes 0/4/7) into a Pitch Graph -> Wireframe; the
    // rendered texture must contain a RED line (note 60, pitch class 0 -> hue 0), which
    // the default-green wireframe could never produce -- proving MIDI -> coloured geometry
    // -> Wireframe end to end.
    {
        std::vector<MidiEvent> on = { midiNoteOn(60, 110), midiNoteOn(64, 110), midiNoteOn(67, 110) };
        Graph g;
        auto pg = std::make_unique<PitchGraphNode>();
        auto wire = std::make_unique<WireframeNode>();
        wire->inputDefault(1) = 0.0f;   // spin off -> static
        auto out = std::make_unique<OutputNode>();
        pg->initGL(); wire->initGL(); out->initGL();
        int pId = g.addNode(std::move(pg));
        int wId = g.addNode(std::move(wire));
        int oId = g.addNode(std::move(out));
        if (!g.connect(pId, 0, wId, 0) || !g.connect(wId, 0, oId, 0)) { glfwTerminate(); return fail("connect pitchgraph->wire->output"); }
        auto* pn = dynamic_cast<PitchGraphNode*>(g.findNode(pId));
        pn->inputDefault(0) = MidiRef{on.data(), on.size()};   // note-ons this frame
        g.evaluate(1.0f/60.0f);                                 // ingest the notes
        pn->inputDefault(0) = MidiRef{};                        // no further events
        for (int f = 0; f < 4; ++f) g.evaluate(1.0f/60.0f);    // hold + scroll a little
        TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
        if (!t.id) { glfwTerminate(); return fail("pitch graph texture not produced"); }
        std::vector<unsigned char> px((size_t)t.w * t.h * 4);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        bool sawRed = false;
        for (size_t i = 0; i < px.size(); i += 4)
            if (px[i] > 120 && px[i+1] < 80 && px[i+2] < 80) { sawRed = true; break; }
        if (!sawRed) { glfwTerminate(); return fail("pitch graph did not render note 60 as a red line"); }
        std::fprintf(stderr, "gl_smoke OK: Pitch Graph -> Wireframe renders MIDI as a coloured pitch graph\n");
    }

    // --- Scenario 18: Skybox samples 6 face textures as a cubemap, rotated by yaw/pitch ---
    // Six Colour nodes (distinct colours) -> the 6 Skybox face inputs -> Output. With the
    // transform fixed, the CENTRE pixel (ray (0,0,-1) before rotation) looks at a known face:
    //   yaw 0,    pitch 0     -> -Z (face 5, cyan)
    //   yaw pi/2, pitch 0     -> -X (face 1, green)
    //   yaw 0,    pitch pi/2  -> +Y (face 2, blue)
    {
        const glm::vec4 faceCols[6] = {
            {1,0,0,1}, {0,1,0,1}, {0,0,1,1}, {1,1,0,1}, {1,0,1,1}, {0,1,1,1}   // +X,-X,+Y,-Y,+Z,-Z
        };
        auto centre = [&](float yaw, float pitch, int& r, int& g, int& b) -> bool {
            Graph gr;
            auto sky = std::make_unique<SkyboxNode>();
            sky->inputDefault(7) = Transform{ yaw, pitch, true };   // port 7 = transform (yaw, pitch, active)
            sky->initGL();
            int skId = gr.addNode(std::move(sky));
            for (int i = 0; i < 6; ++i) {
                auto c = std::make_unique<ColourNode>();
                c->inputDefault(0) = faceCols[i];
                c->initGL();
                int cid = gr.addNode(std::move(c));
                if (!gr.connect(cid, 0, skId, i)) return false;
            }
            auto out = std::make_unique<OutputNode>();
            out->initGL();
            int oId = gr.addNode(std::move(out));
            if (!gr.connect(skId, 0, oId, 0)) return false;
            gr.evaluate(1.0f/60.0f);
            TexRef t = dynamic_cast<OutputNode*>(gr.findNode(oId))->current();
            if (!t.id) return false;
            int a; readCentre(t, r, g, b, a);
            return true;
        };
        const float HALF_PI = 1.57079633f;
        int r, g, b;
        if (!centre(0.0f, 0.0f, r, g, b)    || !(near(r,0) && near(g,255) && near(b,255))) { glfwTerminate(); return fail("skybox centre yaw0/pitch0 not -Z (cyan)"); }
        if (!centre(HALF_PI, 0.0f, r, g, b) || !(near(r,0) && near(g,255) && near(b,0)))   { glfwTerminate(); return fail("skybox yaw pi/2 not -X (green)"); }
        if (!centre(0.0f, HALF_PI, r, g, b) || !(near(r,0) && near(g,0) && near(b,255)))   { glfwTerminate(); return fail("skybox pitch pi/2 not +Y (blue)"); }
        if (!centre(0.0f,-HALF_PI, r, g, b) || !(near(r,255) && near(g,255) && near(b,0))) { glfwTerminate(); return fail("skybox pitch -pi/2 not -Y (yellow)"); }
        if (!centre(3.14159265f,0.0f, r,g,b)|| !(near(r,255) && near(g,0) && near(b,255))) { glfwTerminate(); return fail("skybox yaw pi not +Z (magenta)"); }
        std::fprintf(stderr, "gl_smoke OK: Skybox samples all 6 faces with yaw/pitch rotation\n");
    }

    // --- Scenario: Deform runs a vertex shader over a VBO via transform feedback ---
    // A 1-vertex input VBO (Pos3) + a known preset shader -> Deform; read the transform-
    // feedback output (Pos3Color3, 6 floats) back and verify the GPU transform exactly.
    {
        const float inPos[3] = { 0.2f, 0.3f, 0.4f };
        GLuint inVbo = 0;
        glGenBuffers(1, &inVbo); glBindBuffer(GL_ARRAY_BUFFER, inVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(inPos), inPos, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        auto runDeform = [&](int preset, float pos, glm::vec4 col, float o[6]) -> bool {
            Graph g;
            auto def = std::make_unique<DeformNode>();
            def->inputDefault(0) = VertexRef{ inVbo, 1, Primitive::Lines, VertexFormat::Pos3 };
            def->inputDefault(1) = pos;
            def->inputDefault(2) = col;
            def->inputDefault(3) = ShaderRef{ vertexShaderSource(preset) };
            def->initGL();
            int dId = g.addNode(std::move(def));
            g.evaluate(1.0f / 60.0f);
            VertexRef out = dynamic_cast<DeformNode*>(g.findNode(dId))->output();
            if (out.vbo == 0 || out.count != 1 || out.format != VertexFormat::Pos3Color3) return false;
            glBindBuffer(GL_ARRAY_BUFFER, out.vbo);
            glGetBufferSubData(GL_ARRAY_BUFFER, 0, 6 * sizeof(float), o);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            return true;
        };
        auto af = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };

        float o[6];
        // Identity: vPosition = aPos; vColor = aColor(0) + uColour.rgb = colour.
        if (!runDeform(0, 0.5f, glm::vec4(0.6f, 0.7f, 0.8f, 1.0f), o)) { glfwTerminate(); return fail("Deform identity produced no output"); }
        if (!(af(o[0],0.2f) && af(o[1],0.3f) && af(o[2],0.4f) && af(o[3],0.6f) && af(o[4],0.7f) && af(o[5],0.8f))) {
            std::fprintf(stderr, "Deform identity got (%.3f,%.3f,%.3f, %.3f,%.3f,%.3f)\n", o[0],o[1],o[2],o[3],o[4],o[5]);
            glDeleteBuffers(1, &inVbo); glfwTerminate(); return fail("Deform identity transform wrong");
        }
        // Wave: y += uPos*sin(x*2pi); x=0.2, uPos=0.5 -> y = 0.3 + 0.5*sin(0.2*2pi); x,z unchanged.
        if (!runDeform(2, 0.5f, glm::vec4(0, 0, 0, 1), o)) { glfwTerminate(); return fail("Deform wave produced no output"); }
        float ey = 0.3f + 0.5f * std::sin(0.2f * 6.2831853f);
        if (!(af(o[0],0.2f) && af(o[1],ey) && af(o[2],0.4f))) {
            std::fprintf(stderr, "Deform wave got y=%.4f expected %.4f\n", o[1], ey);
            glDeleteBuffers(1, &inVbo); glfwTerminate(); return fail("Deform wave transform wrong");
        }
        glDeleteBuffers(1, &inVbo);

        // The new Shader edge wires end to end (VertexShader -> Deform.shader).
        {
            Graph g;
            auto vs = std::make_unique<VertexShaderNode>();
            auto def = std::make_unique<DeformNode>();
            def->initGL();
            int vsId = g.addNode(std::move(vs));
            int dId  = g.addNode(std::move(def));
            if (!g.connect(vsId, 0, dId, 3)) { glfwTerminate(); return fail("Shader edge VertexShader->Deform did not connect"); }
        }
        std::fprintf(stderr, "gl_smoke OK: Deform applies a vertex shader via transform feedback\n");
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
