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
  GIT_REPOSITORY https://github.com/thedmd/imgui-node-editor.git
  GIT_TAG 021aa0ea4da13fed864bafb2a92d4c5205076866) # pinned: v0.9.4 (commit 021aa0e)
FetchContent_MakeAvailable(imgui_node_editor)

# --- doctest ---
FetchContent_Declare(doctest GIT_REPOSITORY https://github.com/doctest/doctest.git GIT_TAG v2.4.11)
# doctest v2.4.11's bundled CMake uses cmake_minimum_required(VERSION 3.0), which
# CMake 4.x rejects. Relax the policy minimum just for doctest's sub-build, then
# restore it so other dependencies are unaffected.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(doctest)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# --- libsoundio (cross-platform audio output) ---
FetchContent_Declare(libsoundio
  GIT_REPOSITORY https://github.com/andrewrk/libsoundio.git
  GIT_TAG 2.0.0)
# Build only the static lib we link; skip the shared lib, example programs, and
# the library's own test suite.
set(BUILD_DYNAMIC_LIBS   OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS    ON  CACHE BOOL "" FORCE)
set(BUILD_EXAMPLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS          OFF CACHE BOOL "" FORCE)
# libsoundio's bundled CMake requires cmake_minimum_required(VERSION 2.8.x),
# which CMake 4.x rejects. Relax the policy minimum just for its sub-build.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(libsoundio)
unset(CMAKE_POLICY_VERSION_MINIMUM)
# libsoundio's Debug build adds -Werror; its CoreAudio backend still uses
# kAudioObjectPropertyElementMaster, deprecated since macOS 12, which then fails
# the build. Stop treating its warnings as errors (its target only).
if(TARGET libsoundio_static)
  target_compile_options(libsoundio_static PRIVATE -Wno-error -Wno-deprecated-declarations)
  # Its bundled CMake links the CoreAudio frameworks only to its (skipped) own
  # binaries, not as usage requirements, so consumers must get them. Attach them
  # as INTERFACE deps so anything linking libsoundio_static pulls them in.
  if(APPLE)
    target_link_libraries(libsoundio_static INTERFACE
      "-framework CoreAudio" "-framework AudioUnit"
      "-framework AudioToolbox" "-framework CoreFoundation")
  endif()
endif()

# --- RtMidi (cross-platform MIDI I/O) ---
FetchContent_Declare(rtmidi
  GIT_REPOSITORY https://github.com/thestk/rtmidi.git
  GIT_TAG 6.0.0)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)   # rtmidi has no deps after it
# Avoid a target-name clash: GLFW already defines an "uninstall" target.
set(RTMIDI_TARGETNAME_UNINSTALL "rtmidi_uninstall" CACHE STRING "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(rtmidi)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# --- tinyobjloader (.obj mesh loading) ---
FetchContent_Declare(tinyobjloader
  GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
  GIT_TAG v1.0.6)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(tinyobjloader)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# --- tinygltf (.gltf mesh loading; header-only, geometry only) ---
FetchContent_Declare(tinygltf
  GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
  GIT_TAG v2.9.3)
set(TINYGLTF_HEADER_ONLY ON CACHE BOOL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(tinygltf)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# --- meshoptimizer (decodes EXT_meshopt_compression bufferViews) ---
FetchContent_Declare(meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  GIT_TAG v0.22)
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(meshoptimizer)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# --- draco (KHR_draco_mesh_compression; tinygltf decodes it when enabled) ---
FetchContent_Declare(draco
  GIT_REPOSITORY https://github.com/google/draco.git
  GIT_TAG 1.5.7)
set(DRACO_TRANSCODER_SUPPORTED OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(draco)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# --- stb (stb_truetype: glyph outlines for the Text nodes; header-only) ---
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4)  # pinned: master @ 2024
FetchContent_MakeAvailable(stb)   # no CMakeLists -> populate only

# --- earcut.hpp (polygon triangulation for filled/extruded glyphs; header-only) ---
FetchContent_Declare(earcut
  GIT_REPOSITORY https://github.com/mapbox/earcut.hpp.git
  GIT_TAG v2.2.4)
set(EARCUT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EARCUT_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(EARCUT_BUILD_VIZ   OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(earcut)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# The Text nodes' default font: one of Dear ImGui's bundled, permissively-licensed
# TTFs (Roboto-Medium, Apache-2.0), referenced by absolute path so no font file
# needs to ship or be embedded. Override per-node via the "font" input.
set(OSS_DEFAULT_FONT "${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf")

find_package(OpenGL REQUIRED)

# --- FFmpeg (video/audio decoding for the Video Player node) ---
# The one system dependency: FFmpeg doesn't FetchContent cleanly (it's not a
# CMake project), so we find the brew/system dev libraries via pkg-config.
# Install with `brew install ffmpeg` (macOS) or your distro's libav*-dev packages.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale libswresample)
message(STATUS "FFmpeg: libavcodec ${FFMPEG_libavcodec_VERSION}")

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
  src/core/AutomationStore.cpp
  src/core/Preferences.cpp
  src/core/ProjectFile.cpp
  src/core/MidiFile.cpp
  src/core/Oscilloscope.cpp
  src/core/PitchGraph.cpp
  src/core/VertexTrail.cpp
  src/audio/FFT.cpp
  src/audio/SignalGenerator.cpp
  src/audio/AcidVoice.cpp
  src/audio/AudioFile.cpp
  src/gfx/GLUtil.cpp
  src/gfx/Framebuffer.cpp
  src/gfx/FullscreenPass.cpp
  src/gfx/ShaderNode.cpp
  src/gfx/MeshLoader.cpp
  src/gfx/VideoDecoder.cpp
  src/gfx/VideoEncoder.cpp
  src/gfx/TextGeometry.cpp
  src/modules/AudioInputNode.cpp
  src/modules/AudioPlayerNode.cpp
  src/modules/RecorderNode.cpp
  src/modules/TextNode.cpp
  src/modules/VideoPlayerNode.cpp
  src/modules/AudioOutputNode.cpp
  src/modules/MidiInputNode.cpp
  src/modules/MidiOutputNode.cpp
  src/modules/MeshLoaderNode.cpp
  src/modules/ShadedRenderNode.cpp
  src/modules/SpectrographNode.cpp
  src/modules/WireframeNode.cpp
  src/ui/NodeEditorPanel.cpp
  src/ui/PortWidgets.cpp
  src/ui/TransportBar.cpp
  src/ui/AutomationPanel.cpp
  src/ui/PreferencesPanel.cpp
  src/app/Application.cpp
)

# --- Application executable ---
add_executable(shader_streamer src/main.cpp ${APP_SOURCES})
target_include_directories(shader_streamer PRIVATE src ${libsoundio_SOURCE_DIR} ${rtmidi_SOURCE_DIR}
  ${tinygltf_SOURCE_DIR} ${meshoptimizer_SOURCE_DIR}/src ${draco_SOURCE_DIR}/src
  ${stb_SOURCE_DIR} ${earcut_SOURCE_DIR}/include ${CMAKE_BINARY_DIR})
target_compile_definitions(shader_streamer PRIVATE GLFW_INCLUDE_NONE TINYGLTF_ENABLE_DRACO
  OSS_DEFAULT_FONT="${OSS_DEFAULT_FONT}")
target_link_libraries(shader_streamer PRIVATE ui_thirdparty glad_gl41 glm::glm libsoundio_static rtmidi
  tinyobjloader meshoptimizer draco_static PkgConfig::FFMPEG)

# --- Tests (GL-free core only) ---
enable_testing()
add_executable(core_tests
  tests/test_main.cpp
  tests/test_sanity.cpp
  tests/test_value.cpp
  tests/test_graph.cpp
  tests/test_fft.cpp
  tests/test_signal_generator.cpp
  tests/test_sine_wave.cpp
  tests/test_ring_buffer.cpp
  tests/test_audio_mixer.cpp
  tests/test_audio_pan.cpp
  tests/test_audio_bridges.cpp
  tests/test_arpeggiator.cpp
  tests/test_step_sequencer.cpp
  tests/test_midi_merge.cpp
  tests/test_mesh_edges.cpp
  tests/test_transport.cpp
  tests/test_automation.cpp
  tests/test_world_transform.cpp
  tests/test_auto_curve.cpp
  tests/test_automation_store.cpp
  tests/test_choice_port.cpp
  tests/test_lfo.cpp
  tests/test_step_sync.cpp
  tests/test_acid_voice.cpp
  tests/test_midi_file.cpp
  tests/test_midi_clip.cpp
  tests/test_oscilloscope.cpp
  tests/test_chords.cpp
  tests/test_chord_player.cpp
  tests/test_blend_modes.cpp
  tests/test_pitch_graph.cpp
  tests/test_cube_map.cpp
  tests/test_vertex_shaders.cpp
  tests/test_color_hsv.cpp
  tests/test_vertex_trail.cpp
  tests/test_project_enablers.cpp
  tests/test_project_file.cpp
  tests/test_preferences.cpp
  src/core/Node.cpp
  src/core/Graph.cpp
  src/core/AutomationStore.cpp
  src/core/Preferences.cpp
  src/core/ProjectFile.cpp
  src/core/MidiFile.cpp
  src/core/Oscilloscope.cpp
  src/core/PitchGraph.cpp
  src/core/VertexTrail.cpp
  src/audio/FFT.cpp
  src/audio/SignalGenerator.cpp
  src/audio/AcidVoice.cpp
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
  src/core/AutomationStore.cpp
  src/core/ProjectFile.cpp
  src/core/PitchGraph.cpp
  src/core/VertexTrail.cpp
  src/gfx/GLUtil.cpp
  src/gfx/Framebuffer.cpp
  src/gfx/FullscreenPass.cpp
  src/gfx/ShaderNode.cpp
  src/gfx/MeshLoader.cpp
  src/gfx/VideoDecoder.cpp
  src/gfx/VideoEncoder.cpp
  src/gfx/TextGeometry.cpp
  src/modules/MeshLoaderNode.cpp
  src/modules/RecorderNode.cpp
  src/modules/ShadedRenderNode.cpp
  src/modules/SpectrographNode.cpp
  src/modules/TextNode.cpp
  src/modules/VideoPlayerNode.cpp
  src/modules/WireframeNode.cpp
  src/modules/AudioPlayerNode.cpp
  src/audio/FFT.cpp
  src/audio/SignalGenerator.cpp
  src/audio/AudioFile.cpp
)
target_include_directories(gl_smoke PRIVATE src ${tinygltf_SOURCE_DIR}
  ${meshoptimizer_SOURCE_DIR}/src ${draco_SOURCE_DIR}/src
  ${stb_SOURCE_DIR} ${earcut_SOURCE_DIR}/include ${CMAKE_BINARY_DIR})
target_compile_definitions(gl_smoke PRIVATE GLFW_INCLUDE_NONE TINYGLTF_ENABLE_DRACO
  OSS_DEFAULT_FONT="${OSS_DEFAULT_FONT}")
target_link_libraries(gl_smoke PRIVATE glad_gl41 glfw glm::glm OpenGL::GL tinyobjloader meshoptimizer draco_static PkgConfig::FFMPEG)
add_test(NAME gl_smoke COMMAND gl_smoke WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Copy shaders next to the executable so the app can be run from the build dir too
# (it also resolves shaders/ when run from the repo root).
add_custom_command(TARGET shader_streamer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:shader_streamer>/shaders)
