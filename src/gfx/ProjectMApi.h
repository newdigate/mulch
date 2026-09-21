#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "core/DynLib.h"

namespace oss {

// libprojectM 4.2+ C API, resolved at runtime. No projectM header is included and the library is
// never linked: every function is looked up by name. No GL/GLFW headers here either — GL handles
// cross as uint32_t — so this unit is compiled into core_tests.
struct PmOpaque;                                   // never defined: projectM's opaque instance
using PmHandle         = PmOpaque*;
using PmLoadProc       = void* (*)(const char* name, void* userData);
using PmSwitchFailedFn = void (*)(const char* presetFilename, const char* message, void* userData);

constexpr int kPmStereo = 2;                       // projectm_channels::PROJECTM_STEREO

// The accepted projectM versions: exactly major 4, minor >= 2 (render_frame_fbo, burn_texture,
// set_frame_time and create_with_opengl_load_proc are all "@since 4.2.0").
bool isSupportedProjectMVersion(int major, int minor);

// Where to look, in order: the preference path (when set), the bare platform library names via
// the system loader, then `<homeDir>/.local/lib`, `/usr/local/lib`, `/opt/homebrew/lib` (non-Windows;
// `homeDir` may be empty, in which case that first directory is skipped).
std::vector<std::string> projectMCandidatePaths(const std::string& prefPath, const std::string& homeDir);

// The resolved functions. A plain copyable struct so a candidate library is bound into a LOCAL
// table and adopted only on success: a rejected library is closed without ever leaving dangling
// pointers in the live table. All null until ProjectMApi::available().
struct ProjectMFunctions {
    void     (*getVersionComponents)(int* major, int* minor, int* patch) = nullptr;
    PmHandle (*create)(PmLoadProc loadProc, void* userData) = nullptr;   // ..._with_opengl_load_proc
    void     (*destroy)(PmHandle) = nullptr;
    void     (*loadPresetFile)(PmHandle, const char* filename, bool smoothTransition) = nullptr;
    void     (*setWindowSize)(PmHandle, std::size_t w, std::size_t h) = nullptr;
    void     (*setPresetLocked)(PmHandle, bool) = nullptr;
    void     (*setSoftCutDuration)(PmHandle, double seconds) = nullptr;
    void     (*setBeatSensitivity)(PmHandle, float) = nullptr;
    void     (*setMeshSize)(PmHandle, std::size_t w, std::size_t h) = nullptr;
    void     (*setFrameTime)(PmHandle, double secondsSinceFirstFrame) = nullptr;
    void     (*setFps)(PmHandle, std::int32_t) = nullptr;
    void     (*setAspectCorrection)(PmHandle, bool) = nullptr;
    void     (*setTextureSearchPaths)(PmHandle, const char** paths, std::size_t count) = nullptr;
    void     (*setSwitchFailedCallback)(PmHandle, PmSwitchFailedFn, void* userData) = nullptr;
    void     (*pcmAddFloat)(PmHandle, const float* samples, unsigned int countPerChannel, int channels) = nullptr;
    void     (*renderFrameFbo)(PmHandle, std::uint32_t framebufferId) = nullptr;
    void     (*burnTexture)(PmHandle, std::uint32_t texture, int left, int top, int width, int height) = nullptr;
};

// Only instance() should ever load the REAL library: a local object that loads successfully
// dlcloses the library at scope exit. Locals are for tests (fakes, or candidates that fail).
class ProjectMApi : public ProjectMFunctions {
public:
    static ProjectMApi& instance();                // the app-wide table (deliberately leaked; see .cpp)

    ProjectMApi() = default;
    // Not movable: a moved-from table would still look available() while its library handle,
    // and so every pointer in it, belongs to someone else. (Copy is already deleted via DynLib.)
    ProjectMApi(ProjectMApi&&) = delete;
    ProjectMApi& operator=(ProjectMApi&&) = delete;

    // Try each candidate until one opens, passes the version gate and resolves every symbol.
    // A no-op returning true once available. The library is never unloaded afterwards.
    bool loadFrom(const std::vector<std::string>& candidates);
    bool load(const std::string& prefPath);        // loadFrom(projectMCandidatePaths(prefPath, $HOME))

    bool available() const { return available_; }
    const std::string& statusText()  const { return status_; }       // why not / "projectM 4.2.0"
    const std::string& versionText() const { return version_; }      // "4.2.0"
    const std::string& loadedPath()  const { return loadedPath_; }

private:
    // Version gate + resolve every symbol from `lib` into `fns`. On failure `why` says what was
    // wrong and `fns` must be discarded (the caller closes `lib`).
    static bool bind(const DynLib& lib, ProjectMFunctions& fns, std::string& version, std::string& why);

    DynLib      lib_;
    bool        available_ = false;
    std::string status_ = "projectM not found";
    std::string version_, loadedPath_;
};

} // namespace oss
