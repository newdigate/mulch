cmake_minimum_required(VERSION 3.16)
project(opengl_shader_streamer CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# glad v2 requires jinja2. On macOS the system Python may not have it;
# prefer a Python that does (3.11 ships with Homebrew jinja2 on this machine).
# Override at configure time with -DPython_EXECUTABLE=/path/to/python if needed.
find_program(_py311 python3.11)
if(_py311)
  set(Python_EXECUTABLE "${_py311}" CACHE FILEPATH "Python used by glad generator" FORCE)
endif()
unset(_py311)

# --- GLFW ---
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# --- glad (OpenGL 4.1 core loader) ---
FetchContent_Declare(glad GIT_REPOSITORY https://github.com/Dav1dde/glad.git GIT_TAG v2.0.8
                     SOURCE_SUBDIR cmake)
FetchContent_MakeAvailable(glad)
glad_add_library(glad_gl41 REPRODUCIBLE API gl:core=4.1)

# --- glm ---
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
FetchContent_MakeAvailable(glm)

# --- Dear ImGui (sources only; we build the lib ourselves) ---
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.5)
FetchContent_MakeAvailable(imgui)

# --- imgui-node-editor ---
FetchContent_Declare(imgui_node_editor
  GIT_REPOSITORY https://github.com/thedmd/imgui-node-editor.git GIT_TAG master) # TODO: pin to a tagged release once API verified
FetchContent_MakeAvailable(imgui_node_editor)

# --- doctest ---
FetchContent_Declare(doctest GIT_REPOSITORY https://github.com/doctest/doctest.git GIT_TAG v2.4.11)
# doctest v2.4.11's bundled CMake uses cmake_minimum_required(VERSION 3.0), which
# CMake 4.x rejects. Relax the policy minimum just for doctest's sub-build, then
# restore it so other dependencies are unaffected.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(doctest)
unset(CMAKE_POLICY_VERSION_MINIMUM)

find_package(OpenGL REQUIRED)

# --- UI third-party static lib: ImGui + backends + node editor ---
add_library(ui_thirdparty STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor.cpp
  ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor_api.cpp
  ${imgui_node_editor_SOURCE_DIR}/imgui_canvas.cpp
  ${imgui_node_editor_SOURCE_DIR}/crude_json.cpp
)
target_include_directories(ui_thirdparty PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${imgui_node_editor_SOURCE_DIR}
)
target_link_libraries(ui_thirdparty PUBLIC glfw OpenGL::GL)
target_compile_definitions(ui_thirdparty PRIVATE GLFW_INCLUDE_NONE)

# --- Core/gfx/audio/modules/ui sources shared by the app ---
set(APP_SOURCES
  src/core/Node.cpp
  src/core/Graph.cpp
  src/audio/FFT.cpp
  src/audio/SignalGenerator.cpp
  src/gfx/GLUtil.cpp
  src/gfx/Framebuffer.cpp
  src/gfx/FullscreenPass.cpp
  src/gfx/ShaderNode.cpp
  src/modules/SpectrographNode.cpp
  src/ui/NodeEditorPanel.cpp
  src/ui/PortWidgets.cpp
  src/app/Application.cpp
)

# --- Application executable ---
add_executable(shader_streamer src/main.cpp ${APP_SOURCES})
target_include_directories(shader_streamer PRIVATE src)
target_compile_definitions(shader_streamer PRIVATE GLFW_INCLUDE_NONE)
target_link_libraries(shader_streamer PRIVATE ui_thirdparty glad_gl41 glm::glm)

# --- Tests (GL-free core only) ---
enable_testing()
add_executable(core_tests
  tests/test_main.cpp
  tests/test_sanity.cpp
  tests/test_value.cpp
  tests/test_graph.cpp
  tests/test_fft.cpp
  tests/test_signal_generator.cpp
  src/core/Node.cpp
  src/core/Graph.cpp
  src/audio/FFT.cpp
  src/audio/SignalGenerator.cpp
)
target_include_directories(core_tests PRIVATE src)
target_link_libraries(core_tests PRIVATE glm::glm doctest::doctest)
add_test(NAME core_tests COMMAND core_tests)

# Headless GL render smoke test: renders the graph offscreen and reads back pixels.
# Requires a display/WindowServer (works locally on macOS; may be skipped in headless CI).
add_executable(gl_smoke
  tests/gl_smoke.cpp
  src/core/Node.cpp
  src/core/Graph.cpp
  src/gfx/GLUtil.cpp
  src/gfx/Framebuffer.cpp
  src/gfx/FullscreenPass.cpp
  src/gfx/ShaderNode.cpp
  src/modules/SpectrographNode.cpp
  src/audio/FFT.cpp
  src/audio/SignalGenerator.cpp
)
target_include_directories(gl_smoke PRIVATE src)
target_compile_definitions(gl_smoke PRIVATE GLFW_INCLUDE_NONE)
target_link_libraries(gl_smoke PRIVATE glad_gl41 glfw glm::glm OpenGL::GL)
add_test(NAME gl_smoke COMMAND gl_smoke WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
