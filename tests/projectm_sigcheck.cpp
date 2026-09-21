// Compile-time comparison of oss::ProjectMFunctions against the REAL projectM headers.
// ProjectMApi re-declares 17 C functions by hand and resolves them by name; a name lookup cannot
// catch a wrong signature, and a mismatch is silent corruption at call time. This translation unit
// is compile-only (an OBJECT library, never linked, never shipped) and is built only where the
// projectM-4 headers are installed. When you add a binding to ProjectMFunctions, add a SIGCHECK.
#include "gfx/ProjectMApi.h"

#include <projectM-4/core.h>
#include <projectM-4/audio.h>
#include <projectM-4/parameters.h>
#include <projectM-4/render_opengl.h>
#include <projectM-4/callbacks.h>
#include <projectM-4/types.h>

#include <type_traits>

// Map projectM's opaque handle onto ours, and its channel enum onto the int we pass,
// so only a GENUINE type difference can fail the asserts below.
template <class T> struct Rw { using type = T; };
template <> struct Rw<projectm_handle>   { using type = oss::PmHandle; };
template <> struct Rw<projectm_channels> { using type = int; };

template <class F> struct RwFn;
template <class R, class... A> struct RwFn<R (*)(A...)> {
    using type = typename Rw<R>::type (*)(typename Rw<A>::type...);
};

#define SIGCHECK(member, fn)                                                            \
    static_assert(std::is_same<RwFn<decltype(&fn)>::type,                               \
                               decltype(oss::ProjectMFunctions::member)>::value,        \
                  "signature mismatch: " #member " vs " #fn)

SIGCHECK(getVersionComponents,   projectm_get_version_components);
SIGCHECK(create,                 projectm_create_with_opengl_load_proc);
SIGCHECK(destroy,                projectm_destroy);
SIGCHECK(loadPresetFile,         projectm_load_preset_file);
SIGCHECK(setWindowSize,          projectm_set_window_size);
SIGCHECK(setPresetLocked,        projectm_set_preset_locked);
SIGCHECK(setSoftCutDuration,     projectm_set_soft_cut_duration);
SIGCHECK(setBeatSensitivity,     projectm_set_beat_sensitivity);
SIGCHECK(setMeshSize,            projectm_set_mesh_size);
SIGCHECK(setFrameTime,           projectm_set_frame_time);
SIGCHECK(setFps,                 projectm_set_fps);
SIGCHECK(setAspectCorrection,    projectm_set_aspect_correction);
SIGCHECK(setTextureSearchPaths,  projectm_set_texture_search_paths);
SIGCHECK(setSwitchFailedCallback,projectm_set_preset_switch_failed_event_callback);
SIGCHECK(pcmAddFloat,            projectm_pcm_add_float);
SIGCHECK(renderFrameFbo,         projectm_opengl_render_frame_fbo);
SIGCHECK(burnTexture,            projectm_opengl_burn_texture);

// The two deliberate deviations, checked for ABI equivalence rather than type identity.
static_assert(sizeof(projectm_channels) == sizeof(int), "channel enum is not int-sized");
static_assert(alignof(projectm_channels) == alignof(int), "channel enum alignment differs");
static_assert(static_cast<int>(PROJECTM_STEREO) == oss::kPmStereo, "kPmStereo != PROJECTM_STEREO");
static_assert(static_cast<int>(PROJECTM_MONO) == 1, "PROJECTM_MONO != 1");
static_assert(std::is_same<oss::PmLoadProc, projectm_load_proc>::value, "load proc typedef");
static_assert(std::is_same<oss::PmSwitchFailedFn, projectm_preset_switch_failed_event>::value, "cb typedef");
static_assert(sizeof(bool) == 1, "C++ bool size");
