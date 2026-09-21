#include "gfx/ProjectMApi.h"
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <type_traits>
#include <utility>

namespace oss {

namespace {
std::vector<std::string> platformLibraryNames() {
#if defined(_WIN32)
    return {"projectM-4.dll"};
#elif defined(__APPLE__)
    return {"libprojectM-4.dylib", "libprojectM-4.4.dylib"};
#else
    return {"libprojectM-4.so", "libprojectM-4.so.4"};
#endif
}
} // namespace

bool isSupportedProjectMVersion(int major, int minor) { return major == 4 && minor >= 2; }

std::vector<std::string> projectMCandidatePaths(const std::string& prefPath, const std::string& homeDir) {
    std::vector<std::string> out;
    // Only an ABSOLUTE preference is honoured: a relative one would resolve against the current
    // working directory, and preferences.oss is itself read from the CWD -- so a stray library
    // beside whatever directory the app happened to start in could hijack the load.
    if (!prefPath.empty() && std::filesystem::path(prefPath).is_absolute()) out.push_back(prefPath);
    const std::vector<std::string> names = platformLibraryNames();
    auto addDir = [&](const std::string& d) {
        for (const std::string& n : names) out.push_back(d + "/" + n);
    };
#if defined(_WIN32)
    // LoadLibrary resolves a bare name from the app directory + PATH -- the only mechanism here.
    (void)homeDir; (void)addDir;
    for (const std::string& n : names) out.push_back(n);
#else
    if (!homeDir.empty()) addDir(homeDir + "/.local/lib");   // the user's own build wins everywhere
  #if defined(__linux__)
    // A bare name goes through the ld.so cache + the standard directories -- the main install
    // mechanism on Linux -- and does NOT search the current working directory.
    for (const std::string& n : names) out.push_back(n);
  #else
    // No bare names on macOS: dlopen resolves one from the CURRENT WORKING DIRECTORY first
    // (measured), so a stray dylib sitting next to a project would be loaded ahead of the user's
    // own install. Explicit directories only here.
  #endif
    addDir("/usr/local/lib");
    addDir("/opt/homebrew/lib");
#endif
    return out;
}

ProjectMApi& ProjectMApi::instance() {
    // Deliberately leaked. A function-local static OBJECT would run ~DynLib (dlclose) during
    // static destruction -- after main returns and the GL contexts are gone -- and unloading a
    // GL-touching library at that point is the classic plugin crash-at-exit. The projectM library
    // is never unloaded; the OS reclaims it with the process.
    static ProjectMApi* api = new ProjectMApi();
    return *api;
}

bool ProjectMApi::load(const std::string& prefPath) {
#if defined(_WIN32)
    return loadFrom(projectMCandidatePaths(prefPath, ""), prefPath);
#else
    const char* home = std::getenv("HOME");
    return loadFrom(projectMCandidatePaths(prefPath, home ? home : ""), prefPath);
#endif
}

bool ProjectMApi::loadFrom(const std::vector<std::string>& candidates, const std::string& prefPath) {
    if (available_) return true;
    std::string rejection;                                        // why an opened library was refused
    std::string openError;                                        // why a library that EXISTS would not open
    for (const std::string& path : candidates) {
        DynLib lib;
        if (!lib.open(path)) {
            // A candidate that is simply absent is not worth reporting, but one that is there and
            // will not load (wrong architecture, a missing dependency of its own) is the whole
            // diagnostic -- "not found" would send the user looking in the wrong place.
            std::error_code ec;
            if (openError.empty() && std::filesystem::exists(path, ec))
                openError = "could not load " + path + ": " + lib.error();
            continue;
        }
        ProjectMFunctions fns;                                    // bound locally; adopted only on success
        std::string version, why;
        if (bind(lib, fns, version, why)) {
            static_cast<ProjectMFunctions&>(*this) = fns;
            lib_            = std::move(lib);                     // keeps the handle open: fns stay valid
            version_        = version;
            loadedPath_     = path;
            loadedPrefPath_ = prefPath;
            available_      = true;
            status_         = "projectM " + version_;
            return true;
        }
        if (rejection.empty()) rejection = why + " (" + path + ")";   // `lib` closes here; `fns` is dropped
    }
    // Precedence: a library we opened and refused says the most; then one we could not open at
    // all; only when nothing was even there is it "not found".
    if      (!rejection.empty()) status_ = rejection;
    else if (!openError.empty()) status_ = openError;
    else                         status_ = "projectM not found";
    return false;
}

bool ProjectMApi::bind(const DynLib& lib, ProjectMFunctions& f, std::string& version, std::string& why) {
    bool ok = true;
    auto resolve = [&](auto& fn, const char* name) {
        if (!ok) return;
        fn = lib.symbol<std::decay_t<decltype(fn)>>(name);
        if (!fn) { ok = false; why = std::string("missing symbol ") + name; }
    };

    // The version first, so an older library reports its version rather than a missing symbol.
    resolve(f.getVersionComponents, "projectm_get_version_components");
    if (!ok) return false;
    int major = 0, minor = 0, patch = 0;
    f.getVersionComponents(&major, &minor, &patch);
    version = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (!isSupportedProjectMVersion(major, minor)) {
        why = "found projectM " + version + ", needs 4.2+";
        return false;
    }

    resolve(f.create,                  "projectm_create_with_opengl_load_proc");
    resolve(f.destroy,                 "projectm_destroy");
    resolve(f.loadPresetFile,          "projectm_load_preset_file");
    resolve(f.setWindowSize,           "projectm_set_window_size");
    resolve(f.setPresetLocked,         "projectm_set_preset_locked");
    resolve(f.setSoftCutDuration,      "projectm_set_soft_cut_duration");
    resolve(f.setBeatSensitivity,      "projectm_set_beat_sensitivity");
    resolve(f.setMeshSize,             "projectm_set_mesh_size");
    resolve(f.setFrameTime,            "projectm_set_frame_time");
    resolve(f.setFps,                  "projectm_set_fps");
    resolve(f.setAspectCorrection,     "projectm_set_aspect_correction");
    resolve(f.setTextureSearchPaths,   "projectm_set_texture_search_paths");
    resolve(f.setSwitchFailedCallback, "projectm_set_preset_switch_failed_event_callback");
    resolve(f.pcmAddFloat,             "projectm_pcm_add_float");
    resolve(f.renderFrameFbo,          "projectm_opengl_render_frame_fbo");
    resolve(f.burnTexture,             "projectm_opengl_burn_texture");
    return ok;
}

} // namespace oss
