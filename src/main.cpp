#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <glm/vec2.hpp>
#include "app/Application.h"
#include "modules/AutomationNode.h"
#include "gfx/GLUtil.h"
#include "app/OfflineRenderer.h"
#include "core/OfflineRender.h"
#include <chrono>
#include <thread>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// Fullscreen-triangle blit (no VBO) used by the output window to show the graph's
// output texture. The texture is an FBO attachment (bottom-left origin), so V is
// flipped here to display it upright.
static const char* kBlitVS = R"(#version 410 core
out vec2 vUV;
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    vUV = (p + 1.0) * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
static const char* kBlitFS = R"(#version 410 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
void main() { FragColor = texture(uTex, vec2(vUV.x, 1.0 - vUV.y)); }
)";

// Headless UI capture: render a few ImGui frames of the app (with a small demo
// graph) into a hidden window, lay the windows out, and write the result to a PNG.
// Lets UI layouts be checked without an interactive session. `--screenshot [path]`.
static int runScreenshot(const std::string& path) {
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* win = glfwCreateWindow(1280, 900, "shader-streamer-screenshot", nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "createWindow failed (no offscreen GL?)\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) {
        std::fprintf(stderr, "gladLoadGL failed\n");
        glfwDestroyWindow(win); glfwTerminate(); return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_DockingEnable;
    ImGui::GetIO().IniFilename = nullptr;   // deterministic capture: always the fresh default layout
    ImGui_ImplGlfw_InitForOpenGL(win, false);                 // headless: no input callbacks
    ImGui_ImplOpenGL3_Init("#version 410");

    int rc = 0;
    {
        oss::Application app(win);
        // Demo content so the panels aren't empty.
        int aId = app.addNodeOfType("Automation", glm::vec2(420.0f, 60.0f));
        if (auto* an = dynamic_cast<oss::AutomationNode*>(app.graph().findNode(aId))) {
            app.graph().automation().setLengthBars(8.0f);
            an->channel(0) = { {0.0f, 0.20f}, {2.0f, 0.85f}, {5.0f, 0.40f}, {8.0f, 0.95f} };
            an->channel(1) = { {0.0f, 0.60f}, {4.0f, 0.10f}, {8.0f, 0.70f} };
            an->setOutRange(2, 20.0f, 2000.0f);
        }
        // A ui-automation channel on a second module, so the captured grid shows
        // both a stream group and a ui group.
        int wfId = app.addNodeOfType("Wireframe", glm::vec2(420.0f, 240.0f));
        if (auto* ch = app.graph().automation().add(app.graph(), wfId, 1)) {  // Wireframe "spin" (Float)
            ch->curve.points = { {0.0f, 0.30f}, {3.0f, 0.90f}, {8.0f, 0.20f} };
        }
        // An LFO node so the capture shows the choice dropdowns + the new module.
        app.addNodeOfType("LFO", glm::vec2(740.0f, 60.0f));
        app.addNodeOfType("Acid Bass", glm::vec2(740.0f, 320.0f));
        app.addNodeOfType("MIDI File", glm::vec2(40.0f, 320.0f));
        app.addNodeOfType("Oscilloscope", glm::vec2(1060.0f, 60.0f));
        app.addNodeOfType("Chord Player", glm::vec2(1080.0f, 320.0f));
        app.addNodeOfType("Compositor", glm::vec2(260.0f, 200.0f));
        app.addNodeOfType("Pitch Graph", glm::vec2(620.0f, 200.0f));
        app.addNodeOfType("Skybox", glm::vec2(240.0f, 330.0f));
        app.addNodeOfType("Vertex Shader", glm::vec2(40.0f, 130.0f));
        app.addNodeOfType("Deform", glm::vec2(620.0f, 110.0f));
        app.addNodeOfType("Mono to Stereo", glm::vec2(560.0f, 380.0f));
        app.addNodeOfType("Stereo to Mono", glm::vec2(920.0f, 380.0f));
        app.addNodeOfType("Vertex Trail", glm::vec2(340.0f, 150.0f));
        app.addNodeOfType("Output", glm::vec2(900.0f, 200.0f));   // so the Render dialog validates
        app.showRenderDialog(true);                               // capture the Render Video dialog
        app.graph().transport().bpm = 120.0;
        app.graph().transport().seconds = 6.0;   // playhead at bar 3

        // ImGui lays out in logical points (window size); GL works in framebuffer
        // pixels (2x on a Retina display) -- keep the two apart.
        int fbW = 0, fbH = 0;   glfwGetFramebufferSize(win, &fbW, &fbH);
        for (int f = 0; f < 4; ++f) {            // a few frames so ImGui settles
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.frame(1.0f / 60.0f);             // submits the host dockspace + default layout
            ImGui::Render();
            glViewport(0, 0, fbW, fbH);
            glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        std::vector<unsigned char> px((std::size_t)fbW * fbH * 4);
        glFinish();
        glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        std::vector<unsigned char> flip(px.size());        // GL origin is bottom-left
        for (int y = 0; y < fbH; ++y)
            std::memcpy(&flip[(std::size_t)y * fbW * 4], &px[(std::size_t)(fbH - 1 - y) * fbW * 4], (std::size_t)fbW * 4);
        if (stbi_write_png(path.c_str(), fbW, fbH, 4, flip.data(), fbW * 4))
            std::fprintf(stderr, "wrote screenshot %s (%dx%d)\n", path.c_str(), fbW, fbH);
        else { std::fprintf(stderr, "failed to write %s\n", path.c_str()); rc = 1; }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return rc;
}

// Headless offline render: `--render <project.oss> <out.mp4> [--start B] [--end B] [--fps N]
// [--size WxH] [--preroll B]`. Loads preferences + the project into an Application on a hidden
// window (like --screenshot) and steps its OfflineRenderer to completion, printing progress
// once a second. Exit 0 when the render finished, 1 otherwise (the reason is on stderr).
static int runRender(const std::vector<std::string>& args) {
    oss::RenderCliArgs cli; std::string err;
    if (!oss::parseRenderArgs(args, cli, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(640, 480, "shader-streamer-render", nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "createWindow failed (no offscreen GL?)\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) {
        std::fprintf(stderr, "gladLoadGL failed\n");
        glfwDestroyWindow(win); glfwTerminate(); return 1;
    }

    // ShaderNode loads its fragment shader by a CWD-RELATIVE path, so run from a directory with
    // no shaders/ every shader node links a corrupt program, draws nothing, and still publishes
    // a valid texture id -- the render writes undefined framebuffer memory to a perfectly
    // well-formed mp4 and exits 0. Nothing downstream can catch that: the black-frame counter
    // only sees a MISSING texture, and the outcome line has nothing to report. Running from
    // somewhere else is the entire point of a headless mode and the exit code is the only signal
    // a batch pipeline gets, so refuse up front. (The thorough fix is an executable-relative
    // fallback in readFile(), a wider change that belongs in its own commit.)
    {
        std::error_code ec;
        if (!std::filesystem::exists("shaders/colour.frag", ec)) {
            const std::string cwd = std::filesystem::current_path(ec).string();
            std::fprintf(stderr,
                         "--render: no shaders/ directory here (looked for shaders/colour.frag in %s).\n"
                         "Shaders are loaded relative to the working directory: cd to the repo root, or\n"
                         "to the folder holding the installed binary's shaders/, and run --render from there.\n",
                         cwd.empty() ? "the working directory" : cwd.c_str());
            glfwDestroyWindow(win); glfwTerminate(); return 1;
        }
    }

    IMGUI_CHECKVERSION();                     // the Application's panels need a context even unused
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplGlfw_InitForOpenGL(win, false);
    ImGui_ImplOpenGL3_Init("#version 410");

    int rc = 1;
    {
        oss::Application app(win);
        if (!app.loadProjectFromFile(cli.projectPath)) {
            std::fprintf(stderr, "could not load project %s\n", cli.projectPath.c_str());
        } else {
            oss::RenderSettings s = cli.settings;
            // Fill only what the command line did not actually give: comparing endBar/width/height
            // back against the parser's sentinels would misfire if someone typed the sentinel itself
            // (e.g. `--end -1`, `--size 0x100`) -- endGiven/sizeGiven are the structural signal.
            if (!cli.endGiven)  s.endBar = app.graph().automation().lengthBars();
            if (!cli.sizeGiven) { s.width = app.preferences().textureWidth; s.height = app.preferences().textureHeight; }
            oss::OfflineRenderer& r = app.renderer();
            if (!r.start(app.graph(), s, err)) {
                std::fprintf(stderr, "render failed: %s\n", err.c_str());
            } else {
                double lastPrint = glfwGetTime();
                while (r.step(1.0)) {
                    glfwPollEvents();
                    if (r.progress().waitingForLoad)                                  // loader wait: don't spin
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    double t = glfwGetTime();
                    if (t - lastPrint >= 1.0) {
                        lastPrint = t;
                        const oss::OfflineRenderer::Progress& p = r.progress();
                        std::fprintf(stderr, "  %lld / %lld frames (%.1fx real time)%s%s\n",
                                     p.framesDone, p.framesTotal, p.speed / (double)s.fps,
                                     p.status.empty() ? "" : " - ", p.status.c_str());
                    }
                }
                rc = (r.progress().phase == oss::OfflineRenderer::Phase::Done) ? 0 : 1;
            }
        }
    }   // app destroyed here (its context is current)

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return rc;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--render") == 0)
            return runRender(std::vector<std::string>(argv + i + 1, argv + argc));
        if (std::strcmp(argv[i], "--screenshot") == 0) {
            std::string path = (i + 1 < argc) ? argv[i + 1] : "screenshot.png";
            return runScreenshot(path);
        }
    }

    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* editorWin = glfwCreateWindow(1100, 850, "Shader Streamer - Graph", nullptr, nullptr);
    if (!editorWin) { glfwTerminate(); return 1; }
    // The output window's context SHARES objects with the editor's: textures and
    // buffers created in one are visible in the other (container objects like VAOs
    // and FBOs are not shared, so each context makes its own).
    GLFWwindow* outputWin = glfwCreateWindow(960, 540, "Shader Streamer - Output", nullptr, editorWin);
    if (!outputWin) { glfwDestroyWindow(editorWin); glfwTerminate(); return 1; }

    glfwMakeContextCurrent(editorWin);
    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) {
        std::fprintf(stderr, "glad failed\n");
        glfwDestroyWindow(outputWin); glfwDestroyWindow(editorWin); glfwTerminate();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // dockable editor panels
    ImGui_ImplGlfw_InitForOpenGL(editorWin, true);   // ImGui input/callbacks: editor window only
    ImGui_ImplOpenGL3_Init("#version 410");

    // Build the blit program + VAO in the OUTPUT context (VAOs aren't shared).
    glfwMakeContextCurrent(outputWin);
    glfwSwapInterval(0);
    GLuint blitProg = oss::linkProgram(kBlitVS, kBlitFS);
    GLuint blitVao = 0;
    glGenVertexArrays(1, &blitVao);

    glfwMakeContextCurrent(editorWin);
    glfwSwapInterval(1);   // pace the loop off the editor's vsync

    {
        oss::Application app(editorWin);   // node GL objects live in the editor context
        double last = glfwGetTime();
        while (!glfwWindowShouldClose(editorWin) && !glfwWindowShouldClose(outputWin)) {
            glfwPollEvents();
            double now = glfwGetTime();
            float dt = (float)(now - last); last = now;

            // --- Editor window: ImGui node graph + graph evaluation (renders FBOs) ---
            glfwMakeContextCurrent(editorWin);
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.frame(dt);
            ImGui::Render();
            int ew, eh; glfwGetFramebufferSize(editorWin, &ew, &eh);
            glViewport(0, 0, ew, eh);
            glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(editorWin);

            // --- Output window: blit the graph's output texture (shared object) ---
            glfwMakeContextCurrent(outputWin);
            int ow, oh; glfwGetFramebufferSize(outputWin, &ow, &oh);
            glViewport(0, 0, ow, oh);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            oss::TexRef tex = app.outputTexture();
            if (tex.id) {
                glUseProgram(blitProg);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, tex.id);
                glUniform1i(glGetUniformLocation(blitProg, "uTex"), 0);
                glBindVertexArray(blitVao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
            }
            glfwSwapBuffers(outputWin);
        }

        // Free the output-window GL in its own context, then make the editor
        // context current so the node GL objects free in the context that owns them.
        glfwMakeContextCurrent(outputWin);
        glDeleteVertexArrays(1, &blitVao);
        glDeleteProgram(blitProg);
        glfwMakeContextCurrent(editorWin);
    }   // app destroyed here (editor context current)

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(outputWin);
    glfwDestroyWindow(editorWin);
    glfwTerminate();
    return 0;
}
