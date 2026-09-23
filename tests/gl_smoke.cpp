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
#include "core/ProjectFile.h"
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
#include "modules/VertexTrailNode.h"
#include "core/ColorHsv.h"
#include "modules/VertexShaderNode.h"
#include "modules/DrumMachineNode.h"
#include "modules/MidiFilePlayerNode.h"
#include "core/VertexShaders.h"
#include "gfx/VideoDecoder.h"
#include "gfx/VideoEncoder.h"
#include "gfx/TextGeometry.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "modules/ImageStreamerNode.h"
#include "modules/ImageSequencerNode.h"
#include "modules/KaleidoscopeNode.h"
#include "modules/HsvAdjustNode.h"
#include "gfx/GLStateGuard.h"
#include "gfx/GLUtil.h"
#include "modules/ProjectMNode.h"
#include "modules/AudioOutputNode.h"
#include "core/OfflineRender.h"
#include "app/OfflineRenderer.h"
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <limits>
#include <chrono>
#include <cmath>
#include <thread>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

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

// Read the texel nearest UV (u,v in [0,1]); vertical orientation doesn't matter for
// horizontal (u) checks. Used by the image scenarios.
static void readAtUV(TexRef tex, float u, float v, int& r, int& g, int& b, int& a) {
    std::vector<unsigned char> px((size_t)tex.w * tex.h * 4);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    int x = (int)(u * (tex.w - 1));
    int y = (int)(v * (tex.h - 1));
    size_t i = ((size_t)y * tex.w + x) * 4;
    r = px[i]; g = px[i+1]; b = px[i+2]; a = px[i+3];
}

// A minimal Milkdrop preset authored for these tests (no third-party preset ships in the repo).
// No warp/zoom/rotation, so a burned image stays where it was stamped; the orange outer border
// guarantees non-black pixels even with silent audio.
static const char* kTestPreset =
    "[preset00]\n"
    "fDecay=0.98\nzoom=1.0\nrot=0.0\nwarp=0.0\n"
    "nWaveMode=0\nfWaveAlpha=1.0\nfWaveScale=1.0\n"
    "wave_r=1.0\nwave_g=0.4\nwave_b=0.1\n"
    "ob_size=0.04\nob_r=0.9\nob_g=0.4\nob_b=0.1\nob_a=1.0\n";

// Write a.milk / b.milk / c.milk into a fresh temp folder; returns the folder ("" on failure).
static std::string writePresetFolder() {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "oss_projectm_smoke";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (!std::filesystem::create_directories(dir, ec)) return "";
    for (const char* n : {"a.milk", "b.milk", "c.milk"}) {
        std::ofstream f(dir / n);
        if (!f) return "";
        f << kTestPreset;
    }
    return dir.string();
}

// A Milkdrop-2 preset (both MILKDROP_PRESET_VERSION >= 200 and PSVERSION* are required, or projectM
// silently uses its one-sampler default shaders) whose composite shader samples five textures, which
// is what makes projectM bind samplers on units 1..4. Authored for these tests; no third-party
// preset ships in the repo.
static const char* kMultiSamplerPreset =
    "[preset00]\n"
    "MILKDROP_PRESET_VERSION=201\nPSVERSION=2\nPSVERSION_WARP=2\nPSVERSION_COMP=2\n"
    "fDecay=0.98\nwarp=0.100000\nwave_a=0\n"
    "nMotionVectorsX=16\nnMotionVectorsY=12\nmv_a=1\nbDarkenCenter=1\nbBrighten=1\n"
    "comp_1=`shader_body\n"
    "comp_2=`{\n"
    "comp_3=`float3 a = tex2D(sampler_main, uv).xyz;\n"
    "comp_4=`float3 b = tex2D(sampler_blur1, uv).xyz;\n"
    "comp_5=`float3 c = tex2D(sampler_fw_noise_lq, uv).xyz;\n"
    "comp_6=`float3 d = tex3D(sampler_fw_noisevol_lq, float3(uv, 0.5)).xyz;\n"
    "comp_7=`float3 e = tex2D(sampler_blur2, uv).xyz;\n"
    "comp_8=`ret = a*0.4 + b*0.2 + c*0.2 + d*0.1 + e*0.1;\n"
    "comp_9=`}\n"
    "warp_1=`shader_body\n"
    "warp_2=`{\n"
    "warp_3=`ret = tex2D(sampler_main, uv).xyz * 0.9 + tex2D(sampler_pc_noise_lq, uv).xyz * 0.1;\n"
    "warp_4=`}\n";

// Write the multi-sampler preset into its OWN temp folder (so the a/b/c folder stays 3 files for
// the "(2/3)" status check); returns the file path ("" on failure).
static std::string writeMultiSamplerPreset() {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "oss_projectm_smoke_multi";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (!std::filesystem::create_directories(dir, ec)) return "";
    std::ofstream f(dir / "multi.milk");
    if (!f) return "";
    f << kMultiSamplerPreset;
    return (dir / "multi.milk").string();
}

// The node's input defaults as a resolved input vector (what Graph::evaluate would hand it).
static std::vector<Value> defaultInputs(const Node& n) {
    std::vector<Value> v;
    for (const Port& p : n.inputs()) v.push_back(p.defaultValue);
    return v;
}

// Write a 64x64 PNG: left half red, right half green. Returns the path (or "" on failure).
static std::string writeSplitFixture() {
    const int W = 64, H = 64;
    std::vector<unsigned char> px((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = ((size_t)y * W + x) * 4;
            bool left = x < W / 2;
            px[i+0] = left ? 255 : 0;
            px[i+1] = left ? 0   : 255;
            px[i+2] = 0;
            px[i+3] = 255;
        }
    std::string path = "gl_smoke_image_fixture.png";
    if (!stbi_write_png(path.c_str(), W, H, 4, px.data(), W * 4)) return std::string();
    return path;
}

// A deliberately mis-sized audio source for the offline capture's pad/trim contract: it ignores
// dt and always publishes `frames` samples at 48 kHz. Every real source sizes its block with
// audioBlockFrames(), which at 48 kHz divides exactly by all five supported frame rates -- so
// nothing in a normal graph exercises the capture's resize to audioSamplesPerFrame(), and a
// dropped pad/trim would silently drift the audio clock away from the video clock.
class MisSizedAudioNode : public Node {
public:
    explicit MisSizedAudioNode(int frames) : Node("MisSizedAudio"), buf_((std::size_t)frames) {
        for (std::size_t i = 0; i < buf_.size(); ++i)
            buf_[i] = 0.5f * (float)std::sin(6.283185307179586 * 440.0 * (double)i / 48000.0);
        addOutput("audio", PortType::Audio);
    }
    void evaluate(EvalContext& ctx) override {
        ctx.out<AudioRef>(0, AudioRef{buf_.data(), buf_.size(), 48000});
    }
private:
    std::vector<float> buf_;
};

// Records the clock it was evaluated with, so the offline render's central claim -- that frame k
// sits at exactly startBar*secondsPerBar + k/fps on a fixed 1/fps dt -- is checked rather than
// assumed. Wired into nothing: Graph::topologicalOrder seeds every zero-indegree node, so a
// disconnected probe still runs once per evaluate.
class ClockProbeNode : public Node {
public:
    ClockProbeNode() : Node("ClockProbe") { addOutput("f", PortType::Float); }
    void evaluate(EvalContext& ctx) override {
        secs.push_back(ctx.transport ? ctx.transport->seconds : -1.0);
        dts.push_back(ctx.dt);
        ext.push_back(ctx.transport && ctx.transport->externalClock && ctx.transport->playing);
        ctx.out<float>(0, 0.0f);
    }
    std::vector<double> secs; std::vector<float> dts; std::vector<char> ext;
};

// Write a deterministic high-entropy PNG: frames built from it barely compress, so an encoder
// writing them overflows AVIOContext's ~32 KiB buffer and actually reaches the file mid-stream.
static bool writeNoisePNG(const char* path, int W, int H) {
    std::vector<unsigned char> px((std::size_t)W * H * 4);
    std::uint32_t seed = 12345u;                                  // fixed seed: reproducible
    for (std::size_t i = 0; i < px.size(); i += 4) {
        seed = seed * 1664525u + 1013904223u;
        px[i] = (unsigned char)(seed >> 24); px[i+1] = (unsigned char)(seed >> 16);
        px[i+2] = (unsigned char)(seed >> 8); px[i+3] = 255;
    }
    return stbi_write_png(path, W, H, 4, px.data(), W * 4) != 0;
}

// Write a 16x16 solid-colour PNG at `path`. Returns true on success.
static bool writeSolidPNG(const std::string& path, unsigned char r, unsigned char g, unsigned char b) {
    const int W = 16, H = 16;
    std::vector<unsigned char> px((size_t)W * H * 4);
    for (size_t i = 0; i < px.size(); i += 4) { px[i]=r; px[i+1]=g; px[i+2]=b; px[i+3]=255; }
    return stbi_write_png(path.c_str(), W, H, 4, px.data(), W * 4) != 0;
}

int main() {
    // Once at startup, like the app's own main(): VideoEncoder::open() no longer does it (it is
    // process-wide, so it used to silence the decoders too), and without this the ~20 lines of
    // libx264/aac statistics per encoder open bury the scenario log.
    quietFFmpegLog();

    // Phase 2: the six media file inputs are asset-backed with the matching AssetType.
    // Pure CPU (node constructors don't touch GL), so it runs before any GL setup --
    // a bare `return fail(...)` is correct here (no context to clean up).
    {
        auto bad = [](const Node& n, int port, AssetType want) {
            if (port < 0 || port >= (int)n.inputs().size()) return true;
            const Port& p = n.inputs()[(std::size_t)port];
            return p.type != PortType::String || !p.assetBacked || p.assetType != want;
        };
        AudioPlayerNode    ap; if (bad(ap, 0, AssetType::Audio)) return fail("AudioPlayer.file not asset-backed Audio");
        VideoPlayerNode    vp; if (bad(vp, 0, AssetType::Video)) return fail("VideoPlayer.file not asset-backed Video");
        MeshLoaderNode     ml; if (bad(ml, 0, AssetType::Mesh))  return fail("MeshLoader.file not asset-backed Mesh");
        MidiFilePlayerNode mf; if (bad(mf, 0, AssetType::Midi))  return fail("MidiFile.file not asset-backed Midi");
        DrumMachineNode    dm;
        for (int v = 0; v < DrumMachineNode::kVoices; ++v)
            if (bad(dm, 4 * v, AssetType::Audio)) return fail("DrumMachine.file voice not asset-backed Audio");
        ProjectMNode       pmn; if (bad(pmn, ProjectMNode::kPreset, AssetType::Preset)) return fail("projectM.preset not asset-backed Preset");
        std::fprintf(stderr, "gl_smoke OK: 6 media nodes expose asset-backed file inputs\n");
    }

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

    // --- Scenario: Image Streamer loads a split image ---
    {
        std::string fixture = writeSplitFixture();
        if (fixture.empty()) { glfwTerminate(); return fail("write image fixture"); }

        Graph g;
        auto img = std::make_unique<ImageStreamerNode>();
        auto out = std::make_unique<OutputNode>();
        img->initGL(); out->initGL();
        img->inputDefault(0) = Value(fixture);          // set the "file" path
        int iId = g.addNode(std::move(img));
        int oId = g.addNode(std::move(out));
        if (!g.connect(iId, 0, oId, 0)) { std::remove(fixture.c_str()); glfwTerminate(); return fail("connect ImageStreamer->Output"); }

        g.evaluate(1.0f / 60.0f);
        TexRef tex = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
        std::remove(fixture.c_str());
        if (tex.id == 0 || tex.w <= 0 || tex.h <= 0) { glfwTerminate(); return fail("image output texture not produced"); }

        int r, gg, b, a;
        readAtUV(tex, 0.25f, 0.5f, r, gg, b, a);   // left quarter -> red
        if (!(r > 200 && gg < 60)) { glfwTerminate(); return fail("image left half not red"); }
        readAtUV(tex, 0.75f, 0.5f, r, gg, b, a);   // right quarter -> green
        if (!(gg > 200 && r < 60)) { glfwTerminate(); return fail("image right half not green"); }
        std::fprintf(stderr, "gl_smoke OK: Image Streamer loaded a split image\n");
    }

    // --- Scenario: Kaleidoscope folds an image (segments=4) ---
    {
        std::string fixture = writeSplitFixture();   // left red, right green
        if (fixture.empty()) { glfwTerminate(); return fail("write kaleidoscope fixture"); }

        Graph g;
        auto img = std::make_unique<ImageStreamerNode>();
        auto kal = std::make_unique<KaleidoscopeNode>();
        auto out = std::make_unique<OutputNode>();
        img->initGL(); kal->initGL(); out->initGL();
        img->inputDefault(0) = Value(fixture);
        kal->inputDefault(1) = Value(4.0f);   // segments = 4 -> 90-degree rotational symmetry
        int iId = g.addNode(std::move(img));
        int kId = g.addNode(std::move(kal));
        int oId = g.addNode(std::move(out));
        bool wired = g.connect(iId, 0, kId, 0) && g.connect(kId, 0, oId, 0);
        if (!wired) { std::remove(fixture.c_str()); glfwTerminate(); return fail("wire ImageStreamer->Kaleidoscope->Output"); }

        g.evaluate(1.0f / 60.0f);
        TexRef tex = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
        std::remove(fixture.c_str());
        if (tex.id == 0) { glfwTerminate(); return fail("kaleidoscope output not produced"); }

        // (1) 4-fold rotational symmetry: UV (0.7,0.5) [angle 0] == UV (0.5,0.7) [angle 90deg].
        int r0, g0, b0, a0, r1, g1, b1, a1;
        readAtUV(tex, 0.7f, 0.5f, r0, g0, b0, a0);
        readAtUV(tex, 0.5f, 0.7f, r1, g1, b1, a1);
        if (!(near(r0, r1) && near(g0, g1) && near(b0, b1)))
            { glfwTerminate(); return fail("kaleidoscope not 4-fold rotationally symmetric"); }

        // (2) It actually folds: a point on the (red) left half samples the (green) right
        // half after folding. Raw input at UV (0.3,0.5) is red; kaleidoscope output is green.
        int r2, g2, b2, a2;
        readAtUV(tex, 0.3f, 0.5f, r2, g2, b2, a2);
        if (!(g2 > 200 && r2 < 60)) { glfwTerminate(); return fail("kaleidoscope did not fold the left half"); }
        std::fprintf(stderr, "gl_smoke OK: Kaleidoscope folds (symmetric + wedge-folded)\n");
    }

    // --- Scenario: Image Sequencer cycles a folder (async prefetch, split inputs) ---
    {
        namespace fs = std::filesystem;
        fs::path dir = fs::temp_directory_path() / "oss_imgseq_smoke";
        fs::remove_all(dir);
        fs::create_directories(dir);
        bool wrote = writeSolidPNG((dir / "0.png").string(), 255, 0, 0)     // red
                  && writeSolidPNG((dir / "1.png").string(), 0, 255, 0)     // green
                  && writeSolidPNG((dir / "2.png").string(), 0, 0, 255);    // blue
        if (!wrote) { fs::remove_all(dir); glfwTerminate(); return fail("write sequencer fixtures"); }

        // Port-flag check (pure CPU; the ctor doesn't touch GL). folder=0, duration=1, beat length=2, sync=3.
        { ImageSequencerNode probe;
          const Port& pf = probe.inputs()[0];
          if (!(pf.type == PortType::String && pf.assetBacked && pf.folderPicker && pf.assetType == AssetType::Image))
            { fs::remove_all(dir); glfwTerminate(); return fail("Sequencer.folder not a folder picker"); }
          if (probe.inputs().size() != 5 || !probe.inputs()[2].integer)
            { fs::remove_all(dir); glfwTerminate(); return fail("Sequencer 'beat length' not an int input at port 2"); } }

        Graph g;
        auto seq = std::make_unique<ImageSequencerNode>();
        auto out = std::make_unique<OutputNode>();
        seq->initGL(); out->initGL();
        seq->inputDefault(0) = Value(dir.string());   // folder
        seq->inputDefault(1) = Value(1.0f);           // duration = 1s (free-running)
        seq->inputDefault(2) = Value(1.0f);           // beat length = 1
        seq->inputDefault(3) = Value(false);          // sync off
        int sId = g.addNode(std::move(seq));
        int oId = g.addNode(std::move(out));
        if (!g.connect(sId, 0, oId, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("connect Sequencer->Output"); }

        auto centreIs = [&](int R, int G, int B)->bool {
            TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
            if (t.id == 0) return false;
            int r, gg, b, a; readCentre(t, r, gg, b, a);
            return near(r, R) && near(gg, G) && near(b, B);
        };
        // Evaluate a big-dt frame to advance the counter, then poll small-dt frames until the
        // async image for the new index is decoded + uploaded (or time out).
        auto advanceUntil = [&](int R, int G, int B)->bool {
            g.evaluate(1.1f);                                  // cross one `duration` boundary
            for (int f = 0; f < 400; ++f) {
                if (centreIs(R, G, B)) return true;
                g.evaluate(0.001f);                            // poll without advancing further
            }
            return false;
        };

        g.evaluate(1.0f / 60.0f);                              // image 0 loads synchronously -> red
        if (!centreIs(255, 0, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer frame 0 not red"); }
        if (!advanceUntil(0, 255, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer did not reach green"); }
        if (!advanceUntil(0, 0, 255)) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer did not reach blue"); }
        if (!advanceUntil(255, 0, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer did not wrap to red"); }

        // Synced mode: index derives from transport beats (120 bpm -> 0.5 s/beat), beat length = 1.
        g.findNode(sId)->inputDefault(3) = Value(true);        // sync on
        g.findNode(sId)->inputDefault(2) = Value(1.0f);        // beat length = 1
        g.transport().seconds = 1.0;                           // beats = 2.0 -> image 2 (blue)
        bool syncedBlue = false;
        for (int f = 0; f < 400 && !syncedBlue; ++f) { g.evaluate(0.001f); syncedBlue = centreIs(0, 0, 255); }
        if (!syncedBlue) { fs::remove_all(dir); glfwTerminate(); return fail("sequencer sync beats=2 not blue"); }

        fs::remove_all(dir);
        std::fprintf(stderr, "gl_smoke OK: Image Sequencer cycled a folder (async prefetch, free-run + sync)\n");
    }

    // --- Scenario: Image Sequencer cross-fades between images ---
    {
        namespace fs = std::filesystem;
        fs::path dir = fs::temp_directory_path() / "oss_imgseq_fade";
        fs::remove_all(dir);
        fs::create_directories(dir);
        bool wrote = writeSolidPNG((dir / "0.png").string(), 255, 0, 0)     // red
                  && writeSolidPNG((dir / "1.png").string(), 0, 255, 0);    // green
        if (!wrote) { fs::remove_all(dir); glfwTerminate(); return fail("write fade fixtures"); }

        Graph g;
        auto seq = std::make_unique<ImageSequencerNode>();
        auto out = std::make_unique<OutputNode>();
        seq->initGL(); out->initGL();
        seq->inputDefault(0) = Value(dir.string());   // folder
        seq->inputDefault(1) = Value(1.0f);           // duration = 1s
        seq->inputDefault(2) = Value(1.0f);           // beat length
        seq->inputDefault(3) = Value(false);          // sync off
        seq->inputDefault(4) = Value(0.5f);           // fade duration = 0.5s
        int sId = g.addNode(std::move(seq));
        int oId = g.addNode(std::move(out));
        if (!g.connect(sId, 0, oId, 0)) { fs::remove_all(dir); glfwTerminate(); return fail("connect fade Sequencer->Output"); }

        // Step in 20 ms frames: red is shown, image 1 (green) prefetches, then at ~1 s the fade
        // starts and mix(red, green, m) is on the output for ~0.5 s before it resolves to green.
        bool sawBlend = false, reachedGreen = false;
        for (int f = 0; f < 2000 && !reachedGreen; ++f) {
            g.evaluate(0.02f);
            TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
            if (t.id == 0) continue;
            int r, gg, b, a; readCentre(t, r, gg, b, a);
            if (r > 60 && gg > 60 && b < 60) sawBlend = true;   // a red+green blend (mid-fade)
            if (gg > 200 && r < 60)          reachedGreen = true;  // resolved to the incoming image
        }
        fs::remove_all(dir);
        if (!sawBlend)     { glfwTerminate(); return fail("cross-fade produced no red/green blend"); }
        if (!reachedGreen) { glfwTerminate(); return fail("cross-fade did not resolve to green"); }
        std::fprintf(stderr, "gl_smoke OK: Image Sequencer cross-fade blends red->green\n");
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

        // Every frame handed to the encoder must come back out of the file, at every length.
        // The mp4 muxer infers each sample's duration from the NEXT packet's dts, so the LAST
        // sample's duration is whatever the encoder put in pkt->duration -- and libx264 leaves it
        // at 0. The track then measures one frame short, and the half-open edit list written from
        // that length trims the final frame off again on read-back. It only SHOWS when the
        // muxer's millisecond rounding of that length happens to be exact: at 30 fps that is
        // every third length (3 frames = 100 ms), but at 25 fps one frame is exactly 40 ms, so
        // EVERY length lost its last frame. Both rates are checked for that reason.
        for (int fps : {25, 30}) {
            for (int n : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 30, 60}) {
                std::string p = "build/_rec_len" + std::to_string(fps) + "_" + std::to_string(n) + ".mp4";
                {
                    VideoEncoder e2; std::string er;
                    if (!e2.open(p, W, H, fps, 0, 0, er)) { glfwTerminate(); return fail(("length encoder open: " + er).c_str()); }
                    std::vector<unsigned char> fr((size_t)W * H * 4, 0);
                    for (int f = 0; f < n; ++f) {
                        for (size_t i = 0; i < fr.size(); i += 4) {
                            fr[i] = (unsigned char)(f * 7); fr[i+1] = 60; fr[i+2] = 120; fr[i+3] = 255;
                        }
                        if (!e2.addVideoFrame(fr.data(), f / (double)fps)) { glfwTerminate(); return fail("length encode: addVideoFrame failed"); }
                    }
                    std::string ec;
                    if (!e2.close(ec)) { glfwTerminate(); return fail(("length encode close: " + ec).c_str()); }
                }
                VideoDecoder d2; std::string de;
                if (!d2.open(p, de)) { glfwTerminate(); return fail(("length decode open: " + de).c_str()); }
                VideoFrame v2; std::vector<float> a2; double s2 = 0; bool b2 = false; int back = 0;
                while (d2.decodeFrame(v2, a2, s2, b2)) { ++back; a2.clear(); }
                std::remove(p.c_str());
                if (back != n) {
                    char msg[160];
                    std::snprintf(msg, sizeof(msg),
                                  "encoded %d frames at %d fps but decoded %d -- the encoder is losing frames", n, fps, back);
                    glfwTerminate(); return fail(msg);
                }
            }
        }
        std::fprintf(stderr, "gl_smoke OK: VideoEncoder round-trips every length (1..12, 30, 60) at 25 and 30 fps with no lost frames\n");

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
        recN->inputDefault(4) = std::string("build/_rec_node.mp4");   // file
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

        rn->inputDefault(3) = true;                          // record on
        for (int f = 0; f < 12; ++f) g.evaluate(1.0f / 60.0f);
        rn->inputDefault(3) = false;                         // record off -> finalise file
        g.evaluate(1.0f / 60.0f);

        VideoDecoder dec2;
        std::string err2;
        if (!dec2.open("build/_rec_node.mp4", err2)) { glfwTerminate(); return fail(("recorded file did not open: " + err2).c_str()); }
        if (dec2.width() != vw || dec2.height() != vh) { glfwTerminate(); return fail("recorded video has wrong dimensions"); }
        VideoFrame vf2; std::vector<float> au2; double s2 = 0; bool v2 = false; int got2 = 0;
        while (got2 < 3 && dec2.decodeFrame(vf2, au2, s2, v2)) ++got2;
        if (got2 == 0) { glfwTerminate(); return fail("recorded file decoded no frames"); }
        std::fprintf(stderr, "gl_smoke OK: Recorder passed video through and wrote a decodable %dx%d mp4\n", vw, vh);

        // (c) Stereo end-to-end: two sines panned through the Mixer feed the
        // Recorder's L/R audio; the recorded file must be a 2-channel movie.
        {
            Graph gs;
            auto col2 = std::make_unique<ColourNode>();
            auto s1 = std::make_unique<SineWaveNode>(); s1->inputDefault(0) = 300.0f;
            auto s2 = std::make_unique<SineWaveNode>(); s2->inputDefault(0) = 600.0f;
            auto mix = std::make_unique<AudioMixerNode>();
            mix->inputDefault(2) = -1.0f;   // pan 1 -> hard left
            mix->inputDefault(5) =  1.0f;   // pan 2 -> hard right
            auto rec2 = std::make_unique<RecorderNode>();
            rec2->inputDefault(4) = std::string("build/_rec_stereo_node.mp4");
            col2->initGL(); s1->initGL(); s2->initGL(); mix->initGL(); rec2->initGL();
            int cId2 = gs.addNode(std::move(col2));
            int s1Id = gs.addNode(std::move(s1));
            int s2Id = gs.addNode(std::move(s2));
            int mId  = gs.addNode(std::move(mix));
            int r2Id = gs.addNode(std::move(rec2));
            if (!gs.connect(s1Id, 0, mId, 0) || !gs.connect(s2Id, 0, mId, 3) ||
                !gs.connect(cId2, 0, r2Id, 0) || !gs.connect(mId, 0, r2Id, 1) || !gs.connect(mId, 1, r2Id, 2)) {
                glfwTerminate(); return fail("connect stereo record graph");
            }
            auto* rn2 = dynamic_cast<RecorderNode*>(gs.findNode(r2Id));
            rn2->inputDefault(3) = true;                          // record on
            for (int f = 0; f < 16; ++f) gs.evaluate(1.0f / 60.0f);
            rn2->inputDefault(3) = false;                        // record off -> finalise
            gs.evaluate(1.0f / 60.0f);

            VideoDecoder d3; std::string e3;
            if (!d3.open("build/_rec_stereo_node.mp4", e3)) { glfwTerminate(); return fail(("stereo recording did not open: " + e3).c_str()); }
            if (!d3.hasAudio() || d3.audioChannels() != 2) { glfwTerminate(); return fail("graph recording should be stereo (2 channels)"); }
            VideoFrame vf3; std::vector<float> au3; double s3 = 0; bool v3 = false; int got3 = 0;
            while (got3 < 3 && d3.decodeFrame(vf3, au3, s3, v3)) ++got3;
            if (got3 == 0) { glfwTerminate(); return fail("stereo recording decoded no frames"); }
            std::fprintf(stderr, "gl_smoke OK: Sine->Mixer(pan)->Recorder wrote a stereo movie\n");
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

    // --- Scenario: Audio File `auto play` follows the transport ---
    // With `auto play` on, the transport's play state drives the clip and the `play` input is
    // ignored. Pause holds the position; Stop zeroes the transport, which the node sees as a
    // BACKWARDS move and rewinds the clip, so the next Play starts the file from the beginning.
    // A forward scrub is deliberately NOT followed -- strict position locking is what `sync` is
    // for, and auto play is about free-running playback the transport starts and stops.
    {
        Graph g;
        auto ap = std::make_unique<AudioPlayerNode>();
        ap->inputDefault(0) = std::string("tests/assets/tone.mp3");
        ap->inputDefault(3) = false;                 // loop off, so the playhead is monotonic
        ap->inputDefault(6) = true;                  // auto play on
        int aId = g.addNode(std::move(ap));
        auto* an = dynamic_cast<AudioPlayerNode*>(g.findNode(aId));
        g.transport().bpm = 120.0;
        g.transport().playing = false;
        g.transport().seconds = 0.0;

        auto pump = [&](int n) { for (int i = 0; i < n; ++i) g.evaluate(1.0f / 60.0f); };
        // The decode STARTS inside evaluate() (loader_.request), so loading() is false until one
        // frame has run -- drive a frame first, then wait, then drive one more so poll() adopts it.
        pump(1);
        for (int f = 0; f < 500 && an->loading(); ++f) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            pump(1);
        }
        pump(1);
        if (an->loading()) { glfwTerminate(); return fail("auto play: clip never finished loading"); }

        auto blockSilent = [&]() {
            AudioRef l = an->leftOut();
            if (l.count == 0) return true;
            for (std::size_t i = 0; i < l.count; ++i) if (std::fabs(l.samples[i]) > 0.01f) return false;
            return true;
        };

        // (1) Transport stopped -> silent, and the playhead does not move.
        pump(5);
        if (!blockSilent()) { glfwTerminate(); return fail("auto play: clip sounded while the transport was stopped"); }
        if (an->playhead() != 0.0) { glfwTerminate(); return fail("auto play: playhead advanced while the transport was stopped"); }

        // (2) Transport playing -> audible, and the playhead advances. (Graph::evaluate advances
        // the transport itself, so nothing here moves `seconds` by hand.)
        g.transport().playing = true;
        bool heard = false;
        for (int f = 0; f < 20 && !heard; ++f) { pump(1); if (!blockSilent()) heard = true; }
        if (!heard) { glfwTerminate(); return fail("auto play: clip stayed silent while the transport played"); }
        double playedTo = an->playhead();
        if (!(playedTo > 0.0)) { glfwTerminate(); return fail("auto play: playhead did not advance while the transport played"); }

        // (3) Pause -> position holds (the transport stops moving; it does not move backwards).
        g.transport().playing = false;
        pump(5);
        if (an->playhead() != playedTo) { glfwTerminate(); return fail("auto play: a pause did not hold the playhead"); }
        if (!blockSilent()) { glfwTerminate(); return fail("auto play: clip sounded while paused"); }

        // (4) Stop -> playing false AND seconds 0, a backwards move, so the clip rewinds.
        g.transport().stop();
        pump(1);
        if (an->playhead() != 0.0) { glfwTerminate(); return fail("auto play: a stop did not rewind the clip"); }

        // (5) auto play OFF -> the `play` toggle governs again, transport still stopped.
        an->inputDefault(6) = false;
        an->inputDefault(2) = true;
        bool heardManual = false;
        for (int f = 0; f < 20 && !heardManual; ++f) { pump(1); if (!blockSilent()) heardManual = true; }
        if (!heardManual) { glfwTerminate(); return fail("auto play off: the play toggle should still work with the transport stopped"); }

        // (6) A FORWARD scrub is not followed: the clip keeps playing at its own rate.
        an->inputDefault(6) = true;
        g.transport().playing = true;
        pump(3);
        double beforeScrub = an->playhead();
        g.transport().seconds += 5.0;
        pump(1);
        if (an->playhead() < beforeScrub) { glfwTerminate(); return fail("auto play: a forward scrub must not rewind the clip"); }

        std::fprintf(stderr, "gl_smoke OK: Audio File auto play follows the transport (stop rewinds, pause holds, forward scrub ignored)\n");
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

    // --- Scenario: HSV Adjust shader matches the adjustHsv reference ---
    // Feed a solid colour through the node and assert the rendered centre pixel matches
    // adjustHsv() computed on the 8-bit-quantised input (what the texture carries). near() +/-3.
    {
        auto quant = [](glm::vec3 c) {
            return glm::vec3(std::round(c.x*255.0f)/255.0f,
                             std::round(c.y*255.0f)/255.0f,
                             std::round(c.z*255.0f)/255.0f);
        };
        auto check = [&](glm::vec3 in, float hue, float sat, float bright) -> bool {
            Graph g;
            auto col = std::make_unique<ColourNode>(); col->inputDefault(0) = glm::vec4(in, 1.0f);
            auto adj = std::make_unique<HsvAdjustNode>();
            adj->inputDefault(1) = hue; adj->inputDefault(2) = sat; adj->inputDefault(3) = bright;
            auto out = std::make_unique<OutputNode>();
            col->initGL(); adj->initGL(); out->initGL();
            int cId = g.addNode(std::move(col));
            int aId = g.addNode(std::move(adj));
            int oId = g.addNode(std::move(out));
            if (!g.connect(cId,0,aId,0) || !g.connect(aId,0,oId,0)) return false;
            g.evaluate(1.0f/60.0f);
            TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
            if (!t.id) return false;
            int r, gg, bb, aa; readCentre(t, r, gg, bb, aa);
            glm::vec3 e = adjustHsv(quant(in), hue, sat, bright);
            int er = (int)std::lround(e.x*255.0f), eg = (int)std::lround(e.y*255.0f), eb = (int)std::lround(e.z*255.0f);
            std::fprintf(stderr, "gl_smoke hsv (h%.2f s%.2f v%.2f): got (%d,%d,%d) expected (%d,%d,%d)\n",
                         hue, sat, bright, r, gg, bb, er, eg, eb);
            return near(r,er) && near(gg,eg) && near(bb,eb);
        };
        if (!check(glm::vec3(1.0f,0.0f,0.0f),   1.0f/3.0f, 1.0f, 1.0f)) { glfwTerminate(); return fail("HSV Adjust hue-shift (red->green) mismatch"); }
        if (!check(glm::vec3(0.2f,0.5f,0.8f),   0.1f,      1.0f, 0.8f)) { glfwTerminate(); return fail("HSV Adjust hue+brightness mismatch"); }
        if (!check(glm::vec3(0.6f,0.3f,0.9f),   0.0f,      0.0f, 1.0f)) { glfwTerminate(); return fail("HSV Adjust desaturate mismatch"); }
        std::fprintf(stderr, "gl_smoke OK: HSV Adjust shader matches adjustHsv (hue/sat/bright)\n");
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

    // --- Scenario: Vertex Trail queues snapshots, offset in z + hue-rotated by age ---
    // Push the same 1-vertex Pos3 input 3 frames; the trail holds 3 copies at z = 0.3 / 0.8 / 1.3
    // with colours red / hue 0.1 / hue 0.2. Read the output VBO back and verify.
    {
        const float inPos[3] = { 0.1f, 0.2f, 0.3f };
        GLuint inVbo = 0;
        glGenBuffers(1, &inVbo); glBindBuffer(GL_ARRAY_BUFFER, inVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(inPos), inPos, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        Graph g;
        auto tr = std::make_unique<VertexTrailNode>();
        tr->inputDefault(0) = VertexRef{ inVbo, 1, Primitive::Lines, VertexFormat::Pos3 };
        tr->inputDefault(1) = 8.0f;    // max frames
        tr->inputDefault(2) = 0.5f;    // z spacing
        tr->inputDefault(3) = 0.1f;    // hue rate
        tr->initGL();
        int tId = g.addNode(std::move(tr));
        auto* tn = dynamic_cast<VertexTrailNode*>(g.findNode(tId));
        for (int f = 0; f < 3; ++f) g.evaluate(1.0f / 60.0f);

        VertexRef out = tn->output();
        if (out.vbo == 0 || out.count != 3 || out.format != VertexFormat::Pos3Color3) {
            glDeleteBuffers(1, &inVbo); glfwTerminate(); return fail("Vertex Trail wrong output shape");
        }
        float o[18];
        glBindBuffer(GL_ARRAY_BUFFER, out.vbo);
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, 18 * sizeof(float), o);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        auto af = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
        glm::vec3 c1 = hsvToRgb(0.1f, 1.0f, 1.0f);   // age 1
        glm::vec3 c2 = hsvToRgb(0.2f, 1.0f, 1.0f);   // age 2
        bool ok =
            af(o[0],0.1f)  && af(o[1],0.2f) && af(o[2],0.3f) && af(o[3],1.0f)  && af(o[4],0.0f)  && af(o[5],0.0f)   &&  // age0 red
            af(o[6],0.1f)  && af(o[7],0.2f) && af(o[8],0.8f) && af(o[9],c1.x)  && af(o[10],c1.y) && af(o[11],c1.z)  &&  // age1
            af(o[12],0.1f) && af(o[13],0.2f)&& af(o[14],1.3f)&& af(o[15],c2.x) && af(o[16],c2.y) && af(o[17],c2.z);     // age2
        if (!ok) {
            std::fprintf(stderr, "Vertex Trail got z=(%.3f,%.3f,%.3f)\n", o[2], o[8], o[14]);
            glDeleteBuffers(1, &inVbo); glfwTerminate(); return fail("Vertex Trail z-offset/hue wrong");
        }
        glDeleteBuffers(1, &inVbo);
        std::fprintf(stderr, "gl_smoke OK: Vertex Trail queued snapshots with z-offset + hue rotation\n");
    }

    // --- Scenario: Vertex Trail (LineStrip) -> Wireframe draws each snapshot via glMultiDrawArrays ---
    // A 4-point LineStrip pushed for 3 frames builds a 3-snapshot trail kept COMPACT (strips = frame
    // count, primitive LineStrip); Wireframe must render it with glMultiDrawArrays. Asserts the strip
    // layout + that the rendered texture is non-blank (the multi-draw path executes and draws).
    {
        const float strip[12] = {   // a 4-point Pos3 line strip (a zig-zag near origin, all distinct)
            -0.6f, -0.3f, 0.0f,   -0.2f, 0.3f, 0.0f,   0.2f, -0.3f, 0.0f,   0.6f, 0.3f, 0.0f,
        };
        GLuint inVbo = 0;
        glGenBuffers(1, &inVbo); glBindBuffer(GL_ARRAY_BUFFER, inVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(strip), strip, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        Graph g;
        auto tr   = std::make_unique<VertexTrailNode>();
        tr->inputDefault(0) = VertexRef{ inVbo, 4, Primitive::LineStrip, VertexFormat::Pos3 };
        tr->inputDefault(1) = 5.0f;    // max frames
        tr->inputDefault(2) = 0.2f;    // z spacing
        tr->inputDefault(3) = 0.05f;   // hue rate
        auto wire = std::make_unique<WireframeNode>();
        wire->inputDefault(1) = 0.0f;  // spin off -> static
        auto out  = std::make_unique<OutputNode>();
        tr->initGL(); wire->initGL(); out->initGL();
        int tId = g.addNode(std::move(tr));
        int wId = g.addNode(std::move(wire));
        int oId = g.addNode(std::move(out));
        if (!g.connect(tId, 0, wId, 0) || !g.connect(wId, 0, oId, 0)) {
            glDeleteBuffers(1, &inVbo); glfwTerminate(); return fail("connect trail->wire->output");
        }
        for (int f = 0; f < 3; ++f) g.evaluate(1.0f / 60.0f);

        VertexRef to = dynamic_cast<VertexTrailNode*>(g.findNode(tId))->output();
        if (to.primitive != Primitive::LineStrip || to.strips != 3 || to.count != 3 * 4) {
            std::fprintf(stderr, "trail strips=%d count=%d prim=%d\n", to.strips, to.count, (int)to.primitive);
            glDeleteBuffers(1, &inVbo); glfwTerminate(); return fail("trail did not produce 3 compact strips");
        }
        TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
        if (!t.id) { glDeleteBuffers(1, &inVbo); glfwTerminate(); return fail("multi-strip wireframe texture not produced"); }
        std::vector<unsigned char> px((size_t)t.w * t.h * 4);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        bool sawLine = false;
        for (size_t i = 0; i < px.size(); i += 4)
            if (px[i] > 40 || px[i+1] > 40 || px[i+2] > 40) { sawLine = true; break; }   // any foreground pixel
        glDeleteBuffers(1, &inVbo);
        if (!sawLine) { glfwTerminate(); return fail("multi-strip wireframe (glMultiDrawArrays) rendered nothing"); }
        std::fprintf(stderr, "gl_smoke OK: Vertex Trail LineStrip drawn as %d separate strips via glMultiDrawArrays\n", to.strips);
    }

    // --- Scenario: Vertex Trail -> Deform -> Wireframe keeps each snapshot a separate strip ---
    // Regression for the per-snapshot `strips` hint surviving Deform's transform feedback. The trail
    // emits a COMPACT multi-strip LineStrip (strips = frame count); Deform must forward VertexRef::strips
    // through transform feedback, else the Wireframe draws the whole buffer as ONE LINE_STRIP and joins
    // every snapshot with a connecting line. Asserts the strip layout reaches Deform's output intact and
    // the multi-draw pipeline renders. (The plain Trail->Wireframe path is covered above; this is the
    // Deform-in-the-middle path -- the real-world Oscilloscope -> Trail -> Deform -> Wireframe chain.)
    {
        const float strip[12] = {   // a 4-point Pos3 line strip
            -0.6f, -0.3f, 0.0f,   -0.2f, 0.3f, 0.0f,   0.2f, -0.3f, 0.0f,   0.6f, 0.3f, 0.0f,
        };
        GLuint inVbo = 0;
        glGenBuffers(1, &inVbo); glBindBuffer(GL_ARRAY_BUFFER, inVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(strip), strip, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        Graph g;
        auto tr  = std::make_unique<VertexTrailNode>();
        tr->inputDefault(0) = VertexRef{ inVbo, 4, Primitive::LineStrip, VertexFormat::Pos3 };
        tr->inputDefault(1) = 5.0f;    // max frames
        tr->inputDefault(2) = 0.2f;    // z spacing
        tr->inputDefault(3) = 0.05f;   // hue rate
        auto def = std::make_unique<DeformNode>();
        def->inputDefault(1) = 0.5f;                                 // position uniform
        def->inputDefault(3) = ShaderRef{ vertexShaderSource(1) };   // Twist preset (a real TF shader)
        auto wire = std::make_unique<WireframeNode>();
        wire->inputDefault(1) = 0.0f;  // spin off -> static
        auto out  = std::make_unique<OutputNode>();
        tr->initGL(); def->initGL(); wire->initGL(); out->initGL();
        int tId = g.addNode(std::move(tr));
        int dId = g.addNode(std::move(def));
        int wId = g.addNode(std::move(wire));
        int oId = g.addNode(std::move(out));
        if (!g.connect(tId, 0, dId, 0) || !g.connect(dId, 0, wId, 0) || !g.connect(wId, 0, oId, 0)) {
            glDeleteBuffers(1, &inVbo); glfwTerminate(); return fail("connect trail->deform->wire->output");
        }
        for (int f = 0; f < 3; ++f) g.evaluate(1.0f / 60.0f);

        // The key regression assertion: Deform forwards the trail's 3-strip layout (3 snapshots x 4 verts).
        VertexRef td = dynamic_cast<DeformNode*>(g.findNode(dId))->output();
        if (td.primitive != Primitive::LineStrip || td.strips != 3 || td.count != 3 * 4) {
            std::fprintf(stderr, "deform out strips=%d count=%d prim=%d\n", td.strips, td.count, (int)td.primitive);
            glDeleteBuffers(1, &inVbo); glfwTerminate();
            return fail("Deform dropped the per-snapshot strips hint (Wireframe would join the trail)");
        }
        TexRef t = dynamic_cast<OutputNode*>(g.findNode(oId))->current();
        if (!t.id) { glDeleteBuffers(1, &inVbo); glfwTerminate(); return fail("trail->deform->wire texture not produced"); }
        std::vector<unsigned char> px((size_t)t.w * t.h * 4);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        bool sawLine = false;
        for (size_t i = 0; i < px.size(); i += 4)
            if (px[i] > 40 || px[i+1] > 40 || px[i+2] > 40) { sawLine = true; break; }
        glDeleteBuffers(1, &inVbo);
        if (!sawLine) { glfwTerminate(); return fail("trail->deform->wire (glMultiDrawArrays) rendered nothing"); }
        std::fprintf(stderr, "gl_smoke OK: Vertex Trail strips survive Deform; drawn as %d separate strips\n", td.strips);
    }

    // --- Scenario: project save/load round-trips a real graph through a factory + initGL ---
    {
        auto factory = [](const std::string& t) -> std::unique_ptr<Node> {
            if (t == "Colour") return std::make_unique<ColourNode>();
            if (t == "Output") return std::make_unique<OutputNode>();
            return nullptr;
        };
        auto init = [](Node& n){ n.initGL(); };

        Graph g;
        auto col = std::make_unique<ColourNode>();
        col->inputDefault(0) = Value(glm::vec4(0.25f, 0.5f, 0.75f, 1.0f));   // a known colour
        col->initGL();
        auto out = std::make_unique<OutputNode>(); out->initGL();
        int cId = g.addNode(std::move(col));
        int oId = g.addNode(std::move(out));
        if (!g.connect(cId, 0, oId, 0)) { glfwTerminate(); return fail("save/load: connect Colour->Output"); }

        std::string text = saveProject(g);

        Graph g2;
        if (!loadProject(text, g2, factory, init)) { glfwTerminate(); return fail("save/load: loadProject returned false"); }
        if (g2.nodes().size() != 2 || g2.connections().size() != 1) { glfwTerminate(); return fail("save/load: graph shape not restored"); }
        Node* col2 = nullptr;
        for (auto& np : g2.nodes()) if (np->name() == "Colour") col2 = np.get();
        if (!col2) { glfwTerminate(); return fail("save/load: Colour node missing after load"); }
        glm::vec4 c = std::get<glm::vec4>(col2->inputDefault(0));
        if (!(std::fabs(c.x - 0.25f) < 1e-4f && std::fabs(c.y - 0.5f) < 1e-4f && std::fabs(c.z - 0.75f) < 1e-4f)) {
            glfwTerminate(); return fail("save/load: Colour control value not restored");
        }
        g2.evaluate(1.0f / 60.0f);   // the restored + initGL'd graph evaluates without crashing
        std::fprintf(stderr, "gl_smoke OK: project save/load round-trips a real graph\n");
    }

    // --- Scenario: Drum Machine triggers a sample on a step + applies accent / pan (GL-free) ---
    {
        auto ramp = [](int frames) {
            AudioClip c; c.ok = true; c.sampleRate = 48000; c.channels = 2;
            c.samples.assign((std::size_t)frames * 2, 1.0f);   // constant 1.0 L/R
            return c;
        };
        // 20 input Values in port order; voice 0 vol=1 rate=1 pan=`pan`, sync off (free clock fires step 0).
        auto inputs = [](float pan) {
            std::vector<Value> in;
            for (int v = 0; v < 4; ++v) {
                in.push_back(Value(std::string("")));                       // file
                in.push_back(Value(1.0f));                                  // vol
                in.push_back(Value(1.0f));                                  // rate
                in.push_back(Value(v == 0 ? pan : 0.0f));                   // pan
            }
            in.push_back(Value(120.0f));   // tempo
            in.push_back(Value(false));    // sync (free)
            in.push_back(Value(4.0f));     // rate sync (1/16)
            in.push_back(Value(1.0f));     // pattern
            return in;
        };
        auto peak = [](const AudioRef& a) {
            float m = 0.0f; for (std::size_t i = 0; i < a.count; ++i) m = std::max(m, std::fabs(a.samples[i])); return m;
        };

        // (a) an ON cell triggers voice 0 -> non-zero output on both channels (center pan).
        DrumMachineNode dm;
        dm.injectClip(0, ramp(64));
        dm.patterns().setCell(0, 0, 0, 1);
        std::vector<Value> in = inputs(0.0f), outs(2);
        EvalContext ctx{in, outs, 0.02f, nullptr, nullptr};
        dm.evaluate(ctx);
        float onPeak = peak(dm.leftOut());
        if (onPeak <= 0.05f) { glfwTerminate(); return fail("Drum Machine: ON step produced no audio"); }
        if (peak(dm.rightOut()) <= 0.05f) { glfwTerminate(); return fail("Drum Machine: center pan gave no right channel"); }

        // (b) an ACCENT cell is louder than an ON cell.
        DrumMachineNode dmA;
        dmA.injectClip(0, ramp(64));
        dmA.patterns().setCell(0, 0, 0, 2);   // accent
        std::vector<Value> inA = inputs(0.0f), outsA(2);
        EvalContext ctxA{inA, outsA, 0.02f, nullptr, nullptr};
        dmA.evaluate(ctxA);
        if (peak(dmA.leftOut()) <= onPeak + 0.01f) { glfwTerminate(); return fail("Drum Machine: accent not louder than on"); }

        // (c) hard-left pan routes to left only.
        DrumMachineNode dmP;
        dmP.injectClip(0, ramp(64));
        dmP.patterns().setCell(0, 0, 0, 1);
        std::vector<Value> inP = inputs(-1.0f), outsP(2);
        EvalContext ctxP{inP, outsP, 0.02f, nullptr, nullptr};
        dmP.evaluate(ctxP);
        if (peak(dmP.leftOut()) <= 0.05f || peak(dmP.rightOut()) >= 0.02f) {
            glfwTerminate(); return fail("Drum Machine: hard-left pan did not route to left only");
        }
        std::fprintf(stderr, "gl_smoke OK: Drum Machine triggers a step, accent louder, pan routes\n");
    }

    // projectM node, library-absent path (what CI sees). ProjectMApi::load() has deliberately not
    // been called yet in this process, so the node must be inert.
    {
        ProjectMNode pm;
        pm.initGL();
        std::vector<Value> ins = defaultInputs(pm), outs(1);
        EvalContext ctx{ins, outs, 1.0f / 60.0f, nullptr, nullptr};
        pm.evaluate(ctx);
        TexRef t = std::get<TexRef>(outs[0]);
        if (t.id == 0 || t.w != kCanvasW || t.h != kCanvasH) { glfwTerminate(); return fail("projectM (inert): no canvas-sized texture"); }
        int r, gg, b, a;
        readCentre(t, r, gg, b, a);
        if (!(r == 0 && gg == 0 && b == 0 && a == 255)) { glfwTerminate(); return fail("projectM (inert): texture not opaque black"); }
        if (pm.statusLine().empty()) { glfwTerminate(); return fail("projectM (inert): empty status line"); }

        // Playlist write-back needs no library: next from a.milk -> b.milk, written into the field.
        std::string dir = writePresetFolder();
        if (dir.empty()) { glfwTerminate(); return fail("projectM: write preset folder"); }
        ins[ProjectMNode::kPreset] = Value(dir + "/a.milk");
        pm.evaluate(ctx);
        if (fileBaseName(std::get<std::string>(pm.inputDefault(ProjectMNode::kPreset))) != "a.milk") { glfwTerminate(); return fail("projectM: incoming preset not written back"); }
        pm.onButtonPressed(1);   // next
        pm.evaluate(ctx);        // `ins` still says a.milk, like an unchanged edge: the step must hold
        if (fileBaseName(std::get<std::string>(pm.inputDefault(ProjectMNode::kPreset))) != "b.milk") { glfwTerminate(); return fail("projectM: next did not step to b.milk"); }
        pm.evaluate(ctx);
        if (fileBaseName(std::get<std::string>(pm.inputDefault(ProjectMNode::kPreset))) != "b.milk") { glfwTerminate(); return fail("projectM: step snapped back"); }
        std::fprintf(stderr, "gl_smoke OK: projectM inert path (black texture, status '%s') + playlist write-back\n", pm.statusLine().c_str());
    }

    // projectM node, live path. Needs libprojectM 4.2+ (OSS_PROJECTM_LIB overrides the search).
    {
        const char* envLib = std::getenv("OSS_PROJECTM_LIB");
        ProjectMApi& api = ProjectMApi::instance();
        if (!api.load(envLib ? envLib : "")) {
            std::fprintf(stderr, "gl_smoke SKIP: projectM render/burn checks (%s)\n", api.statusText().c_str());
        } else {
            std::string dir = writePresetFolder();
            if (dir.empty()) { glfwTerminate(); return fail("projectM live: write preset folder"); }

            std::vector<float> sine(800);
            for (std::size_t i = 0; i < sine.size(); ++i) sine[i] = 0.8f * std::sin(2.0f * 3.14159265f * 220.0f * (float)i / 48000.0f);

            ProjectMNode pm;
            pm.initGL();
            std::vector<Value> ins = defaultInputs(pm), outs(1);
            ins[ProjectMNode::kLeft]   = Value(AudioRef{sine.data(), sine.size(), 48000});   // right mirrors it
            ins[ProjectMNode::kPreset] = Value(dir + "/a.milk");
            ins[ProjectMNode::kBlend]  = Value(false);                                       // hard cut: no blend wait
            EvalContext ctx{ins, outs, 1.0f / 60.0f, nullptr, nullptr};

            // (a) GL state is RESTORED, not reset. Every value installed here is non-default, so a
            // guard that reset to GL defaults on exit instead of restoring what it saved would fail.
            // (The 2D texture binding is deliberately not asserted: the guard only restores it for
            // the unit that was active on entry, by design.)
            GLuint probeVao = 0, probeBuf = 0;
            glGenVertexArrays(1, &probeVao);
            glGenBuffers(1, &probeBuf);
            GLuint probeProg = linkProgram(
                "#version 410 core\nvoid main() { gl_Position = vec4(0.0); }\n",
                "#version 410 core\nout vec4 f;\nvoid main() { f = vec4(1.0, 0.0, 1.0, 1.0); }\n");
            if (probeProg == 0) { glfwTerminate(); return fail("projectM live: could not link the GL-state probe program"); }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, 33, 44);
            glUseProgram(probeProg);
            glBindVertexArray(probeVao);
            glBindBuffer(GL_ARRAY_BUFFER, probeBuf);
            glActiveTexture(GL_TEXTURE3);
            for (int i = 0; i < 10; ++i) pm.evaluate(ctx);
            GLint fb = -1, vp[4] = {0, 0, 0, 0}, prog = -1, vao = -1, arrayBuf = -1, activeTex = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
            glGetIntegerv(GL_VIEWPORT, vp);
            glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuf);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
            std::fprintf(stderr, "gl_smoke projectM status: %s\n", pm.statusLine().c_str());
            bool stateKept = fb == 0 && vp[2] == 33 && vp[3] == 44 && prog == (GLint)probeProg &&
                             vao == (GLint)probeVao && arrayBuf == (GLint)probeBuf &&
                             activeTex == GL_TEXTURE3;
            glActiveTexture(GL_TEXTURE0);
            glUseProgram(0); glBindVertexArray(0); glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteProgram(probeProg); glDeleteBuffers(1, &probeBuf); glDeleteVertexArrays(1, &probeVao);
            if (!stateKept) { glfwTerminate(); return fail("projectM live: GL state leaked out of evaluate"); }

            // (b) it rendered something.
            TexRef t = std::get<TexRef>(outs[0]);
            std::vector<unsigned char> px((size_t)t.w * t.h * 4);
            glBindTexture(GL_TEXTURE_2D, t.id);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            bool lit = false;
            for (size_t i = 0; i + 3 < px.size() && !lit; i += 4) lit = (px[i] + px[i + 1] + px[i + 2]) > 60;
            if (!lit) { glfwTerminate(); return fail("projectM live: output is black (see the status line above)"); }

            // (c) the status shows the playlist position after a step.
            pm.onButtonPressed(1);
            pm.evaluate(ctx);
            if (pm.statusLine().find("(2/3)") == std::string::npos) { glfwTerminate(); return fail("projectM live: status does not show (2/3) after next"); }

            // (d) burn: a texture whose TOP half is red and BOTTOM half green (GL rows are bottom-up).
            const int S = 64;
            std::vector<unsigned char> img((size_t)S * S * 4);
            for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) {
                unsigned char* p = &img[((size_t)y * S + x) * 4];
                bool top = (y >= S / 2);
                p[0] = top ? 255 : 0; p[1] = top ? 0 : 255; p[2] = 0; p[3] = 255;
            }
            GLuint src = 0;
            glGenTextures(1, &src);
            glBindTexture(GL_TEXTURE_2D, src);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            ins[ProjectMNode::kTexIn] = Value(TexRef{src, S, S});
            ins[ProjectMNode::kBurn]  = Value(1.0f);
            for (int i = 0; i < 5; ++i) pm.evaluate(ctx);
            t = std::get<TexRef>(outs[0]);
            int r1, g1, b1, a1, r2, g2, b2, a2;
            readAtUV(t, 0.2f, 0.85f, r1, g1, b1, a1);   // upper area, clear of the border and the centre wave
            readAtUV(t, 0.2f, 0.15f, r2, g2, b2, a2);   // lower area
            std::fprintf(stderr, "gl_smoke projectM burn: upper=(%d,%d,%d) lower=(%d,%d,%d)\n", r1, g1, b1, r2, g2, b2);
            bool upperRed = r1 > g1 + 40, lowerGreen = g2 > r2 + 40;
            bool upperGreen = g1 > r1 + 40, lowerRed = r2 > g2 + 40;
            if (upperGreen && lowerRed) { glfwTerminate(); return fail("projectM live: burn is vertically flipped (plan Task 9 Step 4)"); }
            if (!(upperRed && lowerGreen)) { glfwTerminate(); return fail("projectM live: burned texture not visible in the output"); }

            // (d2) burn respects alpha: a transparent region must NOT overwrite the canvas.
            // Top half opaque red, bottom half GREEN WITH ALPHA 0. With the node's straight-alpha
            // blend the lower area keeps the preset's own pixels; an unblended copy would write green.
            // (d)'s stamp has to be gone first or this would prove nothing. fDecay=0.98 is far too
            // slow: measured, the lower sample is still (0,142,0) after 60 burn-off frames and
            // (0,46,0) after 120 -- green-dominant throughout, and only creeping up on the
            // threshold. So hard-cut to a third preset file instead: a freshly loaded preset
            // starts on a clean canvas, in 5 frames.
            ins[ProjectMNode::kPreset] = Value(dir + "/c.milk");
            ins[ProjectMNode::kBurn]   = Value(0.0f);
            for (int i = 0; i < 5; ++i) pm.evaluate(ctx);
            t = std::get<TexRef>(outs[0]);
            int r3, g3, b3, a3;
            readAtUV(t, 0.2f, 0.15f, r3, g3, b3, a3);
            std::fprintf(stderr, "gl_smoke projectM pre-alpha-burn lower=(%d,%d,%d)\n", r3, g3, b3);
            if (g3 > r3 + 40) { glfwTerminate(); return fail("projectM live: canvas still green before the alpha burn, so it would prove nothing"); }
            for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) {
                unsigned char* p = &img[((size_t)y * S + x) * 4];
                bool top = (y >= S / 2);
                p[0] = top ? 255 : 0; p[1] = top ? 0 : 255; p[2] = 0; p[3] = top ? 255 : 0;
            }
            glBindTexture(GL_TEXTURE_2D, src);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
            ins[ProjectMNode::kBurn] = Value(1.0f);
            for (int i = 0; i < 5; ++i) pm.evaluate(ctx);
            t = std::get<TexRef>(outs[0]);
            int r4, g4, b4, a4, r5, g5, b5, a5;
            readAtUV(t, 0.2f, 0.85f, r4, g4, b4, a4);
            readAtUV(t, 0.2f, 0.15f, r5, g5, b5, a5);
            std::fprintf(stderr, "gl_smoke projectM alpha burn: upper=(%d,%d,%d) lower=(%d,%d,%d)\n", r4, g4, b4, r5, g5, b5);
            glDeleteTextures(1, &src);
            if (!(r4 > g4 + 40)) { glfwTerminate(); return fail("projectM live: the opaque half of the alpha burn did not land"); }
            if (g5 > r5 + 40)    { glfwTerminate(); return fail("projectM live: burn ignored alpha (a transparent region overwrote the canvas)"); }

            // (e) A MULTI-SAMPLER preset must not leak sampler objects or the read framebuffer.
            // Audited against projectM 4.2: it binds a sampler per texture unit and unbinds only
            // unit 0, and it leaves READ_FRAMEBUFFER on an internal FBO. A one-sampler preset (like
            // kTestPreset) leaks nothing, so this needs a Milkdrop-2 preset whose shaders sample
            // main + blur + noise textures. GLStateGuard is what contains both.
            std::string multi = writeMultiSamplerPreset();
            if (multi.empty()) { glfwTerminate(); return fail("projectM live: write multi-sampler preset"); }
            ins[ProjectMNode::kBurn]   = Value(0.0f);
            ins[ProjectMNode::kPreset] = Value(multi);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            for (GLuint u = 0; u < 8; ++u) glBindSampler(u, 0);
            // Units 1..5 start on a SENTINEL, not 0, so the assertion below stays meaningful even if a
            // future projectM binds no samplers at all (or this preset quietly falls back to the
            // one-sampler default shaders): the guard must clear them either way.
            GLuint sentinel = 0;
            glGenSamplers(1, &sentinel);
            for (GLuint u = 1; u <= 5; ++u) glBindSampler(u, sentinel);
            for (int i = 0; i < 10; ++i) pm.evaluate(ctx);
            std::fprintf(stderr, "gl_smoke projectM multi-sampler status: %s\n", pm.statusLine().c_str());
            if (pm.statusLine().rfind("failed:", 0) == 0) { glfwTerminate(); return fail("projectM live: the multi-sampler preset did not load, so the leak check would be vacuous"); }
            GLint readFb = -1;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
            int leaked = 0;
            for (GLuint u = 1; u <= 5; ++u) {
                GLint s = -1;
                glActiveTexture(GL_TEXTURE0 + u);
                glGetIntegerv(GL_SAMPLER_BINDING, &s);
                if (s != 0) { ++leaked; std::fprintf(stderr, "gl_smoke projectM: sampler %d left on unit %u\n", s, u); }
            }
            glActiveTexture(GL_TEXTURE0);
            glDeleteSamplers(1, &sentinel);
            if (leaked)      { glfwTerminate(); return fail("projectM live: sampler objects leaked onto texture units"); }
            if (readFb != 0) { glfwTerminate(); return fail("projectM live: READ_FRAMEBUFFER binding leaked"); }

            // (f) projectM must render correctly whatever GL state the previous node left, and hand it back.
            // It never touches scissor/cull/depth anywhere in its source, and sets no blend state for a
            // plain texture copy, so a badly-behaved node upstream could clip or erase its whole output.
            ins[ProjectMNode::kPreset] = Value(dir + "/a.milk");
            ins[ProjectMNode::kBurn]   = Value(0.0f);
            // Zero the node's canvas first. Nothing clears it between frames -- the preset's output
            // covers it every frame -- so a render that got scissored away entirely would leave (e)'s
            // picture sitting there and this check would pass without projectM drawing a thing.
            // (Measured: without this the hostile run reports 100% lit; with it, 0%.)
            t = std::get<TexRef>(outs[0]);
            px.assign((size_t)t.w * t.h * 4, 0);
            glBindTexture(GL_TEXTURE_2D, t.id);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 1, 1);      // would clip everything but one pixel
            glEnable(GL_CULL_FACE);
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_BLEND); glBlendFunc(GL_ZERO, GL_ONE);      // would draw nothing at all
            for (int i = 0; i < 10; ++i) pm.evaluate(ctx);
            bool handedBack = glIsEnabled(GL_SCISSOR_TEST) && glIsEnabled(GL_CULL_FACE) &&
                              glIsEnabled(GL_DEPTH_TEST) && glIsEnabled(GL_BLEND);
            GLint srcRgb = 0, dstRgb = 0;
            glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb); glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
            glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ZERO);
            if (!handedBack || srcRgb != GL_ZERO || dstRgb != GL_ONE) { glfwTerminate(); return fail("projectM live: did not hand the caller's GL state back"); }
            t = std::get<TexRef>(outs[0]);
            px.assign((size_t)t.w * t.h * 4, 0);
            glBindTexture(GL_TEXTURE_2D, t.id);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            size_t litPixels = 0;
            for (size_t i = 0; i + 3 < px.size(); i += 4)
                if (px[i] + px[i + 1] + px[i + 2] > 60) ++litPixels;
            std::fprintf(stderr, "gl_smoke projectM hostile state: %.1f%% of the canvas lit\n",
                         100.0 * (double)litPixels / ((double)t.w * (double)t.h));
            if (litPixels * 100 <= (size_t)t.w * (size_t)t.h) { glfwTerminate(); return fail("projectM live: output was clipped/culled/blended away by the caller's GL state"); }

            // (g) a preset path that is not a file must be REPORTED, not handed to projectM: typing a
            // path changes the incoming value per keystroke, and a load attempt each time costs tens of
            // milliseconds. What is already playing must keep rendering.
            ins[ProjectMNode::kPreset] = Value(dir + "/a.milk");
            for (int i = 0; i < 5; ++i) pm.evaluate(ctx);
            t = std::get<TexRef>(outs[0]);
            px.assign((size_t)t.w * t.h * 4, 0);                 // zero the canvas, as (f) does
            glBindTexture(GL_TEXTURE_2D, t.id);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            ins[ProjectMNode::kPreset] = Value(dir + "/missing.milk");
            for (int i = 0; i < 5; ++i) pm.evaluate(ctx);
            std::fprintf(stderr, "gl_smoke projectM missing preset status: %s\n", pm.statusLine().c_str());
            if (pm.statusLine().rfind("preset not found: missing", 0) != 0) { glfwTerminate(); return fail("projectM live: a missing preset was not reported"); }
            t = std::get<TexRef>(outs[0]);
            px.assign((size_t)t.w * t.h * 4, 0);
            glBindTexture(GL_TEXTURE_2D, t.id);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            size_t stillLit = 0;
            for (size_t i = 0; i + 3 < px.size(); i += 4)
                if (px[i] + px[i + 1] + px[i + 2] > 60) ++stillLit;
            std::fprintf(stderr, "gl_smoke projectM missing preset: %.1f%% of the canvas still lit\n",
                         100.0 * (double)stillLit / ((double)t.w * (double)t.h));
            if (stillLit * 100 <= (size_t)t.w * (size_t)t.h) { glfwTerminate(); return fail("projectM live: the previous preset stopped rendering after a missing preset"); }

            // (h) a bad dt must not latch the node black. projectM takes the accumulated time we hand
            // it verbatim, so a NaN would poison it permanently, and a negative one makes projectM fall
            // back to its own wall clock (losing determinism). The node clamps both to "no advance".
            // Measured with the clamp removed: 0.0% lit here, and still 0.0% after ten good frames --
            // it never recovers. With the clamp: 100%.
            ins[ProjectMNode::kPreset] = Value(dir + "/a.milk");
            for (int i = 0; i < 5; ++i) pm.evaluate(ctx);
            ctx.dt = std::numeric_limits<float>::quiet_NaN();
            pm.evaluate(ctx);
            ctx.dt = -1.0f;
            pm.evaluate(ctx);
            ctx.dt = 1.0f / 60.0f;
            t = std::get<TexRef>(outs[0]);
            px.assign((size_t)t.w * t.h * 4, 0);                 // zero the canvas, as (f) and (g) do
            glBindTexture(GL_TEXTURE_2D, t.id);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (int i = 0; i < 10; ++i) pm.evaluate(ctx);
            t = std::get<TexRef>(outs[0]);
            px.assign((size_t)t.w * t.h * 4, 0);
            glBindTexture(GL_TEXTURE_2D, t.id);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            size_t litAfterBadDt = 0;
            for (size_t i = 0; i + 3 < px.size(); i += 4)
                if (px[i] + px[i + 1] + px[i + 2] > 60) ++litAfterBadDt;
            std::fprintf(stderr, "gl_smoke projectM bad dt: %.1f%% of the canvas lit after a NaN and a negative dt\n",
                         100.0 * (double)litAfterBadDt / ((double)t.w * (double)t.h));
            if (litAfterBadDt * 100 <= (size_t)t.w * (size_t)t.h) { glfwTerminate(); return fail("projectM live: a NaN or negative dt left the node black"); }

            std::fprintf(stderr, "gl_smoke OK: projectM live (renders, GL state + samplers contained, status, burn upright + alpha, survives hostile caller state, reports a missing preset, survives a bad dt)\n");
        }
        std::error_code ec;                                      // the temp preset folders this file wrote
        std::filesystem::remove_all(std::filesystem::temp_directory_path() / "oss_projectm_smoke", ec);
        std::filesystem::remove_all(std::filesystem::temp_directory_path() / "oss_projectm_smoke_multi", ec);
    }

    // GLStateGuard: state changed inside the scope is restored on exit.
    {
        Framebuffer scratch;
        scratch.create(8, 8);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, 17, 19);
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_TRUE);
        glUseProgram(0);
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        GLuint probeVao = 0; glGenVertexArrays(1, &probeVao); glBindVertexArray(probeVao);
        GLuint probeBuf = 0; glGenBuffers(1, &probeBuf);      glBindBuffer(GL_ARRAY_BUFFER, probeBuf);
        GLuint probeSampler = 0; glGenSamplers(1, &probeSampler);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        {
            GLStateGuard guard;
            scratch.bind();                                   // FBO + viewport
            glEnable(GL_BLEND); glEnable(GL_DEPTH_TEST); glEnable(GL_SCISSOR_TEST); glEnable(GL_CULL_FACE);
            glBlendFunc(GL_ONE, GL_ONE);
            glDepthMask(GL_FALSE);
            glBindTexture(GL_TEXTURE_2D, scratch.texture());  // unit 0's binding
            glActiveTexture(GL_TEXTURE3);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, scratch.id());   // what projectM really leaks
            glBindSampler(1, probeSampler);                         // ...and this: a sampler left on unit 1
        }
        GLint fb = -1, vp[4] = {0, 0, 0, 0}, unit = 0, tex = -1, align = 0, srcRgb = 0;
        GLboolean depthMask = GL_FALSE;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        glGetIntegerv(GL_VIEWPORT, vp);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        GLint readFb = -1, vaoNow = -1, arrayBufNow = -1, sampler1 = -1;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vaoNow);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBufNow);
        glActiveTexture(GL_TEXTURE1);
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler1);               // queried per active unit
        glActiveTexture(GL_TEXTURE0);
        bool ok = fb == 0 && vp[2] == 17 && vp[3] == 19 && unit == GL_TEXTURE0 && tex == 0 &&
                  align == 4 && srcRgb == GL_SRC_ALPHA && depthMask == GL_TRUE &&
                  !glIsEnabled(GL_BLEND) && !glIsEnabled(GL_DEPTH_TEST) &&
                  !glIsEnabled(GL_SCISSOR_TEST) && !glIsEnabled(GL_CULL_FACE) &&
                  readFb == 0 && vaoNow == (GLint)probeVao && arrayBufNow == (GLint)probeBuf && sampler1 == 0;
        if (!ok) { glfwTerminate(); return fail("GLStateGuard did not restore the GL state"); }
        glBindVertexArray(0); glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteSamplers(1, &probeSampler); glDeleteBuffers(1, &probeBuf); glDeleteVertexArrays(1, &probeVao);
        std::fprintf(stderr, "gl_smoke OK: GLStateGuard restores framebuffers/viewport/VAO/buffer/texture/enables and clears samplers\n");
    }

    // --- Scenario: offline mode -- Audio Out taps its block and never touches the device ---
    // Split from the Recorder scenario below on purpose: Graph::evaluate() runs every node every
    // frame regardless of connections, so an AudioOutputNode sitting in a graph that takes even
    // one LIVE evaluate (needed to prime a real recording, below) would open the real device on a
    // machine that has one -- deviceTouched() is monotonic (never resets outside the destructor),
    // so that would make the "never touched while offline" check below vacuously true. Keeping
    // Audio Out in its own graph, offline from before its first evaluate, keeps the check honest.
    {
        Graph g;
        auto sine = std::make_unique<SineWaveNode>();
        auto aout = std::make_unique<AudioOutputNode>();
        int sId = g.addNode(std::move(sine));
        int aId = g.addNode(std::move(aout));
        if (!g.connect(sId, 0, aId, 0)) { glfwTerminate(); return fail("offline sinks: connect Sine->AudioOut"); }
        auto* an = dynamic_cast<AudioOutputNode*>(g.findNode(aId));

        g.setOffline(true);   // offline before the first evaluate: the device path is never reached
        for (int f = 0; f < 3; ++f) g.evaluate(1.0f / 60.0f);
        // A lone left wire mirrors to both channels; 800 frames at 48 kHz / 60 fps.
        if (an->lastSampleRate() != 48000) { glfwTerminate(); return fail("offline sinks: Audio Out sample rate not tapped"); }
        if (an->lastBlock().size() != 800 * 2) { glfwTerminate(); return fail("offline sinks: Audio Out block should be 800 stereo frames"); }
        bool mirrored = true;
        for (std::size_t i = 0; i < an->lastBlock().size(); i += 2)
            if (an->lastBlock()[i] != an->lastBlock()[i + 1]) { mirrored = false; break; }
        if (!mirrored) { glfwTerminate(); return fail("offline sinks: lone mono wire should mirror to both channels"); }
        bool nonSilent = false;
        for (float v : an->lastBlock()) if (v > 0.01f || v < -0.01f) { nonSilent = true; break; }
        if (!nonSilent) { glfwTerminate(); return fail("offline sinks: tapped block should carry the sine, not silence"); }
        if (an->deviceTouched()) { glfwTerminate(); return fail("offline sinks: Audio Out must not touch the device while offline"); }

        // Nothing connected -> empty block, rate 0.
        g.disconnect(aId, 0);
        g.evaluate(1.0f / 60.0f);
        if (!an->lastBlock().empty() || an->lastSampleRate() != 0) { glfwTerminate(); return fail("offline sinks: disconnected Audio Out should tap an empty block"); }

        // Positive control: back live, the device path IS reached -- otherwise the negative check
        // above could pass for the wrong reason (deviceTouched() never firing at all).
        g.setOffline(false);
        g.evaluate(1.0f / 60.0f);   // live, still disconnected: reaches the device path, pushes silence
        if (!an->deviceTouched()) { glfwTerminate(); return fail("offline sinks: a live frame should reach the device path"); }
        std::fprintf(stderr, "gl_smoke OK: offline mode taps the Audio Out block and never touches the device\n");
    }

    // --- Scenario: an offline render interrupts a live recording (stop + save), and the Recorder
    //     does not restart -- and truncate the file it just saved -- once the render ends ---
    {
        const char* recFile = "build/_offline_interrupted.mp4";
        Graph g;
        auto col = std::make_unique<ColourNode>(); col->initGL();
        auto rec = std::make_unique<RecorderNode>();
        auto out = std::make_unique<OutputNode>(); out->initGL();
        int cId = g.addNode(std::move(col));
        int rId = g.addNode(std::move(rec));
        int oId = g.addNode(std::move(out));
        if (!g.connect(cId, 0, rId, 0) || !g.connect(rId, 0, oId, 0)) {
            glfwTerminate(); return fail("offline sinks: connect Colour->Recorder->Output");
        }
        std::remove(recFile);
        auto* rn = dynamic_cast<RecorderNode*>(g.findNode(rId));

        // Start a real live recording first, so the offline render actually interrupts one -- the
        // bug this guards (a stale `record` toggle truncating the just-saved file once the render
        // ends) only shows up if a recording was genuinely in progress.
        rn->inputDefault(4) = std::string(recFile);
        rn->inputDefault(3) = true;                          // record on, live
        g.evaluate(1.0f / 60.0f);
        // start() sets "recording..." but the same evaluate() immediately overwrites it with the
        // "REC <time>  <frames>" counter once encoding is under way -- check that prefix instead.
        if (rn->statusLine().rfind("REC ", 0) != 0) { glfwTerminate(); return fail("offline sinks: Recorder should be recording live before the render"); }

        g.setOffline(true);
        g.evaluate(1.0f / 60.0f);   // render begins -> `record` reads false -> stop() + save
        std::string savedStatus = std::string("saved ") + recFile;
        if (rn->statusLine() != savedStatus) { glfwTerminate(); return fail("offline sinks: Recorder should stop and save when the render begins"); }
        if (!std::ifstream(recFile).good()) { glfwTerminate(); return fail("offline sinks: Recorder did not save the interrupted recording"); }

        for (int f = 0; f < 2; ++f) g.evaluate(1.0f / 60.0f);   // rest of the render: stays saved, not idle
        if (rn->statusLine() != savedStatus) { glfwTerminate(); return fail("offline sinks: Recorder should stay saved for the rest of the render"); }

        g.setOffline(false);
        g.evaluate(1.0f / 60.0f);
        // `record` is still armed from before the render -- the suppression latch must keep it
        // from restarting (and truncating the file it just saved) on this first live frame.
        if (rn->statusLine() != savedStatus) { glfwTerminate(); return fail("offline sinks: Recorder must not restart after an offline render ends"); }
        std::fprintf(stderr, "gl_smoke OK: an offline render interrupts a live recording and it does not restart afterward\n");
    }

    // --- Scenario: OfflineRenderer start/cancel -- validation, prefs swap, transport snapshot + restore ---
    {
        Graph g;
        auto col = std::make_unique<ColourNode>(); col->initGL();
        int cId = g.addNode(std::move(col));
        Preferences live; live.textureWidth = 320; live.textureHeight = 240;
        g.setPreferences(&live);
        g.transport().bpm = 120.0; g.transport().seconds = 5.0; g.transport().looping = true;

        RenderSettings s; s.startBar = 0.0; s.endBar = 1.0; s.prerollBars = 0.5; s.fps = 30;
        s.width = 160; s.height = 120; s.outPath = "build/_offline_cancel.mp4";
        OfflineRenderer r; std::string err;
        if (r.start(g, s, err)) { glfwTerminate(); return fail("offline start: should refuse a graph with no Output node"); }
        if (err != "add an Output node") { glfwTerminate(); return fail("offline start: wrong error for a missing Output node"); }
        if (r.progress().phase != OfflineRenderer::Phase::Failed || r.progress().outPath != s.outPath)
            { glfwTerminate(); return fail("offline start: a rejected start should still report its outPath"); }

        auto out = std::make_unique<OutputNode>(); out->initGL();
        int oId = g.addNode(std::move(out));
        if (!g.connect(cId, 0, oId, 0)) { glfwTerminate(); return fail("offline start: connect"); }
        if (!r.start(g, s, err)) { glfwTerminate(); return fail(("offline start: " + err).c_str()); }
        if (!r.active()) { glfwTerminate(); return fail("offline start: not active"); }
        if (r.progress().framesTotal != 60 || r.progress().prerollTotal != 30) { glfwTerminate(); return fail("offline start: expected 60 frames + 30 pre-roll (0.5 bar = 1 s at 30 fps)"); }
        if (!g.offline()) { glfwTerminate(); return fail("offline start: graph should be offline"); }
        const Transport& t = g.transport();
        if (!(t.externalClock && t.playing && !t.looping)) { glfwTerminate(); return fail("offline start: transport should be an external, playing, non-looping clock"); }

        // The render-size prefs swap actually reaches the graph (not just inferred later from
        // the restore): a ColourNode is a ShaderNode and resizes its FBO from ctx.prefs.
        g.evaluate(1.0f / 60.0f);
        auto* onDuring = dynamic_cast<OutputNode*>(g.findNode(oId));
        if (onDuring->current().w != s.width || onDuring->current().h != s.height)
            { glfwTerminate(); return fail("offline start: render size did not reach the graph"); }

        // Starting a second job while this one is active is refused -- and must not disturb it.
        RenderSettings s2 = s; s2.outPath = "build/_offline_cancel2.mp4";
        std::string err2;
        if (r.start(g, s2, err2)) { glfwTerminate(); return fail("offline start: should refuse a second job while active"); }
        if (err2 != "a render is already running") { glfwTerminate(); return fail("offline start: wrong error for starting while active"); }
        if (!r.active() || r.progress().framesTotal != 60)
            { glfwTerminate(); return fail("offline start: a rejected second start must not disturb the running job"); }

        r.cancel();
        if (r.active()) { glfwTerminate(); return fail("offline cancel: still active"); }
        if (r.progress().phase != OfflineRenderer::Phase::Cancelled) { glfwTerminate(); return fail("offline cancel: phase should be Cancelled"); }
        if (g.offline()) { glfwTerminate(); return fail("offline cancel: graph still offline"); }
        if (!(t.seconds == 5.0 && t.looping && !t.playing && !t.externalClock && t.bpm == 120.0)) { glfwTerminate(); return fail("offline cancel: transport not restored"); }
        if (g.preferences() != &live) { glfwTerminate(); return fail("offline cancel: prefs pointer not restored to the graph's original"); }
        g.evaluate(1.0f / 60.0f);
        auto* on = dynamic_cast<OutputNode*>(g.findNode(oId));
        if (on->current().w != 320 || on->current().h != 240) { glfwTerminate(); return fail("offline cancel: live prefs (texture size) not restored"); }

        {   // a renderer that never ran: cancel + destruction must be no-ops (graph_ is null)
            OfflineRenderer idle;
            idle.cancel();
            if (idle.active() || idle.progress().phase != OfflineRenderer::Phase::Idle)
                { glfwTerminate(); return fail("offline: cancel on a never-started renderer should do nothing"); }
        }

        std::string statusBeforeSecondCancel = r.progress().status;
        r.cancel();                                              // idempotent
        if (r.active() || r.progress().phase != OfflineRenderer::Phase::Cancelled
            || r.progress().status != statusBeforeSecondCancel)
            { glfwTerminate(); return fail("offline cancel: second cancel should be a no-op"); }
        std::fprintf(stderr, "gl_smoke OK: OfflineRenderer start validates, swaps prefs + clock, and cancel restores them\n");
    }

    // --- Scenario: start() has no separate prefs argument -- the render-time copy and the
    //     restored value are both sourced from g.preferences(), so they cannot disagree ---
    {
        Graph g;
        auto col = std::make_unique<ColourNode>(); col->initGL();
        int cId = g.addNode(std::move(col));
        auto out = std::make_unique<OutputNode>(); out->initGL();
        int oId = g.addNode(std::move(out));
        if (!g.connect(cId, 0, oId, 0)) { glfwTerminate(); return fail("offline prefs source: connect"); }

        Preferences real; real.textureWidth = 400; real.textureHeight = 300;
        g.setPreferences(&real);

        RenderSettings s; s.startBar = 0.0; s.endBar = 1.0; s.prerollBars = 0.0; s.fps = 30;
        s.width = 160; s.height = 120; s.outPath = "build/_offline_prefs_source.mp4";

        OfflineRenderer r; std::string err;
        if (!r.start(g, s, err)) { glfwTerminate(); return fail(("offline prefs source: start: " + err).c_str()); }
        // The render-time copy was built from the graph's actual prefs (real), then resized.
        g.evaluate(1.0f / 60.0f);
        auto* on = dynamic_cast<OutputNode*>(g.findNode(oId));
        if (on->current().w != s.width || on->current().h != s.height)
            { glfwTerminate(); return fail("offline prefs source: render size did not reach the graph"); }
        r.cancel();
        if (g.preferences() != &real)
            { glfwTerminate(); return fail("offline prefs source: restore should return the graph's actual prefs pointer"); }
        std::fprintf(stderr, "gl_smoke OK: OfflineRenderer sources the render-time copy and the restore from the same g.preferences(), with no separate argument to disagree\n");
    }

    // --- Scenario: offline render writes every frame of a bar range, sample-locked, and restores state ---
    {
        Graph g;
        auto col  = std::make_unique<ColourNode>(); col->initGL();
        auto rec  = std::make_unique<RecorderNode>();
        rec->inputDefault(3) = true;                                            // a LIVE recording in progress
        rec->inputDefault(4) = std::string("build/_offline_live_rec.mp4");
        auto out  = std::make_unique<OutputNode>(); out->initGL();
        auto sine = std::make_unique<SineWaveNode>();
        auto aout = std::make_unique<AudioOutputNode>();
        int cId = g.addNode(std::move(col));  int rId = g.addNode(std::move(rec));
        int oId = g.addNode(std::move(out));  int sId = g.addNode(std::move(sine));
        int aId = g.addNode(std::move(aout));
        if (!g.connect(cId, 0, rId, 0) || !g.connect(rId, 0, oId, 0) || !g.connect(sId, 0, aId, 0)) {
            glfwTerminate(); return fail("offline e2e: connect");
        }
        Preferences live; live.textureWidth = 320; live.textureHeight = 240;
        g.setPreferences(&live);
        g.transport().bpm = 120.0; g.transport().seconds = 5.0; g.transport().looping = true;
        g.evaluate(1.0f / 60.0f);                         // one live frame: 320x240, recorder starts
        auto* on = dynamic_cast<OutputNode*>(g.findNode(oId));
        auto* rn = dynamic_cast<RecorderNode*>(g.findNode(rId));
        if (on->current().w != 320) { glfwTerminate(); return fail("offline e2e: live size not applied"); }
        if (rn->statusLine().rfind("REC", 0) != 0) { glfwTerminate(); return fail("offline e2e: live recorder should be recording"); }

        RenderSettings s; s.startBar = 0.0; s.endBar = 1.0; s.prerollBars = 0.5; s.fps = 30;
        s.width = 160; s.height = 120; s.outPath = "build/_offline.mp4";
        std::remove("build/_offline.mp4");
        OfflineRenderer r; std::string err;
        if (!r.start(g, s, err)) { glfwTerminate(); return fail(("offline e2e: start: " + err).c_str()); }
        // The header's contract: step() renders AT LEAST one frame per call, so progress is
        // guaranteed even with a zero budget. A budget test made before the first frame (rather
        // than after) would stall here forever.
        for (long long want = 1; want <= 2; ++want) {
            if (!r.step(0.0)) { glfwTerminate(); return fail("offline e2e: a zero-budget step ended the job"); }
            if (r.progress().prerollDone != want) { glfwTerminate(); return fail("offline e2e: a zero-budget step must still render exactly one frame"); }
        }
        int steps = 0;
        while (r.step(0.02)) { if (++steps > 100000) { glfwTerminate(); return fail("offline e2e: never finished"); } }
        const OfflineRenderer::Progress& p = r.progress();
        if (p.phase != OfflineRenderer::Phase::Done) { glfwTerminate(); return fail(("offline e2e: " + p.status).c_str()); }
        if (p.framesDone != 60 || p.prerollDone != 30) { glfwTerminate(); return fail("offline e2e: progress counts (want 60 + 30 pre-roll)"); }
        if (!p.audio) { glfwTerminate(); return fail("offline e2e: expected an audio track (Sine -> Audio Out)"); }
        if (p.blackFrames != 0 || p.resizedAudioFrames != 0) { glfwTerminate(); return fail("offline e2e: unexpected black frames or resized audio blocks"); }
        if (p.status.rfind("rendered _offline.mp4 (60 frames, 2.0 s)", 0) != 0) { glfwTerminate(); return fail(("offline e2e: status line: " + p.status).c_str()); }
        if (rn->statusLine() != "saved build/_offline_live_rec.mp4") { glfwTerminate(); return fail("offline e2e: the live recording should have stopped + saved during the render"); }
        // finish() must run exactly once per job. A stray cancel() after a completed render must
        // not overwrite the outcome -- this is the assertion that makes cancel()'s active() guard
        // observable in the Done direction (Task 6 covers the never-started null-graph direction).
        r.cancel();
        if (r.progress().phase != OfflineRenderer::Phase::Done) { glfwTerminate(); return fail("offline e2e: a cancel() after the job finished overwrote the outcome"); }
        if (r.progress().framesDone != 60) { glfwTerminate(); return fail("offline e2e: a cancel() after the job finished disturbed the counts"); }

        // The file: exactly 60 frames, 160x120, 2-channel non-silent audio, the Colour at the centre.
        VideoDecoder dec; std::string derr;
        if (!dec.open("build/_offline.mp4", derr)) { glfwTerminate(); return fail(("offline e2e: open output: " + derr).c_str()); }
        if (dec.width() != 160 || dec.height() != 120) { glfwTerminate(); return fail("offline e2e: output size should be 160x120"); }
        if (!dec.hasAudio() || dec.audioChannels() != 2) { glfwTerminate(); return fail("offline e2e: output should have 2-channel audio"); }
        VideoFrame vf; std::vector<float> au; double aS = 0; bool aV = false;
        int frames = 0; bool nz = false; bool centreOk = false;
        while (dec.decodeFrame(vf, au, aS, aV)) {
            if (frames == 0) {
                std::size_t i = ((std::size_t)(vf.height / 2) * vf.width + vf.width / 2) * 4;
                // Colour default (255,128,25) through a lossy yuv420p round-trip: check loosely.
                centreOk = vf.rgba[i] > 200 && vf.rgba[i + 1] > 90 && vf.rgba[i + 1] < 170 && vf.rgba[i + 2] < 80;
            }
            ++frames;
            for (float v : au) if (v > 0.01f || v < -0.01f) { nz = true; break; }
            au.clear();
        }
        if (frames != 60) { std::fprintf(stderr, "got %d frames\n", frames); glfwTerminate(); return fail("offline e2e: expected exactly 60 frames in the file"); }
        if (!nz) { glfwTerminate(); return fail("offline e2e: encoded audio is silent"); }
        if (!centreOk) { glfwTerminate(); return fail("offline e2e: centre pixel is not the Colour"); }

        // State restored; the next live frame is back at the live size.
        const Transport& t = g.transport();
        if (!(t.seconds == 5.0 && t.looping && !t.playing && !t.externalClock)) { glfwTerminate(); return fail("offline e2e: transport not restored"); }
        if (g.offline()) { glfwTerminate(); return fail("offline e2e: graph still offline"); }
        g.evaluate(1.0f / 60.0f);
        if (on->current().w != 320 || on->current().h != 240) { glfwTerminate(); return fail("offline e2e: live texture size not restored"); }
        // The interrupted live recording must NOT restart on its own: `record` is still true, so
        // without the Recorder's re-arm latch this frame reopens the same path and truncates the
        // file the render just saved. Assert the saved file survives, byte size and all.
        if (rn->statusLine() != "saved build/_offline_live_rec.mp4") { glfwTerminate(); return fail("offline e2e: the live recorder restarted and overwrote its saved file"); }
        std::ifstream liveRec("build/_offline_live_rec.mp4", std::ios::binary | std::ios::ate);
        if (!liveRec.good() || liveRec.tellg() <= 0) { glfwTerminate(); return fail("offline e2e: the interrupted live recording was truncated"); }
        rn->inputDefault(3) = false; g.evaluate(1.0f / 60.0f);   // re-arm cleared; still not recording
        std::fprintf(stderr, "gl_smoke OK: offline render wrote 60 sample-locked 160x120 frames with stereo audio and restored state\n");
    }

    // --- Scenario: the capture blit must NOT flip vertically ---
    // The end-to-end scenario above renders a FLAT colour, which is flip-invariant: not one of
    // its assertions can tell an upright movie from an upside-down one. FBO textures are
    // bottom-up and VideoEncoder::addVideoFrame wants bottom-up rows (it flips for encoding),
    // so the capture blit must leave the rows alone -- unlike the Output window's blit, which
    // flips because it draws to a screen. Render a vertically asymmetric picture and check which
    // end of the decoded frame it lands on.
    {
        const int W = 64, H = 64;
        std::vector<unsigned char> px((std::size_t)W * H * 4);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                std::size_t i = ((std::size_t)y * W + x) * 4;
                unsigned char v = (y < H / 2) ? 255 : 0;    // stb_image_write's row 0 is the TOP row
                px[i] = px[i + 1] = px[i + 2] = v; px[i + 3] = 255;
            }
        const char* fixture = "gl_smoke_offline_orient.png";
        if (!stbi_write_png(fixture, W, H, 4, px.data(), W * 4)) { glfwTerminate(); return fail("offline flip: write fixture"); }

        Graph g;
        auto img = std::make_unique<ImageStreamerNode>(); img->initGL();
        img->inputDefault(0) = Value(std::string(fixture));
        auto out = std::make_unique<OutputNode>(); out->initGL();
        int iId = g.addNode(std::move(img));
        int oId = g.addNode(std::move(out));
        if (!g.connect(iId, 0, oId, 0)) { std::remove(fixture); glfwTerminate(); return fail("offline flip: connect"); }

        RenderSettings s; s.startBar = 0.0; s.endBar = 0.1; s.prerollBars = 0.0; s.fps = 30;
        s.width = 64; s.height = 64; s.outPath = "build/_offline_flip.mp4";
        std::remove(s.outPath.c_str());
        OfflineRenderer r; std::string err;
        if (!r.start(g, s, err)) { std::remove(fixture); glfwTerminate(); return fail(("offline flip: start: " + err).c_str()); }
        int guard = 0;
        while (r.step(0.05)) { if (++guard > 100000) { std::remove(fixture); glfwTerminate(); return fail("offline flip: never finished"); } }
        std::remove(fixture);
        if (r.progress().phase != OfflineRenderer::Phase::Done) { glfwTerminate(); return fail(("offline flip: " + r.progress().status).c_str()); }

        VideoDecoder dec; std::string derr;
        if (!dec.open(s.outPath, derr)) { glfwTerminate(); return fail(("offline flip: open output: " + derr).c_str()); }
        VideoFrame vf; std::vector<float> au; double aS = 0; bool aV = false;
        if (!dec.decodeFrame(vf, au, aS, aV)) { glfwTerminate(); return fail("offline flip: no frame decoded"); }
        // VideoFrame rows are bottom-up as well, so row 0 is the BOTTOM of the picture: the
        // white half must come out at the far end. A flipping blit puts it at row 0 instead.
        auto midRow = [&](int y) { return (int)vf.rgba[((std::size_t)y * vf.width + vf.width / 2) * 4]; };
        int bottom = midRow(2), top = midRow(vf.height - 3);
        if (!(top > 170 && bottom < 90)) {
            std::fprintf(stderr, "offline flip: bottom row = %d, top row = %d (want dark bottom, bright top)\n", bottom, top);
            glfwTerminate(); return fail("offline flip: the captured frame is upside down -- the capture blit must not flip");
        }
        std::fprintf(stderr, "gl_smoke OK: the offline capture blit preserves bottom-up row order\n");
    }

    // --- Scenario: every captured frame encodes exactly sampleRate/fps audio frames ---
    // The end-to-end scenario's Sine sizes its block with audioBlockFrames(), which at 48 kHz
    // divides exactly by every supported frame rate -- so its blocks are already the right
    // length and none of its assertions can tell a padding/trimming capture from one that
    // encodes the raw block. Drive the capture with a source that hands it the WRONG length in
    // both directions and check the encoded track's duration is set by the video clock anyway.
    {
        struct Case { int block; const char* path; } cases[] = {
            {  800, "build/_offline_audio_pad.mp4"  },   // half a frame's worth -> padded
            { 2400, "build/_offline_audio_trim.mp4" },   // 1.5x                 -> trimmed
        };
        for (const Case& c : cases) {
            Graph g;
            auto col = std::make_unique<ColourNode>(); col->initGL();
            auto out = std::make_unique<OutputNode>(); out->initGL();
            auto src = std::make_unique<MisSizedAudioNode>(c.block);
            auto ao  = std::make_unique<AudioOutputNode>();
            int cId = g.addNode(std::move(col)); int oId = g.addNode(std::move(out));
            int sId = g.addNode(std::move(src)); int aId = g.addNode(std::move(ao));
            if (!g.connect(cId, 0, oId, 0) || !g.connect(sId, 0, aId, 0)) { glfwTerminate(); return fail("offline audio lock: connect"); }

            RenderSettings s; s.startBar = 0.0; s.endBar = 0.25; s.prerollBars = 0.0; s.fps = 30;
            s.width = 64; s.height = 64; s.outPath = c.path;    // 0.25 bar at 120 bpm = 0.5 s = 15 frames
            std::remove(c.path);
            OfflineRenderer r; std::string err;
            if (!r.start(g, s, err)) { glfwTerminate(); return fail(("offline audio lock: start: " + err).c_str()); }
            int guard = 0;
            while (r.step(0.05)) { if (++guard > 100000) { glfwTerminate(); return fail("offline audio lock: never finished"); } }
            if (r.progress().phase != OfflineRenderer::Phase::Done) { glfwTerminate(); return fail(("offline audio lock: " + r.progress().status).c_str()); }
            if (r.progress().framesDone != 15) { glfwTerminate(); return fail("offline audio lock: expected 15 frames"); }
            // Every block was the wrong length, so every frame was resized -- and the outcome says so.
            if (r.progress().resizedAudioFrames != 15) { glfwTerminate(); return fail("offline audio lock: resizedAudioFrames should count every resized frame"); }
            if (r.progress().status.find("15 audio blocks resized") == std::string::npos) { glfwTerminate(); return fail("offline audio lock: the outcome line should name the resized blocks"); }

            VideoDecoder dec; std::string derr;
            if (!dec.open(c.path, derr)) { glfwTerminate(); return fail(("offline audio lock: open output: " + derr).c_str()); }
            VideoFrame vf; std::vector<float> au; double aS = 0; bool aV = false;
            std::size_t total = 0;
            while (dec.decodeFrame(vf, au, aS, aV)) { total += au.size(); au.clear(); }   // 48 kHz MONO
            // 15 frames * 48000/30 = 24000 samples of audio, whatever the source's block size.
            // Encoding the raw block instead gives 12000 (pad case) or 36000 (trim case), so the
            // window below is wide enough for AAC's priming/padding and the tail the decoder
            // stops short of, yet nowhere near either drifted value.
            std::fprintf(stderr, "[audio lock] %s: %zu mono samples decoded (want ~24000)\n", c.path, total);
            if (total < 20000 || total > 28000) { glfwTerminate(); return fail("offline audio lock: the audio clock drifted from the video clock"); }
        }
        std::fprintf(stderr, "gl_smoke OK: the offline capture pads/trims every frame's audio to sampleRate/fps, so the audio clock cannot drift\n");
    }

    // --- Scenario: the sinks are re-resolved by id, and a vanished Output fails through finish() ---
    // capture() looks the Output/Audio Out nodes up through Graph::findNode every frame instead
    // of caching a Node*, because Graph::clear() (a project load) frees every node mid-render.
    // Clearing the graph between steps is what tells a re-resolving capture() from one holding a
    // stale pointer, and the failure must still run finish(): a skipped finish() leaves the
    // graph's offline flag stuck true, silently muting Audio Out, MIDI Out and the Recorder.
    {
        Graph g;
        auto col = std::make_unique<ColourNode>(); col->initGL();
        auto out = std::make_unique<OutputNode>(); out->initGL();
        int cId = g.addNode(std::move(col));
        int oId = g.addNode(std::move(out));
        if (!g.connect(cId, 0, oId, 0)) { glfwTerminate(); return fail("offline vanish: connect"); }
        Preferences live; live.textureWidth = 320; live.textureHeight = 240;
        g.setPreferences(&live);

        RenderSettings s; s.startBar = 0.0; s.endBar = 1.0; s.prerollBars = 0.0; s.fps = 30;
        s.width = 64; s.height = 64; s.outPath = "build/_offline_vanish.mp4";
        std::remove(s.outPath.c_str());
        OfflineRenderer r; std::string err;
        if (!r.start(g, s, err)) { glfwTerminate(); return fail(("offline vanish: start: " + err).c_str()); }
        if (!r.step(0.0) || r.progress().prerollDone != 1) { glfwTerminate(); return fail("offline vanish: expected the forced pre-roll frame first"); }
        if (!r.step(0.0) || r.progress().framesDone != 1) { glfwTerminate(); return fail("offline vanish: expected one captured frame before the graph is cleared"); }

        g.clear();                                   // the nodes capture() was reading are now gone
        if (r.step(0.0)) { glfwTerminate(); return fail("offline vanish: step should have failed once the Output node was freed"); }
        if (r.progress().phase != OfflineRenderer::Phase::Failed) { glfwTerminate(); return fail("offline vanish: phase should be Failed"); }
        if (r.progress().status != "the Output node disappeared mid-render") { glfwTerminate(); return fail(("offline vanish: wrong status: " + r.progress().status).c_str()); }
        if (g.offline()) { glfwTerminate(); return fail("offline vanish: the failure path must still clear the offline flag"); }
        if (g.preferences() != &live) { glfwTerminate(); return fail("offline vanish: the failure path must still restore the live preferences"); }
        // The encoder was open with one frame in it; finish() closed it, so the partial file plays.
        VideoDecoder dec; std::string derr;
        if (!dec.open(s.outPath, derr)) { glfwTerminate(); return fail(("offline vanish: the partial file should still be finalised: " + derr).c_str()); }
        std::fprintf(stderr, "gl_smoke OK: the offline capture re-resolves its sinks by id and a vanished Output fails through finish()\n");
    }

    // --- Scenario: the fixed clock places every frame, pre-roll included ---
    // The feature's central claim, and the one thing the other scenarios all ASSUME: they render
    // from bar 0, where dropping the start offset entirely -- the likeliest clock bug, and the one
    // that makes a render begin at the wrong musical position -- is invisible. Rendering from
    // bar 2 makes the offset load-bearing, and recording the whole clock catches an accumulated
    // dt, a dt taken from the step budget, and a pre-roll with the sign backwards.
    {
        Graph g;
        auto col   = std::make_unique<ColourNode>(); col->initGL();
        auto out   = std::make_unique<OutputNode>(); out->initGL();
        auto probe = std::make_unique<ClockProbeNode>();
        int cId = g.addNode(std::move(col));
        int oId = g.addNode(std::move(out));
        int pId = g.addNode(std::move(probe));
        if (!g.connect(cId, 0, oId, 0)) { glfwTerminate(); return fail("offline clock: connect"); }
        g.transport().bpm = 120.0;                     // 4/4 at 120 bpm -> 2 s per bar

        RenderSettings s; s.startBar = 2.0; s.endBar = 2.5; s.prerollBars = 0.25; s.fps = 30;
        s.width = 64; s.height = 64; s.outPath = "build/_offline_clock.mp4";   // 15 pre-roll + 30 captured
        std::remove(s.outPath.c_str());
        OfflineRenderer r; std::string err;
        if (!r.start(g, s, err)) { glfwTerminate(); return fail(("offline clock: start: " + err).c_str()); }
        int guard = 0;
        while (r.step(0.05)) { if (++guard > 100000) { glfwTerminate(); return fail("offline clock: never finished"); } }
        if (r.progress().phase != OfflineRenderer::Phase::Done) { glfwTerminate(); return fail(("offline clock: " + r.progress().status).c_str()); }

        auto* pr = dynamic_cast<ClockProbeNode*>(g.findNode(pId));
        if (!pr) { glfwTerminate(); return fail("offline clock: probe node missing"); }
        if (pr->secs.size() != 45) { std::fprintf(stderr, "offline clock: %zu evaluates\n", pr->secs.size()); glfwTerminate(); return fail("offline clock: wrong evaluate count"); }
        for (std::size_t i = 0; i < pr->secs.size(); ++i) {
            double want = 4.0 + ((double)i - 15.0) / 30.0;   // bar 2 == 4.0 s; frame 0 lands exactly there
            // EXACT, not a tolerance. renderFrameSeconds computes startBar*secondsPerBar + k/fps,
            // and `want` is that same expression over the same exactly-representable values
            // (2.0*2.0 == 4.0, k/30.0), so the two are bit-identical. This is the assertion that
            // separates PLACING each frame from ACCUMULATING dt: over these 45 frames accumulation
            // drifts by ~1e-15, which no sane epsilon would catch, yet it grows without bound over
            // a long render. Measured: a 1e-9 tolerance passes an accumulating implementation.
            if (pr->secs[i] != want) {
                std::fprintf(stderr, "offline clock: evaluate %zu at %.9f s, want %.9f s\n", i, pr->secs[i], want);
                glfwTerminate(); return fail("offline clock: frame placed wrong");
            }
            if (pr->dts[i] != 1.0f / 30.0f) { glfwTerminate(); return fail("offline clock: dt is not 1/fps"); }
            if (!pr->ext[i]) { glfwTerminate(); return fail("offline clock: transport not armed as an external clock"); }
        }
        std::fprintf(stderr, "gl_smoke OK: the offline fixed clock places all 45 frames (15 pre-roll + 30) from bar 2 at exactly k/fps\n");
    }

    // --- Scenario: a render range that is empty at this tempo is rejected, not "completed" ---
    // renderFrameCount returns 0 for a frame count that overflows (endBar 1e18 is finite and
    // passes every validateRenderSettings rule) and for a zero-length bar. start() must reject
    // that rather than let step() finish(Done) with no encoder ever opened -- a success with no
    // file -- and the reject has to land BEFORE the graph is put offline, or it strands it there.
    {
        Graph g;
        auto col = std::make_unique<ColourNode>(); col->initGL();
        auto out = std::make_unique<OutputNode>(); out->initGL();
        int cId = g.addNode(std::move(col));
        int oId = g.addNode(std::move(out));
        if (!g.connect(cId, 0, oId, 0)) { glfwTerminate(); return fail("offline empty range: connect"); }
        Preferences live; live.textureWidth = 320; live.textureHeight = 240;
        g.setPreferences(&live);
        g.transport().bpm = 120.0; g.transport().seconds = 5.0;

        RenderSettings s; s.startBar = 0.0; s.endBar = 1e18; s.prerollBars = 0.0; s.fps = 30;
        s.width = 64; s.height = 64; s.outPath = "build/_offline_empty.mp4";
        OfflineRenderer r; std::string err;
        if (r.start(g, s, err)) { glfwTerminate(); return fail("offline empty range: an overflowing range should be refused"); }
        if (err != "the render range is empty or too long at this tempo") { glfwTerminate(); return fail(("offline empty range: wrong error: " + err).c_str()); }
        // The reject must leave the graph exactly as it found it -- this is what makes the
        // ordering in start() (counts before the offline flag and the prefs swap) observable.
        if (g.offline()) { glfwTerminate(); return fail("offline empty range: a rejected start left the graph offline"); }
        if (g.preferences() != &live) { glfwTerminate(); return fail("offline empty range: a rejected start swapped the preferences"); }
        if (g.transport().externalClock || g.transport().playing || g.transport().seconds != 5.0)
            { glfwTerminate(); return fail("offline empty range: a rejected start armed the transport"); }

        s.endBar = 8.0; g.transport().beatsPerBar = 0;   // a hand-edited project: no seconds in a bar
        if (r.start(g, s, err)) { glfwTerminate(); return fail("offline empty range: a zero-length bar should be refused"); }
        if (err != "the render range is empty or too long at this tempo") { glfwTerminate(); return fail("offline empty range: wrong error for a zero-length bar"); }
        if (g.offline()) { glfwTerminate(); return fail("offline empty range: a zero-tempo reject left the graph offline"); }
        std::fprintf(stderr, "gl_smoke OK: an empty render range is rejected without disturbing the graph\n");
    }

    // --- Scenario: a destination that cannot be written is rejected by start(), and one that
    //     goes bad AFTER start() still fails the render (exit 1), never Done ---
    // The CLI's exit code is `phase == Done ? 0 : 1`, so this is the whole signal a batch
    // pipeline gets. start() now probes the destination up front, because openEncoder() runs at
    // the FIRST CAPTURED frame -- the whole pre-roll (1.5 s for a trivial graph at 1920x1080
    // with a 1-bar 60 fps pre-roll, minutes for a heavy one) used to render before a typo in the
    // path was reported. The late check stays and is what (b) below covers: the probe is an
    // optimisation, and a path can always go bad between the two.
    {
        Graph g;
        auto col = std::make_unique<ColourNode>(); col->initGL();
        auto out = std::make_unique<OutputNode>(); out->initGL();
        int cId = g.addNode(std::move(col));
        int oId = g.addNode(std::move(out));
        if (!g.connect(cId, 0, oId, 0)) { glfwTerminate(); return fail("offline enc fail: connect"); }
        Preferences live; live.textureWidth = 320; live.textureHeight = 240;
        g.setPreferences(&live);

        RenderSettings s; s.startBar = 0.0; s.endBar = 1.0; s.prerollBars = 0.0; s.fps = 30;
        s.width = 64; s.height = 64;

        // (a) Up front: a missing directory, and an extension no muxer claims. Both must leave
        //     the graph untouched -- a reject after the offline flag was set would strand it.
        {
            s.outPath = "build/_no_such_dir_offline/out.mp4";   // the directory does not exist
            OfflineRenderer r; std::string err;
            if (r.start(g, s, err)) { glfwTerminate(); return fail("offline enc fail: a path in a missing directory should be rejected before rendering"); }
            if (err.rfind("cannot write ", 0) != 0) { glfwTerminate(); return fail(("offline enc fail: wrong error for a missing directory: " + err).c_str()); }
            if (g.offline() || g.transport().externalClock) { glfwTerminate(); return fail("offline enc fail: a rejected start left the graph armed"); }
            if (g.preferences() != &live) { glfwTerminate(); return fail("offline enc fail: a rejected start swapped the preferences"); }

            s.outPath = "build/_offline_probe.xyzzy";           // no muxer for this extension
            std::remove(s.outPath.c_str());
            if (r.start(g, s, err)) { glfwTerminate(); return fail("offline enc fail: an unmuxable extension should be rejected before rendering"); }
            if (err.rfind("no video format matches ", 0) != 0) { glfwTerminate(); return fail(("offline enc fail: wrong error for a bad extension: " + err).c_str()); }
            // The probe must not leave a file behind for a path it only tested.
            if (std::ifstream(s.outPath).good()) { glfwTerminate(); return fail("offline enc fail: the destination probe left a file behind"); }
        }

        // (b) After start(): the path turns into a DIRECTORY between the probe and the first
        //     captured frame, so openEncoder() is the one that has to catch it. This is the late
        //     route the probe deliberately does not replace.
        {
            s.outPath = "build/_offline_enc_fail_late.mp4";
            std::remove(s.outPath.c_str());
            std::filesystem::remove_all(s.outPath);
            OfflineRenderer r; std::string err;
            if (!r.start(g, s, err)) { glfwTerminate(); return fail(("offline enc fail: start: " + err).c_str()); }
            std::error_code ec;
            std::filesystem::create_directory(s.outPath, ec);   // now nothing can open it for writing
            if (ec) { glfwTerminate(); return fail("offline enc fail: could not stage the late failure"); }
            int guard = 0;
            while (r.step(0.05)) { if (++guard > 100000) { glfwTerminate(); return fail("offline enc fail: never finished"); } }
            std::filesystem::remove_all(s.outPath, ec);
            if (r.progress().phase != OfflineRenderer::Phase::Failed) { glfwTerminate(); return fail("offline enc fail: an unopenable encoder must fail the render, not complete it"); }
            if (r.progress().status.rfind("could not open ", 0) != 0) { glfwTerminate(); return fail(("offline enc fail: wrong status: " + r.progress().status).c_str()); }
            if (g.offline()) { glfwTerminate(); return fail("offline enc fail: the failure must still clear the offline flag"); }
        }
        std::fprintf(stderr, "gl_smoke OK: an unwritable destination is rejected before the pre-roll, and one that goes bad after start still fails the render\n");
    }

    // --- Scenario: the loader gate yields without losing frames; the load timeout fails naming
    //     the node; cancel mid-render keeps a partial, playable file ---
    // The gate + timeout in step() are currently untested code: nothing else in this file drives
    // a node that reports loading().
    {
        // busy flips the gate directly rather than counting down polls, so the scenario makes no
        // assumption about how many times loading() gets called per step() -- a detail of both
        // anyNodeLoading()'s short-circuit and step()'s loop structure that the test has no
        // business depending on. evals counts evaluate() calls, which must stay zero for as long
        // as the gate holds: a step that evaluates the graph while reporting no captured frame
        // would still pass a framesDone-only check, so this is what actually pins the yield down
        // as a REAL yield (no graph work at all), not just "no frame captured".
        struct SlowLoader : Node {
            bool busy = true;
            SlowLoader() : Node("Slow Loader") {}
            void evaluate(EvalContext&) override { ++evals; }
            bool loading() const override { return busy; }
            int evals = 0;
        };
        auto build = [](Graph& g, bool busy, int& slowId) {
            auto col = std::make_unique<ColourNode>(); col->initGL();
            auto out = std::make_unique<OutputNode>(); out->initGL();
            int cId = g.addNode(std::move(col)); int oId = g.addNode(std::move(out));
            auto slow = std::make_unique<SlowLoader>();
            slow->busy = busy;
            slowId = g.addNode(std::move(slow));
            g.connect(cId, 0, oId, 0);
            g.transport().bpm = 120.0;
        };
        RenderSettings s; s.startBar = 0.0; s.endBar = 1.0; s.prerollBars = 0.0; s.fps = 30;
        s.width = 160; s.height = 120;

        // (a) Gate: 5 yielding steps that touch nothing at all -- no captured frame AND no
        //     evaluate() call on any node, including the Slow Loader itself -- each one a REAL
        //     yield, not a busy spin: called with budget 0, it still returns immediately, which is
        //     what lets a CLI loop sleep on waitingForLoad instead of burning CPU until the
        //     timeout. Clearing the gate then renders exactly one frame and waitingForLoad drops
        //     before the job otherwise finishes, proving the flag tracks the wait, not just the
        //     job's outcome. The render then completes normally: 1 bar at 120 bpm = 2 s at 30 fps
        //     = 60 frames.
        {
            Graph g; int slowId = 0; build(g, true, slowId);
            auto* slow = dynamic_cast<SlowLoader*>(g.findNode(slowId));
            if (!slow) { glfwTerminate(); return fail("offline gate: Slow Loader node missing"); }
            const std::string waitStatus = "waiting for Slow Loader #" + std::to_string(slowId);
            s.outPath = "build/_offline_gate.mp4"; std::remove(s.outPath.c_str());
            OfflineRenderer r; std::string err;
            if (!r.start(g, s, err)) { glfwTerminate(); return fail(("offline gate: start: " + err).c_str()); }
            for (int i = 0; i < 5; ++i) {
                if (!r.step(0.0)) { glfwTerminate(); return fail("offline gate: step should stay active while waiting"); }
                if (r.progress().framesDone != 0) { glfwTerminate(); return fail("offline gate: rendered a frame while a node was loading"); }
                if (!r.progress().waitingForLoad) { glfwTerminate(); return fail("offline gate: waitingForLoad should be true while gated"); }
                if (r.progress().status != waitStatus) { glfwTerminate(); return fail(("offline gate: status: " + r.progress().status).c_str()); }
            }
            if (slow->evals != 0) { glfwTerminate(); return fail("offline gate: a gated step must not evaluate the graph at all"); }
            slow->busy = false;   // clear the gate
            // The settings ask for no pre-roll, but start() always burns one frame (an async load
            // only STARTS on a node's first evaluate, so the gate means nothing before one has
            // run) -- so the first ungated step renders THAT frame, and the next captures frame 0.
            if (!r.step(0.0)) { glfwTerminate(); return fail("offline gate: step should stay active once the gate clears"); }
            if (r.progress().prerollDone != 1 || r.progress().framesDone != 0) { glfwTerminate(); return fail("offline gate: the first ungated step should render the forced pre-roll frame"); }
            if (!r.step(0.0)) { glfwTerminate(); return fail("offline gate: step should stay active after the pre-roll frame"); }
            if (r.progress().framesDone != 1) { glfwTerminate(); return fail("offline gate: the first ungated step after the pre-roll should capture exactly one frame"); }
            if (r.progress().waitingForLoad) { glfwTerminate(); return fail("offline gate: waitingForLoad should clear as soon as rendering resumes"); }
            int steps = 0;
            while (r.step(0.02)) { if (++steps > 100000) { glfwTerminate(); return fail("offline gate: never finished"); } }
            if (r.progress().phase != OfflineRenderer::Phase::Done || r.progress().framesDone != 60) { glfwTerminate(); return fail("offline gate: should finish with all 60 frames"); }
            if (r.progress().waitingForLoad) { glfwTerminate(); return fail("offline gate: a finished job should not report waitingForLoad"); }
            VideoDecoder dec; std::string derr;
            if (!dec.open(s.outPath, derr)) { glfwTerminate(); return fail("offline gate: output did not open"); }
            VideoFrame vf; std::vector<float> au; double aS = 0; bool aV = false; int frames = 0;
            while (dec.decodeFrame(vf, au, aS, aV)) { ++frames; au.clear(); }
            if (frames != 60) { glfwTerminate(); return fail("offline gate: file should hold exactly 60 frames"); }
        }

        // (b) Timeout: a node that never finishes loading fails the job, naming the node. This is
        //     the behaviour a plain `while (r.step()) {}` CLI loop depends on to ever give up --
        //     the graphical path is vsync-limited so a stuck gate does not show there.
        {
            Graph g; int slowId = 0; build(g, true, slowId);   // busy forever
            const std::string timeoutStatus = "timed out waiting for Slow Loader #" + std::to_string(slowId) + " to load";
            s.outPath = "build/_offline_timeout.mp4"; std::remove(s.outPath.c_str());
            OfflineRenderer r; std::string err;
            r.setLoadTimeoutSeconds(0.05);
            if (!r.start(g, s, err)) { glfwTerminate(); return fail(("offline timeout: start: " + err).c_str()); }
            int steps = 0;
            while (r.step(0.0)) {
                if (!r.progress().waitingForLoad) { glfwTerminate(); return fail("offline timeout: waitingForLoad should be true throughout the wait"); }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (++steps > 1000) { glfwTerminate(); return fail("offline timeout: never gave up"); }
            }
            if (r.progress().phase != OfflineRenderer::Phase::Failed) { glfwTerminate(); return fail("offline timeout: phase should be Failed"); }
            if (r.progress().status != timeoutStatus) { glfwTerminate(); return fail(("offline timeout: status: " + r.progress().status).c_str()); }
            if (r.progress().waitingForLoad) { glfwTerminate(); return fail("offline timeout: a failed job should not report waitingForLoad"); }
            if (r.progress().framesDone != 0) { glfwTerminate(); return fail("offline timeout: a load that never finished should never have rendered a frame"); }
            if (g.offline() || g.transport().externalClock) { glfwTerminate(); return fail("offline timeout: state not restored"); }
        }

        // (c) Cancel mid-render: a playable partial file, state restored.
        //
        // step(0.0) renders exactly one frame per call (asserted below) -- but the encoder is
        // opened with max_b_frames = 1, and a container holding EXACTLY one B-delayed frame hits
        // a real edit-list quirk: ffmpeg's own mov demuxer computes an edit list that drops that
        // single sample as being before the presentation start, so a one-frame file decodes to
        // ZERO frames (confirmed independently with plain ffmpeg -i ... -f null -, not just this
        // repo's VideoDecoder -- verbose output: "drop a frame at curr_cts: 0 @ 0"; also confirmed
        // this is not exclusive to one frame -- 4 in / 3 out, 10 in / 9 out -- while 2, 3, 5, 6 and
        // 60 round-trip exactly). That is a pre-existing VideoEncoder/mp4-muxing property, out of
        // scope for this test-only task, so rather than pin down a decode failure that has nothing
        // to do with OfflineRenderer, this cancels one frame later -- after two captured frames,
        // confirmed decodable -- which still exercises a real mid-render cancel while keeping the
        // "playable partial file" claim true.
        {
            Graph g; int slowId = 0; build(g, false, slowId);   // never loading
            g.transport().seconds = 3.0;
            s.outPath = "build/_offline_cancel_partial.mp4"; std::remove(s.outPath.c_str());
            OfflineRenderer r; std::string err;
            if (!r.start(g, s, err)) { glfwTerminate(); return fail(("offline cancel: start: " + err).c_str()); }
            r.step(0.0);                                                   // the forced pre-roll frame
            if (r.progress().prerollDone != 1 || r.progress().framesDone != 0) { glfwTerminate(); return fail("offline cancel: the first step(0) should render the forced pre-roll frame"); }
            r.step(0.0);                                                   // exactly one frame
            if (r.progress().framesDone != 1) { glfwTerminate(); return fail("offline cancel: step(0) should render exactly one frame"); }
            r.step(0.0);                                                   // exactly one more frame
            if (r.progress().framesDone != 2) { glfwTerminate(); return fail("offline cancel: a second step(0) should render exactly one more frame"); }
            r.cancel();
            if (r.active() || r.progress().phase != OfflineRenderer::Phase::Cancelled) { glfwTerminate(); return fail("offline cancel: should be Cancelled"); }
            if (r.progress().status != "cancelled after 2 frames") { glfwTerminate(); return fail(("offline cancel: status: " + r.progress().status).c_str()); }
            VideoDecoder dec; std::string derr;
            if (!dec.open(s.outPath, derr)) { glfwTerminate(); return fail("offline cancel: partial file should open"); }
            VideoFrame vf; std::vector<float> au; double aS = 0; bool aV = false; int frames = 0;
            while (dec.decodeFrame(vf, au, aS, aV)) { ++frames; au.clear(); }
            // Two encoded frames decode to either 1 or 2 depending on the edit-list quirk above --
            // never 0, and structurally never anywhere near 60 after two zero-budget steps.
            if (frames < 1 || frames > 2) { glfwTerminate(); return fail("offline cancel: partial file frame count"); }
            if (g.transport().seconds != 3.0 || g.transport().playing || g.transport().externalClock) { glfwTerminate(); return fail("offline cancel: transport not restored"); }
        }
        std::fprintf(stderr, "gl_smoke OK: offline loader gate yields without losing frames, times out by name, and cancel keeps a partial file\n");
    }

    // --- Scenario: a load that only STARTS on the first evaluate() still gates a render asked
    //     for with no pre-roll, and its audio reaches the file ---
    // The gate scenario above sets busy BEFORE start(), which is not how a real AsyncLoader-backed
    // node behaves: AudioPlayerNode calls loader_.request() inside evaluate(), so loading() is
    // false until a frame has run. step() checks the gate BEFORE evaluating, so with a pre-roll of
    // 0 the gate was a no-op on frame 0 -- and frame 0 is the frame openEncoder() LATCHES the
    // audio track from. The whole render came out video only (exit 0, wrong file, reproduced with
    // `--preroll 0` on an Audio File -> Audio Out project) while the gate sat waiting for a load
    // whose result could no longer be used. start() burns one pre-roll frame however few bars are
    // asked for, so the gate always has a started load to see.
    {
        struct LateLoader : Node {
            LateLoader() : Node("Late Loader") {
                addOutput("left",  PortType::Audio);
                addOutput("right", PortType::Audio);
                buf_.resize(1600);                       // exactly 48000/30: nothing to pad or trim
                for (std::size_t i = 0; i < buf_.size(); ++i)
                    buf_[i] = 0.5f * (float)std::sin(6.283185307179586 * 440.0 * (double)i / 48000.0);
            }
            void evaluate(EvalContext& ctx) override {
                started = true;                          // the decode kicks off HERE, not before
                AudioRef a = busy ? AudioRef{} : AudioRef{buf_.data(), buf_.size(), 48000};
                ctx.out<AudioRef>(0, a);
                ctx.out<AudioRef>(1, a);
            }
            bool loading() const override { return started && busy; }
            bool started = false, busy = true;
            std::vector<float> buf_;
        };

        Graph g;
        auto col  = std::make_unique<ColourNode>(); col->initGL();
        auto out  = std::make_unique<OutputNode>(); out->initGL();
        auto late = std::make_unique<LateLoader>();
        auto ao   = std::make_unique<AudioOutputNode>();
        int cId = g.addNode(std::move(col));  int oId = g.addNode(std::move(out));
        int lId = g.addNode(std::move(late)); int aId = g.addNode(std::move(ao));
        if (!g.connect(cId, 0, oId, 0) || !g.connect(lId, 0, aId, 0) || !g.connect(lId, 1, aId, 1))
            { glfwTerminate(); return fail("offline late load: connect"); }
        g.transport().bpm = 120.0;

        RenderSettings s; s.startBar = 0.0; s.endBar = 0.25; s.prerollBars = 0.0; s.fps = 30;
        s.width = 64; s.height = 64; s.outPath = "build/_offline_late_load.mp4";   // 0.25 bar = 15 frames
        std::remove(s.outPath.c_str());
        OfflineRenderer r; std::string err;
        if (!r.start(g, s, err)) { glfwTerminate(); return fail(("offline late load: start: " + err).c_str()); }
        if (r.progress().prerollTotal != 1) { glfwTerminate(); return fail("offline late load: a 0-bar pre-roll must still burn exactly one frame"); }
        auto* lp = dynamic_cast<LateLoader*>(g.findNode(lId));
        if (!lp) { glfwTerminate(); return fail("offline late load: Late Loader node missing"); }
        if (lp->started) { glfwTerminate(); return fail("offline late load: nothing should have evaluated yet"); }

        if (!r.step(0.0)) { glfwTerminate(); return fail("offline late load: step should stay active after the pre-roll frame"); }
        if (!lp->started) { glfwTerminate(); return fail("offline late load: the burned pre-roll frame should have started the load"); }
        if (r.progress().framesDone != 0) { glfwTerminate(); return fail("offline late load: a frame was captured before the loader had even started"); }
        for (int i = 0; i < 3; ++i) {
            if (!r.step(0.0)) { glfwTerminate(); return fail("offline late load: step should stay active while the load is in flight"); }
            if (!r.progress().waitingForLoad) { glfwTerminate(); return fail("offline late load: the gate must engage once the load has started"); }
            if (r.progress().framesDone != 0) { glfwTerminate(); return fail("offline late load: a frame was captured while the load was in flight"); }
        }
        lp->busy = false;                                  // the decode lands
        int steps = 0;
        while (r.step(0.02)) { if (++steps > 100000) { glfwTerminate(); return fail("offline late load: never finished"); } }
        const OfflineRenderer::Progress& p = r.progress();
        if (p.phase != OfflineRenderer::Phase::Done) { glfwTerminate(); return fail(("offline late load: " + p.status).c_str()); }
        if (p.framesDone != 15) { glfwTerminate(); return fail("offline late load: expected 15 captured frames"); }
        // The point of the whole scenario: the track is there, and the render is not degraded.
        if (!p.audio) { glfwTerminate(); return fail("offline late load: the audio track was latched before the load finished -- the file is video only"); }
        if (p.status.find("video only") != std::string::npos) { glfwTerminate(); return fail(("offline late load: " + p.status).c_str()); }
        if (p.blackFrames != 0 || p.resizedAudioFrames != 0) { glfwTerminate(); return fail("offline late load: unexpected black frames or resized audio blocks"); }

        VideoDecoder dec; std::string derr;
        if (!dec.open(s.outPath, derr)) { glfwTerminate(); return fail(("offline late load: open output: " + derr).c_str()); }
        if (!dec.hasAudio() || dec.audioChannels() != 2) { glfwTerminate(); return fail("offline late load: the file should carry a 2-channel audio track"); }
        VideoFrame vf; std::vector<float> au; double aS = 0; bool aV = false;
        int frames = 0; bool nz = false;
        while (dec.decodeFrame(vf, au, aS, aV)) {
            ++frames;
            for (float v : au) if (v > 0.01f || v < -0.01f) { nz = true; break; }
            au.clear();
        }
        if (frames != 15) { glfwTerminate(); return fail("offline late load: the file should hold exactly 15 frames"); }
        if (!nz) { glfwTerminate(); return fail("offline late load: the encoded audio is silent"); }
        std::fprintf(stderr, "gl_smoke OK: a load that starts on the first evaluate still gates a pre-roll-free render and its audio reaches the file\n");
    }

#ifndef _WIN32
    // --- Scenario: a write failure mid-encode fails the render (the ENOSPC case) ---
    // The realistic encode failure is running out of disk, and it is the one that used to be
    // swallowed at four separate call sites -- so a batch pipeline got exit 0 and a truncated
    // file. RLIMIT_FSIZE is the portable stand-in: with SIGXFSZ ignored, a write past the limit
    // returns EFBIG exactly as a full disk returns ENOSPC. This scenario pins the failure to the
    // close() route, because that is the one that exercises finish()'s Done->Failed downgrade --
    // a branch that had never once executed. That needs a TWO-SIDED constraint on the output, and
    // both sides are held by the render being only 0.5 s long:
    //   lower -- the output must EXCEED the limit, or nothing fails. Measured 2003 bytes with
    //            libx264; the limit below is 512, about 4x under it.
    //   upper -- the output must stay UNDER FFmpeg's ~32 KiB AVIOContext buffer, or it flushes
    //            mid-render and fails at a frame instead. This is the side that a longer render
    //            quietly breaks: with no libx264, VideoEncoder::open falls back to MPEG-4 and
    //            never sets bit_rate, so the ~200 kbps default applies -- ~12.5 KB over 0.5 s
    //            (still under the buffer), but ~200 KB over the 8 s this scenario first used,
    //            which would flush and fail the assertion with a misleading message.
    // The mid-render write route is covered by its own scenario below, not by relaxing this one.
    {
        struct rlimit oldLim{};
        if (getrlimit(RLIMIT_FSIZE, &oldLim) != 0) { glfwTerminate(); return fail("offline enc write fail: getrlimit"); }
        void (*oldXfsz)(int) = std::signal(SIGXFSZ, SIG_IGN);   // else the process dies on the first over-limit write
        // 2 KiB against a ~8 KiB output: a 4x margin, so an x264 that compresses this flat colour
        // rather better or worse than the one measured still overshoots. Creating the file writes
        // nothing, so the limit cannot turn this into an open failure instead.
        struct rlimit lim = oldLim; lim.rlim_cur = 512;
        if (setrlimit(RLIMIT_FSIZE, &lim) != 0) {
            std::signal(SIGXFSZ, oldXfsz); glfwTerminate(); return fail("offline enc write fail: setrlimit");
        }

        Graph g;
        auto col = std::make_unique<ColourNode>(); col->initGL();
        auto out = std::make_unique<OutputNode>(); out->initGL();
        int cId = g.addNode(std::move(col));
        int oId = g.addNode(std::move(out));
        bool wired = g.connect(cId, 0, oId, 0);

        RenderSettings s; s.startBar = 0.0; s.endBar = 0.25; s.prerollBars = 0.0; s.fps = 30;
        s.width = 320; s.height = 240; s.outPath = "build/_offline_enospc.mp4";
        std::remove(s.outPath.c_str());
        OfflineRenderer r; std::string err;
        bool started = wired && r.start(g, s, err);
        int guard = 0; bool spun = true;
        while (started && r.step(0.05)) { if (++guard > 100000) { spun = false; break; } }
        OfflineRenderer::Progress p = r.progress();      // copy before the limit is lifted
        bool stillOffline = g.offline();

        // The same bug at the second call site: RecorderNode::stop() used to discard close()'s
        // bool outright, so an inline recording that never got its trailer still said "saved"
        // and the user closed the app believing the take was on disk.
        std::string recStatus = "(not run)";
        {
            Graph g2;
            Preferences small; small.textureWidth = 320; small.textureHeight = 240;
            g2.setPreferences(&small);
            auto col2 = std::make_unique<ColourNode>(); col2->initGL();
            auto rec2 = std::make_unique<RecorderNode>();
            rec2->inputDefault(3) = true;
            rec2->inputDefault(4) = std::string("build/_rec_enospc.mp4");
            auto out2 = std::make_unique<OutputNode>(); out2->initGL();
            int c2 = g2.addNode(std::move(col2)); int r2 = g2.addNode(std::move(rec2));
            int o2 = g2.addNode(std::move(out2));
            if (g2.connect(c2, 0, r2, 0) && g2.connect(r2, 0, o2, 0)) {
                for (int i = 0; i < 240; ++i) g2.evaluate(1.0f / 30.0f);
                auto* rn2 = dynamic_cast<RecorderNode*>(g2.findNode(r2));
                rn2->inputDefault(3) = false;
                g2.evaluate(1.0f / 30.0f);               // toggles record off -> stop() -> close() fails
                recStatus = rn2->statusLine();
            }
        }

        bool restored = setrlimit(RLIMIT_FSIZE, &oldLim) == 0;   // BEFORE any further file write
        bool sigBack  = std::signal(SIGXFSZ, oldXfsz) != SIG_ERR;
        // stderr is POISONED at this point: its own writes hit EFBIG too, which latches
        // ferror(stderr) so every later fprintf returns -1 and writes NOTHING -- even now the
        // limit is lifted (macOS libc; glibc retries). Without this, running gl_smoke with
        // stderr on a regular file silently loses the rest of the log, including any later
        // gl_smoke FAIL line. ctest pipes stderr, so CI never showed it.
        clearerr(stderr);

        if (!restored) { glfwTerminate(); return fail("offline enc write fail: could not restore RLIMIT_FSIZE -- every later scenario would fail confusingly"); }
        if (!sigBack)  { glfwTerminate(); return fail("offline enc write fail: could not restore the SIGXFSZ handler"); }
        if (!wired)   { glfwTerminate(); return fail("offline enc write fail: connect"); }
        if (!started) { glfwTerminate(); return fail(("offline enc write fail: start: " + err).c_str()); }
        if (!spun)    { glfwTerminate(); return fail("offline enc write fail: never finished"); }
        std::fprintf(stderr, "[enospc] phase=%d status=%s\n", (int)p.phase, p.status.c_str());
        if (p.phase == OfflineRenderer::Phase::Done) { glfwTerminate(); return fail("offline enc write fail: a truncated file was reported as a completed render"); }
        if (p.phase != OfflineRenderer::Phase::Failed) { glfwTerminate(); return fail("offline enc write fail: expected Failed"); }
        if (stillOffline) { glfwTerminate(); return fail("offline enc write fail: the failure must still clear the offline flag"); }
        // Specifically the close() route -- an encode failure at a frame would mean the output
        // outgrew the avio buffer and this scenario stopped covering the Done->Failed downgrade.
        if (p.status.rfind("could not finalise ", 0) != 0) { glfwTerminate(); return fail(("offline enc write fail: expected a close()-route failure (see the two-sided constraint above), got: " + p.status).c_str()); }
        std::fprintf(stderr, "[enospc] recorder status=%s\n", recStatus.c_str());
        if (recStatus.rfind("save failed: ", 0) != 0) { glfwTerminate(); return fail(("offline enc write fail: the Recorder claimed a file it could not finalise: " + recStatus).c_str()); }
        std::fprintf(stderr, "gl_smoke OK: a render and an inline recording that cannot write their file both fail instead of reporting success\n");
    }

    // --- Scenario: a write that fails DURING the render (not at close) fails the render ---
    // The case above is small enough that AVIOContext (~32 KiB buffer) holds the whole file until
    // close(), so it never reaches av_interleaved_write_frame's return -- the one the encoder used
    // to discard, and the true ENOSPC path for a long render that fills a disk hours in. High-
    // entropy frames force flushes mid-render, so the limit is crossed by a write, not a trailer.
    {
        const int W = 640, H = 480;
        const char* fixture = "gl_smoke_enospc_noise.png";
        if (!writeNoisePNG(fixture, W, H)) { glfwTerminate(); return fail("offline enc mid-write: write fixture"); }

        struct rlimit oldLim{};
        if (getrlimit(RLIMIT_FSIZE, &oldLim) != 0) { std::remove(fixture); glfwTerminate(); return fail("offline enc mid-write: getrlimit"); }
        void (*oldXfsz)(int) = std::signal(SIGXFSZ, SIG_IGN);
        // 16 KiB against a ~70 KiB output: a ~4x margin on TOTAL size, which is what has to hold
        // for the limit to be crossed at all. Which frame reports it is incidental and late
        // (measured: 54 of 60) because x264's lookahead and B-frame delay mean a frame's packet
        // reaches the muxer well after capture() handed it over -- so the assertion below is
        // "stopped short of framesTotal", not a specific frame number.
        struct rlimit lim = oldLim; lim.rlim_cur = 16 * 1024;
        if (setrlimit(RLIMIT_FSIZE, &lim) != 0) {
            std::signal(SIGXFSZ, oldXfsz); std::remove(fixture); glfwTerminate(); return fail("offline enc mid-write: setrlimit");
        }

        Graph g;
        auto img = std::make_unique<ImageStreamerNode>(); img->initGL();
        img->inputDefault(0) = Value(std::string(fixture));
        auto out = std::make_unique<OutputNode>(); out->initGL();
        int iId = g.addNode(std::move(img));
        int oId = g.addNode(std::move(out));
        bool wired = g.connect(iId, 0, oId, 0);

        RenderSettings s; s.startBar = 0.0; s.endBar = 1.0; s.prerollBars = 0.0; s.fps = 30;
        s.width = W; s.height = H; s.outPath = "build/_offline_enospc_mid.mp4";
        std::remove(s.outPath.c_str());
        OfflineRenderer r; std::string err;
        bool started = wired && r.start(g, s, err);
        int guard = 0; bool spun = true;
        while (started && r.step(0.05)) { if (++guard > 100000) { spun = false; break; } }
        OfflineRenderer::Progress p = r.progress();
        bool stillOffline = g.offline();

        bool restored = setrlimit(RLIMIT_FSIZE, &oldLim) == 0;
        bool sigBack  = std::signal(SIGXFSZ, oldXfsz) != SIG_ERR;
        clearerr(stderr);            // see the note in the scenario above: stderr is latched in error
        std::remove(fixture);

        if (!restored) { glfwTerminate(); return fail("offline enc mid-write: could not restore RLIMIT_FSIZE -- every later scenario would fail confusingly"); }
        if (!sigBack)  { glfwTerminate(); return fail("offline enc mid-write: could not restore the SIGXFSZ handler"); }
        if (!wired)   { glfwTerminate(); return fail("offline enc mid-write: connect"); }
        if (!started) { glfwTerminate(); return fail(("offline enc mid-write: start: " + err).c_str()); }
        if (!spun)    { glfwTerminate(); return fail("offline enc mid-write: never finished"); }
        std::fprintf(stderr, "[enospc-mid] phase=%d frames=%lld status=%s\n", (int)p.phase, p.framesDone, p.status.c_str());
        if (p.phase == OfflineRenderer::Phase::Done) { glfwTerminate(); return fail("offline enc mid-write: a render that could not write its frames reported success"); }
        // Specifically the frame route: this is what proves av_interleaved_write_frame's return is
        // propagated and that capture() acts on addVideoFrame's bool.
        if (p.status.rfind("encode failed at frame ", 0) != 0) { glfwTerminate(); return fail(("offline enc mid-write: expected a per-frame encode failure, got: " + p.status).c_str()); }
        if (p.framesDone >= p.framesTotal) { glfwTerminate(); return fail("offline enc mid-write: the render should have stopped short, not captured every frame"); }
        if (stillOffline) { glfwTerminate(); return fail("offline enc mid-write: the failure must still clear the offline flag"); }
        std::fprintf(stderr, "gl_smoke OK: a write failure mid-render stops the render and reports the frame it failed on\n");
    }

    // --- Scenario: a recording that LOST FRAMES is not reported as saved ---
    // The fifth swallow: RecorderNode::evaluate drops addVideoFrame()/addAudio()'s bool on every
    // frame, so a take whose writes failed mid-way but whose trailer still wrote came back
    // "saved" -- a file quietly missing frames, reported as a good one. VideoEncoder now latches
    // writeFailed_ on any write/encode failure and close() consults it, so the per-call bools no
    // longer have to be checked for the file to be judged honestly.
    // Isolating that needs the writes to fail DURING the take and SUCCEED at the end, so the
    // limit is lifted before the recording is stopped -- otherwise the trailer fails too and the
    // latch is not what produced the verdict (which is why the scenarios above do not cover it).
    //
    // What this does and does not prove, precisely: FFmpeg's AVIOContext latches its OWN write
    // error, so in this reproduction close()'s flush fails as well and the take would be judged
    // badly even without writeFailed_ -- what the latch changes HERE is that the verdict names
    // the lost frames instead of the flush. The latch's unique ground is a CODEC-level refusal
    // (avcodec_send_frame/avcodec_receive_packet failing), where avio never sees an error and
    // the trailer writes cleanly; that is not provokable from a test, so this scenario pins the
    // message rather than claiming to be the only thing standing between "saved" and not.
    {
        const int W = 640, H = 480;
        const char* fixture = "gl_smoke_lostframes_noise.png";
        if (!writeNoisePNG(fixture, W, H)) { glfwTerminate(); return fail("lost frames: write fixture"); }

        struct rlimit oldLim{};
        if (getrlimit(RLIMIT_FSIZE, &oldLim) != 0) { std::remove(fixture); glfwTerminate(); return fail("lost frames: getrlimit"); }
        void (*oldXfsz)(int) = std::signal(SIGXFSZ, SIG_IGN);
        // 16 KiB and a 120-frame take. x264's lookahead and B-frame delay mean a frame's packet
        // reaches the muxer well after the Recorder handed it over, so the crossing lands late
        // (the sibling scenario above measures frame 54 of 60 at this limit) -- the take has to
        // be long enough to get there BEFORE the limit is lifted, or nothing fails and this
        // scenario silently stops testing the latch. The second assertion below catches that.
        struct rlimit lim = oldLim; lim.rlim_cur = 16 * 1024;
        if (setrlimit(RLIMIT_FSIZE, &lim) != 0) {
            std::signal(SIGXFSZ, oldXfsz); std::remove(fixture); glfwTerminate(); return fail("lost frames: setrlimit");
        }

        std::string recStatus = "(not run)";
        bool wired = false;
        {
            Graph g;
            Preferences big; big.textureWidth = W; big.textureHeight = H;
            g.setPreferences(&big);
            auto img = std::make_unique<ImageStreamerNode>(); img->initGL();
            img->inputDefault(0) = Value(std::string(fixture));
            auto rec = std::make_unique<RecorderNode>();
            rec->inputDefault(3) = true;
            rec->inputDefault(4) = std::string("build/_rec_lostframes.mp4");
            auto out = std::make_unique<OutputNode>(); out->initGL();
            int iId = g.addNode(std::move(img)); int rId = g.addNode(std::move(rec));
            int oId = g.addNode(std::move(out));
            wired = g.connect(iId, 0, rId, 0) && g.connect(rId, 0, oId, 0);
            if (wired) {
                std::remove("build/_rec_lostframes.mp4");
                for (int i = 0; i < 120; ++i) g.evaluate(1.0f / 30.0f);  // writes fail in here
                setrlimit(RLIMIT_FSIZE, &oldLim);                        // ...but not at the end
                auto* rn = dynamic_cast<RecorderNode*>(g.findNode(rId));
                rn->inputDefault(3) = false;
                g.evaluate(1.0f / 30.0f);                                // stop() -> close()
                recStatus = rn->statusLine();
            }
        }

        bool restored = setrlimit(RLIMIT_FSIZE, &oldLim) == 0;
        bool sigBack  = std::signal(SIGXFSZ, oldXfsz) != SIG_ERR;
        clearerr(stderr);            // see the note two scenarios above: stderr is latched in error
        std::remove(fixture);

        if (!restored) { glfwTerminate(); return fail("lost frames: could not restore RLIMIT_FSIZE"); }
        if (!sigBack)  { glfwTerminate(); return fail("lost frames: could not restore the SIGXFSZ handler"); }
        if (!wired)    { glfwTerminate(); return fail("lost frames: connect"); }
        std::fprintf(stderr, "[lost frames] recorder status=%s\n", recStatus.c_str());
        if (recStatus.rfind("save failed: ", 0) != 0) { glfwTerminate(); return fail(("lost frames: a take that lost frames was reported as saved: " + recStatus).c_str()); }
        // Specifically the latch, not the trailer: the trailer wrote fine once the limit was
        // lifted, so anything else here means this scenario stopped testing writeFailed_.
        if (recStatus.find("frames were lost during encoding") == std::string::npos)
            { glfwTerminate(); return fail(("lost frames: expected the sticky write-failure verdict, got: " + recStatus).c_str()); }
        std::fprintf(stderr, "gl_smoke OK: a recording whose writes failed mid-take is not reported as saved, even though its trailer wrote\n");
    }
#else
    std::fprintf(stderr, "gl_smoke SKIP: the three encode-write-failure scenarios (RLIMIT_FSIZE is POSIX-only)\n");
#endif

    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
