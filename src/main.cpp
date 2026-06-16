#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <cstdio>
#include "app/Application.h"
#include "gfx/GLUtil.h"

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

int main() {
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
