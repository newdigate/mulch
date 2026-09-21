#include "core/DynLib.h"

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace oss {

bool DynLib::open(const std::string& path) {
    close();
    error_.clear();
#if defined(_WIN32)
    handle_ = static_cast<void*>(LoadLibraryA(path.c_str()));
    if (!handle_)
        error_ = "LoadLibrary failed (" + std::to_string((unsigned long)GetLastError()) + "): " + path;
#else
    dlerror();                                  // discard any pending error so the one we read is dlopen's
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        const char* e = dlerror();
        error_ = e ? std::string(e) : ("dlopen failed: " + path);
    }
#endif
    return handle_ != nullptr;
}

void DynLib::close() {
    if (!handle_) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
}

void* DynLib::rawSymbol(const char* name) const {
    if (!handle_) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return dlsym(handle_, name);
#endif
}

} // namespace oss
