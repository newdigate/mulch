#pragma once

// A shared library that exists on every machine of each platform and exports `cos`.
// Used by the DynLib and ProjectMApi tests as "a real library that is not projectM".
#if defined(_WIN32)
constexpr const char* kSystemLibPath = "ucrtbase.dll";
#elif defined(__APPLE__)
constexpr const char* kSystemLibPath = "/usr/lib/libSystem.B.dylib";
#else
constexpr const char* kSystemLibPath = "libm.so.6";
#endif
