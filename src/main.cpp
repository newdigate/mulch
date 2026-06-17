#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <glm/vec2.hpp>
#include "app/Application.h"
#include "modules/AutomationNode.h"
#include "gfx/GLUtil.h"
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
    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) { glfwDestroyWindow(win); glfwTerminate(); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;   // no hover artefacts
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
        app.graph().transport().bpm = 120.0;
        app.graph().transport().seconds = 6.0;   // playhead at bar 3

        // ImGui lays out in logical points (window size); GL works in framebuffer
        // pixels (2x on a Retina display) -- keep the two apart.
        int winW = 0, winH = 0; glfwGetWindowSize(win, &winW, &winH);
        int fbW = 0, fbH = 0;   glfwGetFramebufferSize(win, &fbW, &fbH);
        for (int f = 0; f < 4; ++f) {            // a few frames so ImGui settles
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.frame(1.0f / 60.0f);
            // Lay the windows out (logical points) below the transport menu bar.
            const float top = 26.0f, gap = 8.0f;
            const float graphH = (winH - top) * 0.55f;
            ImGui::SetWindowPos("Node Graph", ImVec2(gap, top));
            ImGui::SetWindowSize("Node Graph", ImVec2(winW - gap * 2, graphH));
            ImGui::SetWindowPos("Automation", ImVec2(gap, top + graphH + gap));
            ImGui::SetWindowSize("Automation", ImVec2(winW - gap * 2, (winH - top) * 0.45f - gap * 2));
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

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--screenshot") == 0) {
            std::string path = (i + 1 < argc) ? argv[i + 1] : "screenshot.png";
            return runScreenshot(path);
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
