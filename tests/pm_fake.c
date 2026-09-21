/* A stand-in for libprojectM used ONLY by core_tests: it exports the 17 functions ProjectMApi
   binds, as no-ops, and reports version 4.PM_FAKE_MINOR.0. Built twice (4.1 and 4.2) so the tests
   can exercise the version gate, the rejection message and candidate fall-through without a real
   projectM install. Never linked into the app. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
  #define PM_FAKE_EXPORT __declspec(dllexport)
#else
  #define PM_FAKE_EXPORT __attribute__((visibility("default")))
#endif

#ifndef PM_FAKE_MINOR
  #error "PM_FAKE_MINOR must be defined (1 or 2)"
#endif

struct projectm;
typedef struct projectm* projectm_handle;
typedef void* (*projectm_load_proc)(const char* name, void* user_data);
typedef void (*projectm_preset_switch_failed_event)(const char* preset_filename, const char* message, void* user_data);

PM_FAKE_EXPORT void projectm_get_version_components(int* major, int* minor, int* patch) {
    if (major) *major = 4;
    if (minor) *minor = PM_FAKE_MINOR;
    if (patch) *patch = 0;
}
PM_FAKE_EXPORT projectm_handle projectm_create_with_opengl_load_proc(projectm_load_proc p, void* u) { (void)p; (void)u; return NULL; }
PM_FAKE_EXPORT void projectm_destroy(projectm_handle h) { (void)h; }
PM_FAKE_EXPORT void projectm_load_preset_file(projectm_handle h, const char* f, bool s) { (void)h; (void)f; (void)s; }
PM_FAKE_EXPORT void projectm_set_window_size(projectm_handle h, size_t w, size_t ht) { (void)h; (void)w; (void)ht; }
PM_FAKE_EXPORT void projectm_set_preset_locked(projectm_handle h, bool b) { (void)h; (void)b; }
PM_FAKE_EXPORT void projectm_set_soft_cut_duration(projectm_handle h, double d) { (void)h; (void)d; }
PM_FAKE_EXPORT void projectm_set_beat_sensitivity(projectm_handle h, float f) { (void)h; (void)f; }
PM_FAKE_EXPORT void projectm_set_mesh_size(projectm_handle h, size_t w, size_t ht) { (void)h; (void)w; (void)ht; }
PM_FAKE_EXPORT void projectm_set_frame_time(projectm_handle h, double d) { (void)h; (void)d; }
PM_FAKE_EXPORT void projectm_set_fps(projectm_handle h, int32_t f) { (void)h; (void)f; }
PM_FAKE_EXPORT void projectm_set_aspect_correction(projectm_handle h, bool b) { (void)h; (void)b; }
PM_FAKE_EXPORT void projectm_set_texture_search_paths(projectm_handle h, const char** p, size_t c) { (void)h; (void)p; (void)c; }
PM_FAKE_EXPORT void projectm_set_preset_switch_failed_event_callback(projectm_handle h, projectm_preset_switch_failed_event cb, void* u) { (void)h; (void)cb; (void)u; }
PM_FAKE_EXPORT void projectm_pcm_add_float(projectm_handle h, const float* s, unsigned int c, int ch) { (void)h; (void)s; (void)c; (void)ch; }
PM_FAKE_EXPORT void projectm_opengl_render_frame_fbo(projectm_handle h, uint32_t fbo) { (void)h; (void)fbo; }
PM_FAKE_EXPORT void projectm_opengl_burn_texture(projectm_handle h, uint32_t t, int l, int tp, int w, int ht) { (void)h; (void)t; (void)l; (void)tp; (void)w; (void)ht; }
