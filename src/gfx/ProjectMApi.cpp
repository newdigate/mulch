#include "gfx/ProjectMApi.h"
#include <cstdlib>
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
    if (!prefPath.empty()) out.push_back(prefPath);
    const std::vector<std::string> names = platformLibraryNames();
    // Bare names go through the system loader: on Linux that is the ld.so cache (the main
    // mechanism there); on Windows the app dir + PATH; on current macOS almost nothing (dyld no
    // longer applies the old /usr/local/lib fallback), so there the explicit dirs below do the work.
    for (const std::string& n : names) out.push_back(n);
#if !defined(_WIN32)
    std::vector<std::string> dirs;
    if (!homeDir.empty()) dirs.push_back(homeDir + "/.local/lib");     // the user's own build wins
    dirs.push_back("/usr/local/lib");
    dirs.push_back("/opt/homebrew/lib");
    for (const std::string& d : dirs)
        for (const std::string& n : names) out.push_back(d + "/" + n);
#else
    (void)homeDir;
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
    for (const std::string& path : candidates) {
        DynLib lib;
        if (!lib.open(path)) continue;
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
    status_ = rejection.empty() ? std::string("projectM not found") : rejection;
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
