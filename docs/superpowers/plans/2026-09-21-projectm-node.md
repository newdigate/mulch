# projectM Node Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a **projectM** texture node (audio in → Milkdrop visuals out) that loads libprojectM 4.2+ at runtime with `dlopen`, so it exists only where the user has installed projectM.

**Architecture:** A GL-free dynamic-library wrapper (`core/DynLib`) backs a process-wide symbol table (`gfx/ProjectMApi`) with a version gate. A GL-free `core/PresetPlaylist.h` holds the folder listing, the bar-sync math and a `PresetSelector` state machine, so all preset-selection logic is unit-tested without GL. `modules/ProjectMNode` owns an FBO and a projectM instance, feeds it interleaved stereo PCM, optionally burns an input texture into its canvas, and renders straight into the FBO inside a `gfx/GLStateGuard`.

**Tech Stack:** C++17, OpenGL 4.1 core, GLFW, Dear ImGui, doctest, CMake. libprojectM 4.2 (master, LGPL-2.1) — runtime-loaded only; never linked, never bundled, no projectM headers included.

**Spec:** `docs/superpowers/specs/2026-09-21-projectm-node-design.md`

**Branch:** `feat/projectm-node` (already created from `develop`; the spec is committed there).

## Deviations from the spec (decided while planning)

1. **`DynLib` is `.h` + `.cpp`, not header-only.** `<windows.h>` must stay out of headers: `tests/gl_smoke.cpp` has a helper named `near`, which `<windows.h>` `#define`s away, and `gl_smoke.cpp` will include the node header.
2. **`PresetSelector` is added to `core/PresetPlaylist.h`.** The preset-selection state machine (incoming change / buttons / bar-sync) is pulled out of the node so it is unit-tested in `core_tests`, which CI runs. The node only calls `update()`.
3. **Turning `sync` on primes, it does not jump.** The first synced frame records the current step and switches nothing; the first switch is at the next bar boundary. This lets a saved `preset` survive a project load (same idea as the Drum Machine's primed `pattern` port).
4. **The node-level playlist write-back check is in the always-run `gl_smoke` group** (it needs no library), so CI covers it. Only the `(2/3)` status check needs the library.
5. **An empty `preset` loads `idle://`** (projectM's built-in idle preset) rather than leaving the last preset running.

## File structure

| File | Status | Responsibility |
|---|---|---|
| `src/core/DynLib.h` / `.cpp` | create | RAII `dlopen`/`LoadLibrary` wrapper. GL-free. |
| `src/core/PresetPlaylist.h` | create | `.milk` folder listing, index/step helpers, bar-sync math, `PresetSelector`. GL-free, header-only. |
| `src/gfx/ProjectMApi.h` / `.cpp` | create | Symbol table + version gate + candidate search paths. No GL/GLFW headers. |
| `src/gfx/GLStateGuard.h` | create | RAII save/restore of GL state around foreign rendering. |
| `src/gfx/Framebuffer.h` | modify | add `id()` accessor. |
| `src/modules/ProjectMNode.h` / `.cpp` | create | The node. |
| `src/core/AssetLibrary.h` | modify | `AssetType::Preset` (= 5), `kAssetTypeCount` = 6. |
| `src/ui/AssetsPanel.cpp`, `src/ui/PropertiesPanel.cpp` | modify | "Presets" tab; type label. |
| `src/core/Preferences.h` / `.cpp` | modify | `projectMLibraryPath`, `projectMTexturesDir` + codec. |
| `src/ui/PreferencesPanel.cpp` | modify | Locations tab rows + status. |
| `src/app/Application.cpp` | modify | load API at startup, retry on pref change, `makeNode`, dynamic `nodeCategories`. |
| `CMakeLists.txt` | modify | new sources in all three targets; `${CMAKE_DL_LIBS}`. |
| `tests/system_lib.h`, `tests/test_dynlib.cpp`, `tests/test_preset_playlist.cpp`, `tests/test_projectm_api.cpp` | create | unit tests. |
| `tests/test_asset_library_file.cpp`, `tests/test_preferences.cpp`, `tests/gl_smoke.cpp` | modify | new cases / scenarios. |
| `CLAUDE.md`, `README.md` | modify | docs. |

## Conventions for every task

- Build: `cmake --build build -j` (re-configures itself after a `CMakeLists.txt` edit). If `build/` does not exist: `cmake -S . -B build` first.
- Run one doctest case: `./build/core_tests -tc="<pattern>"`. Run all: `ctest --test-dir build --output-on-failure`.
- `gl_smoke` must run from the repo root: `./build/gl_smoke`.
- "Verify it fails" for a new C++ file means **the build fails** (missing header / undefined symbol). That is the red state.
- Commit messages: Conventional Commits, ending with the line
  `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Never `git add -A`: the working tree has unrelated untracked files (`build.sh`, `examples/`, `preferences.oss`, `project.oss`). Add only the paths each task names.

---

### Task 1: Build projectM 4.2 (master) locally — no commit

Needed only for the library-dependent `gl_smoke` checks (Task 9) and the manual run (Task 11). Tasks 2–8 and 10 do not need it. It clones from the network and installs into `~/.local`; **confirm with the user before running it.** If it is skipped, Task 9's checks print SKIP and Task 11's manual run is deferred.

**Files:** none in this repo.

- [ ] **Step 1: Clone the pinned commit**

The commit is projectM `master` HEAD as of 2026-09-10. It has **not** been built or tested by the plan author; this task is where that happens.

```bash
mkdir -p ~/src && cd ~/src && git clone --recurse-submodules https://github.com/projectM-visualizer/projectm.git projectm-master && cd projectm-master && git checkout 1e7ef7803b69024d1e0656705670adda2ffac817 && git submodule update --init --recursive
```

- [ ] **Step 2: Configure, build, install into `~/.local`**

```bash
cd ~/src/projectm-master && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF && cmake --build build -j && cmake --install build
```

Expected: the install step prints `Installing: .../.local/lib/libprojectM-4...`.

- [ ] **Step 3: Record the real library file name and confirm the 4.2 symbols**

```bash
ls ~/.local/lib | grep -i projectm
```

```bash
nm -gU ~/.local/lib/libprojectM-4.dylib | grep -E 'projectm_opengl_render_frame_fbo|projectm_opengl_burn_texture|projectm_set_frame_time|projectm_create_with_opengl_load_proc'
```

Expected: four lines (each symbol, `_`-prefixed on macOS). On Linux use `nm -D --defined-only ~/.local/lib/libprojectM-4.so`.

If the file is **not** named `libprojectM-4.dylib` (macOS) / `libprojectM-4.so` (Linux), write the real name down and use it in `platformLibraryNames()` in Task 4 Step 3. If any of the four symbols is missing, stop and report to the user: the pinned commit does not provide the 4.2 API the spec relies on.

---

### Task 2: `core/DynLib` — runtime library wrapper

**Files:**
- Create: `src/core/DynLib.h`, `src/core/DynLib.cpp`
- Create: `tests/system_lib.h`, `tests/test_dynlib.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the shared test constant**

Create `tests/system_lib.h`:

```cpp
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
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_dynlib.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/DynLib.h"
#include "system_lib.h"
#include <utility>

using namespace oss;

TEST_CASE("DynLib: a nonexistent path fails cleanly with an error message") {
    DynLib lib;
    CHECK_FALSE(lib.open("/nonexistent/dir/libnope-12345.so"));
    CHECK_FALSE(lib.isOpen());
    CHECK_FALSE(lib.error().empty());
    CHECK(lib.symbol<double (*)(double)>("cos") == nullptr);   // closed -> null, no crash
}

TEST_CASE("DynLib: opens a system library, resolves and calls a symbol") {
    DynLib lib;
    REQUIRE(lib.open(kSystemLibPath));
    CHECK(lib.isOpen());
    CHECK(lib.error().empty());
    auto cosFn = lib.symbol<double (*)(double)>("cos");
    REQUIRE(cosFn != nullptr);
    CHECK(cosFn(0.0) == doctest::Approx(1.0));
}

TEST_CASE("DynLib: a missing symbol returns null") {
    DynLib lib;
    REQUIRE(lib.open(kSystemLibPath));
    CHECK(lib.symbol<void (*)()>("oss_definitely_not_a_symbol_xyz") == nullptr);
}

TEST_CASE("DynLib: move transfers ownership") {
    DynLib a;
    REQUIRE(a.open(kSystemLibPath));
    DynLib b(std::move(a));
    CHECK_FALSE(a.isOpen());
    CHECK(b.isOpen());
    DynLib c;
    c = std::move(b);
    CHECK_FALSE(b.isOpen());
    CHECK(c.symbol<double (*)(double)>("cos") != nullptr);
}
```

- [ ] **Step 3: Add the files to CMake**

In `CMakeLists.txt`:

1. In `set(APP_SOURCES ...)`, after the line `  src/core/VertexTrail.cpp`, add:
   ```cmake
     src/core/DynLib.cpp
   ```
2. In `add_executable(core_tests ...)`, after the line `  tests/test_spirograph.cpp`, add:
   ```cmake
     tests/test_dynlib.cpp
     src/core/DynLib.cpp
   ```
3. In `add_executable(gl_smoke ...)`, after its `  src/core/VertexTrail.cpp` line, add:
   ```cmake
     src/core/DynLib.cpp
   ```
4. Replace
   ```cmake
   target_link_libraries(core_tests PRIVATE glm::glm doctest::doctest)
   ```
   with
   ```cmake
   target_link_libraries(core_tests PRIVATE glm::glm doctest::doctest ${CMAKE_DL_LIBS})
   ```
   (`tests/test_dynlib.cpp` includes `"system_lib.h"` by quoted name, which resolves next to the test file; no include-path change is needed.)
5. Append ` ${CMAKE_DL_LIBS}` inside the closing parenthesis of **both** the `target_link_libraries(shader_streamer PRIVATE ...)` call (ends with `nfd)`) and the `target_link_libraries(gl_smoke PRIVATE ...)` call (ends with `PkgConfig::FFMPEG)`).

- [ ] **Step 4: Verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `core/DynLib.h` / `src/core/DynLib.cpp` not found.

- [ ] **Step 5: Write `src/core/DynLib.h`**

```cpp
#pragma once
#include <string>
#include <utility>

namespace oss {

// RAII handle to a shared library opened at runtime (dlopen / LoadLibrary). GL-free.
// The platform headers live in DynLib.cpp so <windows.h> never leaks into a header.
class DynLib {
public:
    DynLib() = default;
    ~DynLib() { close(); }
    DynLib(const DynLib&) = delete;
    DynLib& operator=(const DynLib&) = delete;
    DynLib(DynLib&& o) noexcept : handle_(o.handle_), error_(std::move(o.error_)) { o.handle_ = nullptr; }
    DynLib& operator=(DynLib&& o) noexcept {
        if (this != &o) { close(); handle_ = o.handle_; error_ = std::move(o.error_); o.handle_ = nullptr; }
        return *this;
    }

    bool open(const std::string& path);   // false on failure; error() says why
    void close();
    bool isOpen() const { return handle_ != nullptr; }
    const std::string& error() const { return error_; }

    // Resolve `name` as a function pointer of type T; nullptr when closed or missing.
    template <class T> T symbol(const char* name) const { return reinterpret_cast<T>(rawSymbol(name)); }

private:
    void* rawSymbol(const char* name) const;
    void*       handle_ = nullptr;
    std::string error_;
};

} // namespace oss
```

- [ ] **Step 6: Write `src/core/DynLib.cpp`**

```cpp
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
```

- [ ] **Step 7: Verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests -tc="DynLib*"`
Expected: PASS, 4 test cases.

- [ ] **Step 8: Commit**

```bash
git add src/core/DynLib.h src/core/DynLib.cpp tests/system_lib.h tests/test_dynlib.cpp CMakeLists.txt && git commit -m "$(cat <<'EOF'
feat(core): DynLib runtime shared-library wrapper

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: `core/PresetPlaylist.h` — listing, sync math, `PresetSelector`

**Files:**
- Create: `src/core/PresetPlaylist.h`
- Create: `tests/test_preset_playlist.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_preset_playlist.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/PresetPlaylist.h"
#include "core/PathUtil.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace oss;
namespace fs = std::filesystem;

// A fixed three-preset folder for the selector tests (no disk access).
static std::vector<std::string> fakeLister(const std::string& dir) {
    if (dir == "/p") return {"/p/a.milk", "/p/b.milk", "/p/c.milk"};
    return {};
}

TEST_CASE("listPresetsInDir: only .milk, case-insensitive, sorted, no subfolders") {
    fs::path dir = fs::temp_directory_path() / "oss_preset_playlist_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "sub");
    for (const char* n : {"b.milk", "A.MILK", "notes.txt"}) std::ofstream(dir / n) << "x";
    std::ofstream(dir / "sub" / "x.milk") << "x";

    std::vector<std::string> files = listPresetsInDir(dir.string());
    REQUIRE(files.size() == 2);
    CHECK(fileBaseName(files[0]) == "A.MILK");
    CHECK(fileBaseName(files[1]) == "b.milk");
    CHECK(files[0] == dir.string() + "/A.MILK");   // paths are dir + "/" + name

    CHECK(listPresetsInDir((dir / "missing").string()).empty());
    CHECK(listPresetsInDir("").empty());
    fs::remove_all(dir);
}

TEST_CASE("indexOfPreset matches by file name; stepPresetIndex wraps") {
    std::vector<std::string> f = {"/p/a.milk", "/p/b.milk", "/p/c.milk"};
    CHECK(indexOfPreset(f, "/p/b.milk") == 1);
    CHECK(indexOfPreset(f, "C:\\p\\c.milk") == 2);   // separators differ, name matches
    CHECK(indexOfPreset(f, "/p/zzz.milk") == -1);
    CHECK(indexOfPreset(f, "") == -1);

    CHECK(stepPresetIndex(0, -1, 3) == 2);
    CHECK(stepPresetIndex(2, +1, 3) == 0);
    CHECK(stepPresetIndex(1, +1, 3) == 2);
    CHECK(stepPresetIndex(0, +1, 0) == -1);   // empty list
}

TEST_CASE("syncedPresetStep advances every N bars and is stateless") {
    CHECK(syncedPresetStep(0.0, 4) == 0);
    CHECK(syncedPresetStep(3.99, 4) == 0);
    CHECK(syncedPresetStep(4.0, 4) == 1);
    CHECK(syncedPresetStep(9.0, 4) == 2);
    CHECK(syncedPresetStep(-0.5, 4) == -1);
    CHECK(syncedPresetStep(2.5, 0) == 2);      // N < 1 is treated as 1
    CHECK(syncedPresetStep(4.0, 4) == syncedPresetStep(4.0, 4));
}

TEST_CASE("syncedPresetIndex: sequential is a positive modulo; shuffle is deterministic and in range") {
    CHECK(syncedPresetIndex(0, 3, false, kPresetShuffleSeed) == 0);
    CHECK(syncedPresetIndex(4, 3, false, kPresetShuffleSeed) == 1);
    CHECK(syncedPresetIndex(-1, 3, false, kPresetShuffleSeed) == 2);
    CHECK(syncedPresetIndex(7, 0, false, kPresetShuffleSeed) == 0);   // empty list

    std::set<int> seen;
    for (long long s = -50; s <= 50; ++s) {
        int i = syncedPresetIndex(s, 5, true, kPresetShuffleSeed);
        CHECK(i >= 0);
        CHECK(i < 5);
        CHECK(i == syncedPresetIndex(s, 5, true, kPresetShuffleSeed));   // same in, same out
        seen.insert(i);
    }
    CHECK(seen.size() >= 3);   // it actually shuffles
}

TEST_CASE("PresetSelector: an incoming change loads once") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/b.milk";
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/b.milk");
    CHECK(sel.count() == 3);
    CHECK(sel.index() == 1);
    CHECK_FALSE(sel.update(in));   // unchanged -> nothing to do
}

TEST_CASE("PresetSelector: next / prev wrap, and the written-back path does not reload") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/b.milk";
    sel.update(in);

    in.button = 1;                       // next
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/c.milk");

    in.button = -1;
    in.incoming = sel.current();         // the node wrote the path back into the field
    CHECK_FALSE(sel.update(in));

    in.button = 1;                       // next wraps to the first
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/a.milk");

    in.incoming = sel.current();
    in.button = 0;                       // prev wraps to the last
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/c.milk");
}

TEST_CASE("PresetSelector: a step holds against an unchanged edge value") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/a.milk";           // an edge keeps sending this
    CHECK(sel.update(in));
    in.button = 1;
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/b.milk");
    in.button = -1;
    CHECK_FALSE(sel.update(in));         // not snapped back to a.milk
    CHECK(sel.current() == "/p/b.milk");
}

TEST_CASE("PresetSelector: buttons and sync are no-ops without a folder") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.button = 1;
    in.sync = true; in.playing = true; in.bars = 8.0; in.everyNBars = 1;
    CHECK_FALSE(sel.update(in));
    in.bars = 9.0;
    CHECK_FALSE(sel.update(in));
    CHECK(sel.current().empty());
    CHECK(sel.index() == -1);
}

TEST_CASE("PresetSelector: random never repeats the current preset when there is a choice") {
    for (unsigned rv = 0; rv < 10; ++rv) {
        PresetSelector sel(&fakeLister);
        PresetSelectorInput in;
        in.incoming = "/p/a.milk";
        sel.update(in);
        in.button = 2;
        in.randomValue = rv;
        CHECK(sel.update(in));
        CHECK(sel.current() != "/p/a.milk");
    }
}

TEST_CASE("PresetSelector: sync primes, then switches on a bar boundary to an absolute position") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/c.milk";
    in.sync = true; in.playing = true; in.everyNBars = 1;

    in.bars = 0.5;
    CHECK(sel.update(in));               // the incoming load only; sync just primes
    CHECK(sel.current() == "/p/c.milk");

    in.bars = 0.9;
    CHECK_FALSE(sel.update(in));         // same step

    in.bars = 1.1;                       // step 1 -> files[1]
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/b.milk");

    in.button = 0; in.bars = 1.6;        // prev by hand inside the step
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/a.milk");
    in.button = -1; in.bars = 1.9;
    CHECK_FALSE(sel.update(in));         // holds until the next boundary
    CHECK(sel.current() == "/p/a.milk");

    in.bars = 2.0;                       // step 2 -> files[2]
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/c.milk");

    in.bars = 0.2;                       // seek back: step 0 -> files[0]
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/a.milk");
}

TEST_CASE("PresetSelector: sync ignores a stopped transport and re-primes on play") {
    PresetSelector sel(&fakeLister);
    PresetSelectorInput in;
    in.incoming = "/p/a.milk";
    in.sync = true; in.everyNBars = 1;
    in.playing = true;  in.bars = 0.1; sel.update(in);   // primes at step 0
    in.playing = false; in.bars = 5.3;
    CHECK_FALSE(sel.update(in));                          // stopped: no switch
    in.playing = true;
    CHECK_FALSE(sel.update(in));                          // re-primes at step 5
    in.bars = 6.0;                                        // step 6 -> files[0], already current
    CHECK_FALSE(sel.update(in));
    in.bars = 7.0;                                        // step 7 -> files[1]
    CHECK(sel.update(in));
    CHECK(sel.current() == "/p/b.milk");
}
```

- [ ] **Step 2: Add the test to CMake**

In `add_executable(core_tests ...)`, after the `  tests/test_dynlib.cpp` line added in Task 2, add:

```cmake
  tests/test_preset_playlist.cpp
```

- [ ] **Step 3: Verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `core/PresetPlaylist.h` not found.

- [ ] **Step 4: Write `src/core/PresetPlaylist.h`**

```cpp
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#include "core/PathUtil.h"

namespace oss {

// Fixed seed for the bar-synced shuffle, so the order is identical in every session (node ids are
// remapped on project load, so they are not a stable seed).
inline constexpr unsigned kPresetShuffleSeed = 0x9E3779B9u;

namespace detail {
inline std::string lowered(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
inline bool hasMilkExtension(const std::string& name) {
    return name.size() > 5 && lowered(name.substr(name.size() - 5)) == ".milk";
}
} // namespace detail

// The Milkdrop presets directly inside `dir` (no recursion): "*.milk", extension matched
// case-insensitively, sorted case-insensitively by file name. Each entry is dir + "/" + name.
// A missing or empty `dir` yields an empty list.
inline std::vector<std::string> listPresetsInDir(const std::string& dir) {
    std::vector<std::string> names;
    if (dir.empty()) return names;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        std::string name = it->path().filename().string();
        if (detail::hasMilkExtension(name)) names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        std::string la = detail::lowered(a), lb = detail::lowered(b);
        return la != lb ? la < lb : a < b;
    });
    for (std::string& n : names) n = dir + "/" + n;
    return names;
}

// Index of `path` in `files`, matched by file name (one folder, so names are unique; this also
// survives '/' vs '\\' differences). -1 when absent.
inline int indexOfPreset(const std::vector<std::string>& files, const std::string& path) {
    if (path.empty()) return -1;
    std::string want = fileBaseName(path);
    for (std::size_t i = 0; i < files.size(); ++i)
        if (fileBaseName(files[i]) == want) return (int)i;
    return -1;
}

// index + delta, wrapped into [0, count). count <= 0 -> -1.
inline int stepPresetIndex(int index, int delta, int count) {
    if (count <= 0) return -1;
    long long i = ((long long)index + delta) % count;
    return (int)((i + count) % count);
}

// Which bar-synced step song position `bars` falls in: floor(bars / N). N < 1 is treated as 1.
inline long long syncedPresetStep(double bars, int everyNBars) {
    double n = (double)(everyNBars < 1 ? 1 : everyNBars);
    return (long long)std::floor(bars / n);
}

// The preset index for a synced `step`. Sequential: step mod count (an ABSOLUTE position in the
// sorted folder). Shuffle: a hash of (step, seed), so it replays identically after a loop or seek.
inline int syncedPresetIndex(long long step, int count, bool shuffle, unsigned seed) {
    if (count <= 0) return 0;
    if (!shuffle) return (int)(((step % count) + count) % count);
    unsigned long long z = (unsigned long long)step + 0x9E3779B97F4A7C15ULL * ((unsigned long long)seed + 1ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;   // splitmix64 finalizer
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= (z >> 31);
    return (int)(z % (unsigned long long)count);
}

// One frame's inputs to the selector.
struct PresetSelectorInput {
    std::string incoming;          // the resolved `preset` input value this frame
    int      button      = -1;     // -1 none, 0 prev, 1 next, 2 random
    bool     sync        = false;
    bool     playing     = false;  // transport running
    double   bars        = 0.0;    // transport position in bars
    int      everyNBars  = 4;
    bool     shuffle     = false;  // applies to the synced step
    unsigned randomValue = 0;      // caller-supplied random number, used by the random button
};

// Decides which preset should be playing. The playlist is the folder of the current preset.
//   - a CHANGE in `incoming` selects it (field edit, asset pick, or an edge);
//   - a button steps through the folder;
//   - sync is edge-triggered and primed: the first synced frame only records the step, and a
//     switch happens when the step number later changes (bar boundary, loop seam, seek).
// GL-free; the directory lister is injectable for tests.
class PresetSelector {
public:
    using Lister = std::vector<std::string> (*)(const std::string& dir);
    explicit PresetSelector(Lister lister = &listPresetsInDir) : lister_(lister) {}

    // Returns true when current() changed this frame.
    bool update(const PresetSelectorInput& in) {
        bool changed = false;
        if (in.incoming != lastIncoming_) {
            lastIncoming_ = in.incoming;
            if (in.incoming != current_) { current_ = in.incoming; changed = true; }
        }
        rescanIfFolderChanged();
        const int n = (int)files_.size();

        if (in.button >= 0 && n > 0) {
            int i = indexOfPreset(files_, current_);
            int next;
            if (in.button == 2) {
                next = (int)(in.randomValue % (unsigned)n);
                if (n > 1 && next == i) next = (next + 1) % n;
            } else if (i < 0) {
                next = (in.button == 0) ? n - 1 : 0;
            } else {
                next = stepPresetIndex(i, in.button == 0 ? -1 : +1, n);
            }
            changed = select(files_[(std::size_t)next]) || changed;
        }

        if (in.sync && in.playing && n > 0) {
            long long step = syncedPresetStep(in.bars, in.everyNBars);
            if (!syncPrimed_) { syncPrimed_ = true; lastStep_ = step; }
            else if (step != lastStep_) {
                lastStep_ = step;
                int idx = syncedPresetIndex(step, n, in.shuffle, kPresetShuffleSeed);
                changed = select(files_[(std::size_t)idx]) || changed;
            }
        } else {
            syncPrimed_ = false;
        }
        return changed;
    }

    const std::string& current() const { return current_; }
    int count() const { return (int)files_.size(); }
    int index() const { return indexOfPreset(files_, current_); }

private:
    bool select(const std::string& path) {
        if (path == current_) return false;
        current_ = path;
        return true;
    }
    void rescanIfFolderChanged() {
        std::string dir = parentDir(current_);
        if (dir == folder_) return;
        folder_ = dir;
        files_  = dir.empty() ? std::vector<std::string>{} : lister_(dir);
    }

    Lister                   lister_;
    std::vector<std::string> files_;
    std::string              folder_, current_, lastIncoming_;
    bool                     syncPrimed_ = false;
    long long                lastStep_   = 0;
};

} // namespace oss
```

- [ ] **Step 5: Verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests -tc="listPresetsInDir*,indexOfPreset*,syncedPreset*,PresetSelector*"`
Expected: PASS, 11 test cases (`listPresetsInDir…`, `indexOfPreset…`, `syncedPresetStep…`, `syncedPresetIndex…`, and 7 `PresetSelector…`).

- [ ] **Step 6: Commit**

```bash
git add src/core/PresetPlaylist.h tests/test_preset_playlist.cpp CMakeLists.txt && git commit -m "$(cat <<'EOF'
feat(core): PresetPlaylist (folder listing, bar-sync math, PresetSelector)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

#### Task 3 — post-review amendments (applied in a follow-up commit)

The code-quality review measured problems in the code above; the committed header differs from the
listing in these ways (the tests gained seven cases and the listing test cleans up before asserting):

- `PresetSelector::index()` is **cached** (`index_`, refreshed in `select()` and on a folder rescan).
  The uncached scan cost ~1 ms/frame with a 4,000-preset folder, and the node calls it twice a frame.
- `indexOfPreset` compares `std::string_view` base names — no allocation per element.
- `listPresetsInDir` lowercases each name once (decorate-sort-undecorate) and filters with
  `path().extension()`.
- `syncedPresetStep` returns 0 for a non-finite / out-of-range position (the cast was undefined).
- Sync **re-primes instead of switching** when `everyNBars` changes. (A pick in another folder is
  covered by the manual-action rule below: the folder can only change through a manual pick.)
- A pick or a button in a frame **outranks** a sync boundary in that frame; the boundary is consumed.
- Button values outside 0..2 are ignored.

Declined: a public `rescan()`, a button enum, case-insensitive name matching (`a.milk` and `A.milk`
are different files on Linux).

---

### Task 4: `gfx/ProjectMApi` — symbol table + version gate

**Files:**
- Create: `src/gfx/ProjectMApi.h`, `src/gfx/ProjectMApi.cpp`
- Create: `tests/test_projectm_api.cpp`
- Modify: `CMakeLists.txt`

The projectM signatures below were read from the projectM `master` headers on 2026-09-21 (`src/api/include/projectM-4/{core,audio,parameters,render_opengl,callbacks,types}.h`). We re-declare them; no projectM header is included.

- [ ] **Step 1: Write the failing test**

Create `tests/test_projectm_api.cpp`:

```cpp
#include <doctest/doctest.h>
#include "gfx/ProjectMApi.h"
#include "system_lib.h"
#include <string>
#include <vector>

using namespace oss;

TEST_CASE("projectM version gate: major must be 4, minor at least 2") {
    CHECK_FALSE(isSupportedProjectMVersion(4, 1));   // 4.1.7, the latest release
    CHECK(isSupportedProjectMVersion(4, 2));
    CHECK(isSupportedProjectMVersion(4, 9));
    CHECK_FALSE(isSupportedProjectMVersion(5, 0));   // unknown ABI
    CHECK_FALSE(isSupportedProjectMVersion(3, 1));
}

TEST_CASE("projectM candidate paths: the preference comes first; no empty entries") {
    std::vector<std::string> c = projectMCandidatePaths("/opt/pm/libprojectM-4.dylib", "/home/me");
    REQUIRE_FALSE(c.empty());
    CHECK(c.front() == "/opt/pm/libprojectM-4.dylib");
    for (const std::string& s : c) CHECK_FALSE(s.empty());

    std::vector<std::string> d = projectMCandidatePaths("", "/home/me");
    REQUIRE_FALSE(d.empty());
    CHECK(d.size() == c.size() - 1);                 // only the preference entry differs
#if !defined(_WIN32)
    bool sawHome = false;
    for (const std::string& s : d) if (s.rfind("/home/me/.local/lib/", 0) == 0) sawHome = true;
    CHECK(sawHome);
#endif
}

TEST_CASE("ProjectMApi: nothing to open -> unavailable, 'not found'") {
    ProjectMApi api;
    CHECK_FALSE(api.available());
    CHECK_FALSE(api.loadFrom({"/nonexistent/dir/libprojectM-4.dylib"}));
    CHECK_FALSE(api.available());
    CHECK(api.statusText() == "projectM not found");
}

TEST_CASE("ProjectMApi: a real library that is not projectM is rejected by name") {
    ProjectMApi api;
    CHECK_FALSE(api.loadFrom({kSystemLibPath}));
    CHECK_FALSE(api.available());
    CHECK(api.statusText() == "missing symbol projectm_get_version_components");
    CHECK(api.getVersionComponents == nullptr);      // a rejected library leaves the table empty
    CHECK(api.create == nullptr);
    CHECK(api.renderFrameFbo == nullptr);
}
```

- [ ] **Step 2: Add the files to CMake**

1. `APP_SOURCES`: after `  src/gfx/ImageLoader.cpp` add `  src/gfx/ProjectMApi.cpp`.
2. `core_tests`: after `  tests/test_preset_playlist.cpp` add:
   ```cmake
     tests/test_projectm_api.cpp
     src/gfx/ProjectMApi.cpp
   ```
3. `gl_smoke`: after its `  src/gfx/ImageLoader.cpp` line add `  src/gfx/ProjectMApi.cpp`.

- [ ] **Step 3: Verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `gfx/ProjectMApi.h` not found.

- [ ] **Step 4: Write `src/gfx/ProjectMApi.h`**

```cpp
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
// the system loader, then well-known lib directories (non-Windows; `homeDir` may be empty).
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

class ProjectMApi : public ProjectMFunctions {
public:
    static ProjectMApi& instance();                // the app-wide table (deliberately leaked; see .cpp)

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
```

- [ ] **Step 5: Write `src/gfx/ProjectMApi.cpp`**

If Task 1 Step 3 recorded a different library file name, use it in `platformLibraryNames()`. (Task 1 was run on the development Mac: the installed files are `libprojectM-4.dylib` → `libprojectM-4.4.dylib` → `libprojectM-4.4.2.0.dylib`, matching the names below, and all 17 symbols resolved below are exported.)

```cpp
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
    for (const std::string& n : names) out.push_back(n);          // the system loader's own search
#if !defined(_WIN32)
    std::vector<std::string> dirs = {"/usr/local/lib", "/opt/homebrew/lib"};
    if (!homeDir.empty()) dirs.push_back(homeDir + "/.local/lib");
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
    const char* home = std::getenv("HOME");
    return loadFrom(projectMCandidatePaths(prefPath, home ? home : ""));
}

bool ProjectMApi::loadFrom(const std::vector<std::string>& candidates) {
    if (available_) return true;
    std::string rejection;                                        // why an opened library was refused
    for (const std::string& path : candidates) {
        DynLib lib;
        if (!lib.open(path)) continue;
        ProjectMFunctions fns;                                    // bound locally; adopted only on success
        std::string version, why;
        if (bind(lib, fns, version, why)) {
            static_cast<ProjectMFunctions&>(*this) = fns;
            lib_        = std::move(lib);                         // keeps the handle open: fns stay valid
            version_    = version;
            loadedPath_ = path;
            available_  = true;
            status_     = "projectM " + version_;
            return true;
        }
        if (rejection.empty()) rejection = why;                   // `lib` closes here; `fns` is dropped
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
```

- [ ] **Step 6: Verify it passes**

Run: `cmake --build build -j --target core_tests && ./build/core_tests -tc="projectM*,ProjectMApi*"`
Expected: PASS, 4 test cases.

- [ ] **Step 7: Commit**

```bash
git add src/gfx/ProjectMApi.h src/gfx/ProjectMApi.cpp tests/test_projectm_api.cpp CMakeLists.txt && git commit -m "$(cat <<'EOF'
feat(gfx): ProjectMApi runtime symbol table with a 4.2+ version gate

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

#### Task 4 — post-review amendments (applied in follow-up commits)

The quality reviewer verified all 17 signatures against the real 4.2.0 headers (a `static_assert`
comparison compiles clean, plus a live cross-ABI call test for `int`-for-enum and C `bool`). The
committed code differs from the listing above in these ways:

- A rejection names the file: `"found projectM 4.1.0, needs 4.2+ (<path>)"`,
  `"missing symbol <name> (<path>)"`.
- `ProjectMApi` is non-movable (deleted move ctor / assignment), with a comment that only
  `instance()` should load the real library.
- Candidate directories are `~/.local/lib`, `/usr/local/lib`, `/opt/homebrew/lib` (own build first);
  `getenv("HOME")` is not called on Windows; the bare-name comment states what each OS really does.
- `tests/pm_fake.c` is built twice (`pm_fake_41`, `pm_fake_42`, CMake `MODULE`s) so the rejection
  message, the empty-table guarantee and candidate fall-through are unit-tested without an install.
- `tests/projectm_sigcheck.cpp` is a compile-only `OBJECT` library, built only where
  `projectM-4/core.h` is found, that `static_assert`s every binding against the real headers.
  **When a binding is added to `ProjectMFunctions`, add a `SIGCHECK` line and a no-op to `pm_fake.c`.**

---

### Task 5: `AssetType::Preset` — the sixth asset type

**Files:**
- Modify: `src/core/AssetLibrary.h:10-11`
- Modify: `src/ui/AssetsPanel.cpp` (tab bar, ~line 328)
- Modify: `src/ui/PropertiesPanel.cpp:15-24`
- Modify: `tests/test_asset_library_file.cpp:41-43`

- [ ] **Step 1: Update the existing count assertion and add the failing test**

In `tests/test_asset_library_file.cpp`, in `TEST_CASE("AssetType::Image is the fifth type and round-trips the codec")`, change

```cpp
    CHECK(kAssetTypeCount == 5);
```

to

```cpp
    CHECK(kAssetTypeCount == 6);
```

Then append at the end of the file:

```cpp
TEST_CASE("AssetType::Preset is the sixth type and round-trips the codec") {
    CHECK((int)AssetType::Preset == 5);       // appended, so older files' type ints are unchanged

    AssetLibrary lib;
    int i = lib.add(AssetType::Preset, "Cosmic Dust", "/m/presets/Geiss - Cosmic Dust.milk");

    AssetLibrary out;
    REQUIRE(parseLibrary(serializeLibrary(lib), out));
    const Asset* a = out.find(i);
    REQUIRE(a != nullptr);
    CHECK(a->type == AssetType::Preset);
    CHECK(a->path == "/m/presets/Geiss - Cosmic Dust.milk");
    CHECK(out.byType(AssetType::Preset).size() == 1);
    CHECK(out.byType(AssetType::Image).empty());
}
```

- [ ] **Step 2: Verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — `'Preset' is not a member of 'oss::AssetType'`.

- [ ] **Step 3: Add the enum value**

In `src/core/AssetLibrary.h` replace

```cpp
enum class AssetType { Audio, Video, Midi, Mesh, Image };
constexpr int kAssetTypeCount = 5;   // number of AssetType values (and Assets-window tabs)
```

with

```cpp
enum class AssetType { Audio, Video, Midi, Mesh, Image, Preset };   // append only: the codec stores the int
constexpr int kAssetTypeCount = 6;   // number of AssetType values (and Assets-window tabs)
```

- [ ] **Step 4: Add the Assets tab**

In `src/ui/AssetsPanel.cpp`, inside `AssetsPanel::draw`, after the `"3D"` tab block and before `ImGui::EndTabBar();`, add:

```cpp
        if (ImGui::BeginTabItem("Presets")) {
            drawTab(lib, AssetType::Preset, "Milkdrop preset", {"milk"});
            ImGui::EndTabItem();
        }
```

- [ ] **Step 5: Add the type label**

In `src/ui/PropertiesPanel.cpp`, in `assetTypeName`, after `case AssetType::Image: return "Image";` add:

```cpp
        case AssetType::Preset: return "Preset";
```

- [ ] **Step 6: Verify it passes (tests + the app still builds)**

Run: `cmake --build build -j && ./build/core_tests -tc="AssetType*"`
Expected: build succeeds; PASS, including the two `AssetType::…` cases in `test_asset_library_file.cpp`.

- [ ] **Step 7: Commit**

```bash
git add src/core/AssetLibrary.h src/ui/AssetsPanel.cpp src/ui/PropertiesPanel.cpp tests/test_asset_library_file.cpp && git commit -m "$(cat <<'EOF'
feat(assets): Preset asset type (.milk) with its own Assets tab

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Preferences — library path + textures folder

**Files:**
- Modify: `src/core/Preferences.h:22-23`
- Modify: `src/core/Preferences.cpp` (`serializePreferences`, `parsePreferences`)
- Modify: `src/ui/PreferencesPanel.cpp` (Locations tab, ~line 130-148)
- Modify: `tests/test_preferences.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_preferences.cpp`:

```cpp
TEST_CASE("Preferences round-trips the projectM library path + textures dir") {
    Preferences p;
    p.projectMLibraryPath = "/Users/me/.local/lib/libprojectM-4.dylib";
    p.projectMTexturesDir = "/Volumes/media/milkdrop textures";     // spaces survive (rest-of-line)
    Preferences q;
    REQUIRE(parsePreferences(serializePreferences(p), q));
    CHECK(q.projectMLibraryPath == "/Users/me/.local/lib/libprojectM-4.dylib");
    CHECK(q.projectMTexturesDir == "/Volumes/media/milkdrop textures");
}

TEST_CASE("Preferences without the projectM lines parse to empty") {
    Preferences q;
    REQUIRE(parsePreferences("oss-prefs 1\naudio-buffer 200\n", q));
    CHECK(q.projectMLibraryPath.empty());
    CHECK(q.projectMTexturesDir.empty());
}
```

- [ ] **Step 2: Verify it fails**

Run: `cmake --build build -j --target core_tests`
Expected: FAIL — no member named `projectMLibraryPath`.

- [ ] **Step 3: Add the fields**

In `src/core/Preferences.h`, after the `assetLibraryDir` line, add:

```cpp
    std::string projectMLibraryPath; // libprojectM-4 shared library to load ("" = search the usual places)
    std::string projectMTexturesDir; // extra Milkdrop texture search folder for the projectM node ("" = none)
```

- [ ] **Step 4: Add the codec lines**

In `src/core/Preferences.cpp`, in `serializePreferences`, after the `assetlibdir` line add:

```cpp
    if (!p.projectMLibraryPath.empty()) out += "pmlib " + p.projectMLibraryPath + "\n";
    if (!p.projectMTexturesDir.empty()) out += "pmtextures " + p.projectMTexturesDir + "\n";
```

In `parsePreferences`, after the `else if (kw == "assetlibdir") ...` line add:

```cpp
        else if (kw == "pmlib")      out.projectMLibraryPath = rest;
        else if (kw == "pmtextures") out.projectMTexturesDir = rest;
```

- [ ] **Step 5: Verify the tests pass**

Run: `cmake --build build -j --target core_tests && ./build/core_tests -tc="Preferences*projectM*"`
Expected: PASS, 2 test cases.

- [ ] **Step 6: Add the Locations rows + status**

In `src/ui/PreferencesPanel.cpp`, add to the includes at the top:

```cpp
#include "core/PathUtil.h"
#include "gfx/ProjectMApi.h"
```

In the `"Locations"` tab, after the two existing `folderRow(...)` calls and before `ImGui::EndTabItem();`, add:

```cpp
            ImGui::Separator();
            ImGui::TextUnformatted("projectM (optional)");
            {   // the shared library: a file, not a folder
                std::string& lib = prefs.projectMLibraryPath;
                ImGui::TextUnformatted("projectM library");
                ImGui::SameLine(160.0f);
                ImGui::TextUnformatted(lib.empty() ? "(search the usual places)" : lib.c_str());
                ImGui::SameLine();
                ImGui::PushID("pmlib");
                if (ImGui::SmallButton("Browse...")) {
                    std::string picked = openFileDialog("projectM library", "Shared library",
                                                        {"dylib", "so", "dll"}, parentDir(lib));
                    if (!picked.empty()) { lib = picked; if (onChange) onChange(); }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear")) { lib.clear(); if (onChange) onChange(); }
                ImGui::PopID();
            }
            folderRow("projectM textures", prefs.projectMTexturesDir);
            const ProjectMApi& pm = ProjectMApi::instance();
            ImGui::TextDisabled("%s", pm.statusText().c_str());
            if (pm.available() && !prefs.projectMLibraryPath.empty() &&
                prefs.projectMLibraryPath != pm.loadedPath())
                ImGui::TextDisabled("Restart to load a different library.");
```

- [ ] **Step 7: Verify the app builds**

Run: `cmake --build build -j`
Expected: build succeeds.

- [ ] **Step 8: Commit**

```bash
git add src/core/Preferences.h src/core/Preferences.cpp src/ui/PreferencesPanel.cpp tests/test_preferences.cpp && git commit -m "$(cat <<'EOF'
feat(prefs): projectM library path + textures folder (Locations tab)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

#### Tasks 5 and 6 — post-review amendments (applied in follow-up commits)

- **Assets:** an out-of-range (future) type int is preserved verbatim instead of being clamped onto
  the last known type (negative still clamps to 0); a test pins the load/save round trip. The stale
  comments above `enum class AssetType` and on `AssetsPanel` were corrected. `oss-assetlib 1` is not
  bumped.
- **Preferences:** the library picker has no extension filter (`libprojectM-4.so.4` has extension
  `.4`); Browse / Clear precede the path, and the path and status wrap; `parsePreferences` strips a
  trailing `\r` (CRLF files); the restart hint uses the new `ProjectMApi::loadedPrefPath()`
  (`loadFrom(candidates, prefPath = "")` records the preference of the load that succeeded).

Declined: renaming `addImageFolderInput` (the node uses a file input, so there is no second caller),
passing the status into the panel as a parameter.

---

### Task 7: `Framebuffer::id()` + `gfx/GLStateGuard.h`

**Files:**
- Modify: `src/gfx/Framebuffer.h`
- Create: `src/gfx/GLStateGuard.h`
- Modify: `tests/gl_smoke.cpp`

- [ ] **Step 1: Write the failing `gl_smoke` scenario**

In `tests/gl_smoke.cpp`, add to the includes (after `#include "modules/HsvAdjustNode.h"`):

```cpp
#include "gfx/GLStateGuard.h"
```

Immediately **before** the final three lines of `main` (`glfwDestroyWindow(win);` / `glfwTerminate();` / `return 0;`), add:

```cpp
    // GLStateGuard: state changed inside the scope is restored on exit.
    {
        Framebuffer scratch;
        scratch.create(8, 8);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, 17, 19);
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_TRUE);
        glUseProgram(0);
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        {
            GLStateGuard guard;
            scratch.bind();                                   // FBO + viewport
            glEnable(GL_BLEND); glEnable(GL_DEPTH_TEST); glEnable(GL_SCISSOR_TEST); glEnable(GL_CULL_FACE);
            glBlendFunc(GL_ONE, GL_ONE);
            glDepthMask(GL_FALSE);
            glBindTexture(GL_TEXTURE_2D, scratch.texture());  // unit 0's binding
            glActiveTexture(GL_TEXTURE3);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        }
        GLint fb = -1, vp[4] = {0, 0, 0, 0}, unit = 0, tex = -1, align = 0, srcRgb = 0;
        GLboolean depthMask = GL_FALSE;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        glGetIntegerv(GL_VIEWPORT, vp);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        bool ok = fb == 0 && vp[2] == 17 && vp[3] == 19 && unit == GL_TEXTURE0 && tex == 0 &&
                  align == 4 && srcRgb == GL_SRC_ALPHA && depthMask == GL_TRUE &&
                  !glIsEnabled(GL_BLEND) && !glIsEnabled(GL_DEPTH_TEST) &&
                  !glIsEnabled(GL_SCISSOR_TEST) && !glIsEnabled(GL_CULL_FACE);
        if (!ok) { glfwTerminate(); return fail("GLStateGuard did not restore the GL state"); }
        std::fprintf(stderr, "gl_smoke OK: GLStateGuard restores framebuffer/viewport/texture/enables\n");
    }
```

- [ ] **Step 2: Verify it fails**

Run: `cmake --build build -j --target gl_smoke`
Expected: FAIL — `gfx/GLStateGuard.h` not found.

- [ ] **Step 3: Add `Framebuffer::id()`**

In `src/gfx/Framebuffer.h`, after `GLuint texture() const { return tex_; }` add:

```cpp
    GLuint id() const { return fbo_; }     // the FBO name (for renderers that take an FBO id)
```

- [ ] **Step 4: Write `src/gfx/GLStateGuard.h`**

```cpp
#pragma once
#include <glad/gl.h>

namespace oss {

// Saves the GL state a foreign renderer (projectM) disturbs and restores it on scope exit, so the
// nodes and ImGui that run afterwards see what they left. Element-array bindings are VAO state and
// come back with the VAO. Only the 2D texture binding of the unit that was active on entry is
// restored; other units are left as the foreign renderer set them (our nodes bind what they sample).
class GLStateGuard {
public:
    GLStateGuard() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo_);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo_);
        glGetIntegerv(GL_VIEWPORT, viewport_);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao_);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer_);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture_);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2d_);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb_);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb_);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha_);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha_);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment_);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask_);
        blend_   = glIsEnabled(GL_BLEND);
        depth_   = glIsEnabled(GL_DEPTH_TEST);
        cull_    = glIsEnabled(GL_CULL_FACE);
        scissor_ = glIsEnabled(GL_SCISSOR_TEST);
    }
    ~GLStateGuard() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)drawFbo_);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)readFbo_);
        glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        glUseProgram((GLuint)program_);
        glBindVertexArray((GLuint)vao_);
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)arrayBuffer_);
        glActiveTexture((GLenum)activeTexture_);
        glBindTexture(GL_TEXTURE_2D, (GLuint)texture2d_);
        glBlendFuncSeparate((GLenum)blendSrcRgb_, (GLenum)blendDstRgb_,
                            (GLenum)blendSrcAlpha_, (GLenum)blendDstAlpha_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment_);
        glDepthMask(depthMask_);
        set(GL_BLEND, blend_);
        set(GL_DEPTH_TEST, depth_);
        set(GL_CULL_FACE, cull_);
        set(GL_SCISSOR_TEST, scissor_);
    }
    GLStateGuard(const GLStateGuard&) = delete;
    GLStateGuard& operator=(const GLStateGuard&) = delete;

private:
    static void set(GLenum cap, GLboolean on) { if (on) glEnable(cap); else glDisable(cap); }

    GLint drawFbo_ = 0, readFbo_ = 0, viewport_[4] = {0, 0, 0, 0};
    GLint program_ = 0, vao_ = 0, arrayBuffer_ = 0, activeTexture_ = GL_TEXTURE0, texture2d_ = 0;
    GLint blendSrcRgb_ = GL_ONE, blendDstRgb_ = GL_ZERO, blendSrcAlpha_ = GL_ONE, blendDstAlpha_ = GL_ZERO;
    GLint unpackAlignment_ = 4;
    GLboolean depthMask_ = GL_TRUE, blend_ = GL_FALSE, depth_ = GL_FALSE, cull_ = GL_FALSE, scissor_ = GL_FALSE;
};

} // namespace oss
```

- [ ] **Step 5: Verify it passes**

Run: `cmake --build build -j --target gl_smoke && ./build/gl_smoke 2>&1 | tail -3`
Expected: the last lines include `gl_smoke OK: GLStateGuard restores framebuffer/viewport/texture/enables`, exit code 0.

- [ ] **Step 6: Commit**

```bash
git add src/gfx/Framebuffer.h src/gfx/GLStateGuard.h tests/gl_smoke.cpp && git commit -m "$(cat <<'EOF'
feat(gfx): GLStateGuard + Framebuffer::id()

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: `ProjectMNode` — the node, with the always-run `gl_smoke` checks

**Files:**
- Create: `src/modules/ProjectMNode.h`, `src/modules/ProjectMNode.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/gl_smoke.cpp`

Input port indices are the `ProjectMNode::In` enum; tests and the node both use it.

- [ ] **Step 1: Write the failing `gl_smoke` checks**

In `tests/gl_smoke.cpp`:

(a) Add to the includes:

```cpp
#include "modules/ProjectMNode.h"
#include <cstdlib>
#include <fstream>
```

(b) Add these helpers after the existing `readAtUV` function:

```cpp
// A minimal Milkdrop preset authored for these tests (no third-party preset ships in the repo).
// No warp/zoom/rotation, so a burned image stays where it was stamped; the orange outer border
// guarantees non-black pixels even with silent audio.
static const char* kTestPreset =
    "[preset00]\n"
    "fDecay=0.98\nzoom=1.0\nrot=0.0\nwarp=0.0\n"
    "nWaveMode=0\nfWaveAlpha=1.0\nfWaveScale=1.0\n"
    "wave_r=1.0\nwave_g=0.4\nwave_b=0.1\n"
    "ob_size=0.04\nob_r=0.9\nob_g=0.4\nob_b=0.1\nob_a=1.0\n";

// Write a.milk / b.milk / c.milk into a fresh temp folder; returns the folder ("" on failure).
static std::string writePresetFolder() {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "oss_projectm_smoke";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (!std::filesystem::create_directories(dir, ec)) return "";
    for (const char* n : {"a.milk", "b.milk", "c.milk"}) {
        std::ofstream f(dir / n);
        if (!f) return "";
        f << kTestPreset;
    }
    return dir.string();
}

// The node's input defaults as a resolved input vector (what Graph::evaluate would hand it).
static std::vector<Value> defaultInputs(const Node& n) {
    std::vector<Value> v;
    for (const Port& p : n.inputs()) v.push_back(p.defaultValue);
    return v;
}
```

(c) In the pre-GL asset-backed block near line 136 (the one that declares `AudioPlayerNode ap; if (bad(ap, 0, AssetType::Audio)) ...`), add a line in the same style:

```cpp
        ProjectMNode       pmn; if (bad(pmn, ProjectMNode::kPreset, AssetType::Preset)) return fail("projectM.preset not asset-backed Preset");
```

(d) Immediately **before** the `// GLStateGuard:` block added in Task 7, add:

```cpp
    // projectM node, library-absent path (what CI sees). ProjectMApi::load() has deliberately not
    // been called yet in this process, so the node must be inert.
    {
        ProjectMNode pm;
        pm.initGL();
        std::vector<Value> ins = defaultInputs(pm), outs(1);
        EvalContext ctx{ins, outs, 1.0f / 60.0f, nullptr, nullptr};
        pm.evaluate(ctx);
        TexRef t = std::get<TexRef>(outs[0]);
        if (t.id == 0 || t.w != kCanvasW || t.h != kCanvasH) { glfwTerminate(); return fail("projectM (inert): no canvas-sized texture"); }
        int r, gg, b, a;
        readCentre(t, r, gg, b, a);
        if (!(r == 0 && gg == 0 && b == 0 && a == 255)) { glfwTerminate(); return fail("projectM (inert): texture not opaque black"); }
        if (pm.statusLine().empty()) { glfwTerminate(); return fail("projectM (inert): empty status line"); }

        // Playlist write-back needs no library: next from a.milk -> b.milk, written into the field.
        std::string dir = writePresetFolder();
        if (dir.empty()) { glfwTerminate(); return fail("projectM: write preset folder"); }
        ins[ProjectMNode::kPreset] = Value(dir + "/a.milk");
        pm.evaluate(ctx);
        if (fileBaseName(std::get<std::string>(pm.inputDefault(ProjectMNode::kPreset))) != "a.milk") { glfwTerminate(); return fail("projectM: incoming preset not written back"); }
        pm.onButtonPressed(1);   // next
        pm.evaluate(ctx);        // `ins` still says a.milk, like an unchanged edge: the step must hold
        if (fileBaseName(std::get<std::string>(pm.inputDefault(ProjectMNode::kPreset))) != "b.milk") { glfwTerminate(); return fail("projectM: next did not step to b.milk"); }
        pm.evaluate(ctx);
        if (fileBaseName(std::get<std::string>(pm.inputDefault(ProjectMNode::kPreset))) != "b.milk") { glfwTerminate(); return fail("projectM: step snapped back"); }
        std::fprintf(stderr, "gl_smoke OK: projectM inert path (black texture, status '%s') + playlist write-back\n", pm.statusLine().c_str());
    }
```

`kCanvasW`/`kCanvasH` come from `gfx/Canvas.h` and `fileBaseName` from `core/PathUtil.h`, both already included transitively through `modules/ImageSequencerNode.h`.

- [ ] **Step 2: Add the node to CMake**

1. `APP_SOURCES`: after `  src/modules/WireframeNode.cpp` add `  src/modules/ProjectMNode.cpp`.
2. `gl_smoke`: after its `  src/modules/WireframeNode.cpp` line add `  src/modules/ProjectMNode.cpp`.

- [ ] **Step 3: Verify it fails**

Run: `cmake --build build -j --target gl_smoke`
Expected: FAIL — `modules/ProjectMNode.h` not found.

- [ ] **Step 4: Write `src/modules/ProjectMNode.h`**

```cpp
#pragma once
#include <glad/gl.h>
#include <random>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/PresetPlaylist.h"
#include "gfx/Framebuffer.h"
#include "gfx/ProjectMApi.h"

namespace oss {

// Runs the projectM (Milkdrop-compatible) visualizer: audio in -> texture out. libprojectM 4.2+ is
// loaded at runtime through ProjectMApi; when it is unavailable the node is inert (black texture +
// a status line) so projects that use it still open. The `preset` file is the single source of
// truth; its folder is the playlist for the prev/next/random buttons and the bar-synced step
// (PresetSelector). `texture in` is burned into projectM's canvas while `burn` > 0.5.
class ProjectMNode : public Node {
public:
    enum In { kLeft = 0, kRight, kTexIn, kBurn, kPreset, kBlend, kBlendTime,
              kBeatSens, kMesh, kSync, kBars, kShuffle };

    ProjectMNode();
    ~ProjectMNode() override;      // destroys the projectM instance (editor GL context current)

    void initGL() override;
    void evaluate(EvalContext& ctx) override;
    std::string statusLine() const override { return status_; }

    int         buttonCount() const override { return 3; }
    std::string buttonLabel(int i) const override;
    void        onButtonPressed(int i) override { pendingButton_ = i; }

private:
    void ensureInstance(int w, int h);              // lazy: also covers a library found after startup
    void pushParams(EvalContext& ctx);
    void updateSearchPaths(const EvalContext& ctx);
    void clearToBlack();
    static void onSwitchFailed(const char* file, const char* message, void* self);

    Framebuffer      fbo_;
    PmHandle         pm_ = nullptr;
    bool             createFailed_ = false;
    PresetSelector   sel_;
    std::minstd_rand rng_{std::random_device{}()};  // the random button only; not reproducible
    int              pendingButton_ = -1;
    double           time_ = 0.0;                   // accumulated ctx.dt -> projectm_set_frame_time
    float            lastBlendTime_ = -1.0f, lastBeatSens_ = -1.0f;
    int              lastMesh_ = -1;
    bool             pathsSet_ = false;
    std::string      loadedPath_, searchDir_, searchTex_;
    std::vector<float> pcm_;                        // interleaved LRLR scratch
    std::string      status_, failMsg_;
};

} // namespace oss
```

- [ ] **Step 5: Write `src/modules/ProjectMNode.cpp`**

```cpp
#include "modules/ProjectMNode.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include "core/PathUtil.h"
#include "core/Preferences.h"
#include "gfx/Canvas.h"
#include "gfx/GLStateGuard.h"

namespace oss {

namespace {
// projectM resolves its GL entry points through this; GLFW knows the current context's loader.
void* glLoadProc(const char* name, void* /*userData*/) {
    return reinterpret_cast<void*>(glfwGetProcAddress(name));
}
} // namespace

ProjectMNode::ProjectMNode() : Node("projectM") {
    addInput("left",  PortType::Audio,   AudioRef{});
    addInput("right", PortType::Audio,   AudioRef{});
    addInput("texture in", PortType::Texture, TexRef{});
    addInput("burn", PortType::Float, 0.0f, 0.0f, 1.0f);              // gate: > 0.5 burns every frame
    addAssetInput("preset", AssetType::Preset);
    addInput("blend", PortType::Bool, true);                          // smooth transition vs hard cut
    addInput("blend time", PortType::Float, 3.0f, 0.0f, 10.0f);       // seconds
    addInput("beat sensitivity", PortType::Float, 1.0f, 0.0f, 2.0f);
    addIntInput("mesh", 48, 8, 128);                                  // columns; rows = 3/4
    addInput("sync", PortType::Bool, false);
    addIntInput("bars", 4, 1, 64);
    addInput("shuffle", PortType::Bool, false);
    addOutput("texture", PortType::Texture);
}

ProjectMNode::~ProjectMNode() {
    if (pm_) ProjectMApi::instance().destroy(pm_);
}

std::string ProjectMNode::buttonLabel(int i) const {
    static const char* kLabels[3] = {"prev", "next", "random"};
    return (i >= 0 && i < 3) ? kLabels[i] : "";
}

void ProjectMNode::initGL() {
    fbo_.create(kCanvasW, kCanvasH);
    clearToBlack();
    ensureInstance(kCanvasW, kCanvasH);
}

void ProjectMNode::clearToBlack() {
    GLStateGuard guard;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_.id());
    glDisable(GL_SCISSOR_TEST);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    glClearBufferfv(GL_COLOR, 0, black);            // doesn't touch the global clear colour
}

void ProjectMNode::ensureInstance(int w, int h) {
    ProjectMApi& api = ProjectMApi::instance();
    if (pm_ || createFailed_ || !api.available()) return;
    GLStateGuard guard;
    pm_ = api.create(&glLoadProc, nullptr);
    if (!pm_) { createFailed_ = true; return; }
    api.setWindowSize(pm_, (std::size_t)w, (std::size_t)h);
    api.setPresetLocked(pm_, true);                 // the node decides every preset change
    api.setAspectCorrection(pm_, true);
    api.setFps(pm_, 60);
    api.setSwitchFailedCallback(pm_, &ProjectMNode::onSwitchFailed, this);
}

void ProjectMNode::onSwitchFailed(const char* file, const char* message, void* self) {
    auto* n = static_cast<ProjectMNode*>(self);
    n->failMsg_ = "failed: " + fileBaseName(file ? file : "") + ": " + (message ? message : "");
}

void ProjectMNode::pushParams(EvalContext& ctx) {
    ProjectMApi& api = ProjectMApi::instance();
    float blendTime = ctx.in<float>(kBlendTime);
    if (blendTime != lastBlendTime_) { lastBlendTime_ = blendTime; api.setSoftCutDuration(pm_, (double)blendTime); }
    float beat = ctx.in<float>(kBeatSens);
    if (beat != lastBeatSens_) { lastBeatSens_ = beat; api.setBeatSensitivity(pm_, beat); }
    int cols = std::min(128, std::max(8, (int)std::lround(ctx.in<float>(kMesh))));
    if (cols != lastMesh_) {
        lastMesh_ = cols;
        int rows = std::max(2, (int)std::lround(cols * 0.75));
        GLStateGuard guard;                         // a mesh resize reallocates GL buffers
        api.setMeshSize(pm_, (std::size_t)cols, (std::size_t)rows);
    }
}

void ProjectMNode::updateSearchPaths(const EvalContext& ctx) {
    std::string dir = parentDir(sel_.current());
    std::string tex = ctx.prefs ? ctx.prefs->projectMTexturesDir : std::string();
    if (pathsSet_ && dir == searchDir_ && tex == searchTex_) return;
    pathsSet_ = true; searchDir_ = dir; searchTex_ = tex;
    std::vector<const char*> paths;
    if (!searchDir_.empty()) paths.push_back(searchDir_.c_str());
    if (!searchTex_.empty()) paths.push_back(searchTex_.c_str());
    const char* none = nullptr;
    ProjectMApi::instance().setTextureSearchPaths(pm_, paths.empty() ? &none : paths.data(), paths.size());
}

void ProjectMNode::evaluate(EvalContext& ctx) {
    ProjectMApi& api = ProjectMApi::instance();
    const int w = ctx.prefs ? ctx.prefs->textureWidth  : kCanvasW;
    const int h = ctx.prefs ? ctx.prefs->textureHeight : kCanvasH;
    const bool resized = (fbo_.width() != w || fbo_.height() != h);
    if (resized) { fbo_.create(w, h); clearToBlack(); }

    // Preset selection runs even when inert, so the field and buttons behave the same everywhere.
    PresetSelectorInput in;
    in.incoming    = ctx.in<std::string>(kPreset);
    in.button      = pendingButton_;  pendingButton_ = -1;
    in.sync        = ctx.in<bool>(kSync);
    in.playing     = ctx.transport && ctx.transport->playing;
    in.bars        = ctx.transport ? ctx.transport->bars() : 0.0;
    in.everyNBars  = std::max(1, (int)std::lround(ctx.in<float>(kBars)));
    in.shuffle     = ctx.in<bool>(kShuffle);
    in.randomValue = (unsigned)rng_();
    if (sel_.update(in)) inputDefault(kPreset) = Value(sel_.current());   // the field shows what plays

    ensureInstance(w, h);
    if (!pm_) {
        status_ = createFailed_ ? std::string("projectM failed to initialise") : api.statusText();
        ctx.out<TexRef>(0, TexRef{ fbo_.texture(), fbo_.width(), fbo_.height() });
        return;
    }
    if (resized) api.setWindowSize(pm_, (std::size_t)w, (std::size_t)h);

    pushParams(ctx);
    updateSearchPaths(ctx);                          // before the load, so its textures resolve

    if (sel_.current() != loadedPath_) {             // also true right after a lazy create
        loadedPath_ = sel_.current();
        failMsg_.clear();
        GLStateGuard guard;
        api.loadPresetFile(pm_, loadedPath_.empty() ? "idle://" : loadedPath_.c_str(), ctx.in<bool>(kBlend));
    }

    // Audio: two mono edges -> interleaved LRLR. A lone connected side is mirrored (as the Recorder).
    AudioRef l = ctx.in<AudioRef>(kLeft), r = ctx.in<AudioRef>(kRight);
    const AudioRef& effL = (l.samples && l.count > 0) ? l : r;
    const AudioRef& effR = (r.samples && r.count > 0) ? r : l;
    if (effL.samples && effL.count > 0 && effR.samples) {
        std::size_t n = std::min(effL.count, effR.count);
        pcm_.resize(n * 2);
        for (std::size_t i = 0; i < n; ++i) { pcm_[2 * i] = effL.samples[i]; pcm_[2 * i + 1] = effR.samples[i]; }
        api.pcmAddFloat(pm_, pcm_.data(), (unsigned int)n, kPmStereo);
    }

    {
        GLStateGuard guard;
        TexRef tin = ctx.in<TexRef>(kTexIn);
        if (ctx.in<float>(kBurn) > 0.5f && tin.id != 0)
            api.burnTexture(pm_, tin.id, 0, 0, w, h);
        api.setFrameTime(pm_, time_);                // the app's clock, 0.0 on the first frame
        api.renderFrameFbo(pm_, fbo_.id());
    }
    time_ += ctx.dt;

    if (!failMsg_.empty()) {
        status_ = failMsg_;
    } else if (sel_.current().empty()) {
        status_ = "projectM " + api.versionText() + " \xC2\xB7 no preset";
    } else {
        std::string name = fileBaseName(sel_.current());
        if (name.size() > 5) name.resize(name.size() - 5);                 // drop ".milk"
        status_ = "projectM " + api.versionText() + " \xC2\xB7 " + name;
        if (sel_.index() >= 0)
            status_ += " (" + std::to_string(sel_.index() + 1) + "/" + std::to_string(sel_.count()) + ")";
    }
    ctx.out<TexRef>(0, TexRef{ fbo_.texture(), fbo_.width(), fbo_.height() });
}

} // namespace oss
```

Notes for the implementer:
- `"\xC2\xB7"` is a UTF-8 middle dot, written as escapes so MSVC's source charset can't mangle it. It is always followed by a space, so the hex escape cannot swallow a following hex digit.
- The spec's "a failed preset is not retried every frame" holds by construction: a load is attempted only when `sel_.current()` differs from `loadedPath_`, and `loadedPath_` is set to the attempted path before the call.
- `ctx.transport->playing` is a public data member of `Transport` (not a function).

- [ ] **Step 6: Verify it passes**

Run: `cmake --build build -j --target gl_smoke && ./build/gl_smoke 2>&1 | tail -4`
Expected: includes `gl_smoke OK: projectM inert path (black texture, status 'projectM not found') + playlist write-back`, exit code 0.

- [ ] **Step 7: Commit**

```bash
git add src/modules/ProjectMNode.h src/modules/ProjectMNode.cpp tests/gl_smoke.cpp CMakeLists.txt && git commit -m "$(cat <<'EOF'
feat(modules): projectM node (runtime-loaded visualizer, audio in -> texture out)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

### Task 9: Library-dependent `gl_smoke` checks (render, GL state, burn, status)

Runs fully only where Task 1 was done; elsewhere it prints SKIP and passes. CI will skip it.

**Files:**
- Modify: `tests/gl_smoke.cpp`
- Possibly modify: `src/modules/ProjectMNode.cpp` (burn orientation, Step 4)

- [ ] **Step 1: Add the scenario**

In `tests/gl_smoke.cpp`, immediately **after** the projectM inert block from Task 8 (so the library is loaded only once that block has run), add:

```cpp
    // projectM node, live path. Needs libprojectM 4.2+ (OSS_PROJECTM_LIB overrides the search).
    {
        const char* envLib = std::getenv("OSS_PROJECTM_LIB");
        ProjectMApi& api = ProjectMApi::instance();
        if (!api.load(envLib ? envLib : "")) {
            std::fprintf(stderr, "gl_smoke SKIP: projectM render/burn checks (%s)\n", api.statusText().c_str());
        } else {
            std::string dir = writePresetFolder();
            if (dir.empty()) { glfwTerminate(); return fail("projectM live: write preset folder"); }

            std::vector<float> sine(800);
            for (std::size_t i = 0; i < sine.size(); ++i) sine[i] = 0.8f * std::sin(2.0f * 3.14159265f * 220.0f * (float)i / 48000.0f);

            ProjectMNode pm;
            pm.initGL();
            std::vector<Value> ins = defaultInputs(pm), outs(1);
            ins[ProjectMNode::kLeft]   = Value(AudioRef{sine.data(), sine.size(), 48000});   // right mirrors it
            ins[ProjectMNode::kPreset] = Value(dir + "/a.milk");
            ins[ProjectMNode::kBlend]  = Value(false);                                       // hard cut: no blend wait
            EvalContext ctx{ins, outs, 1.0f / 60.0f, nullptr, nullptr};

            // (a) GL state is unchanged across evaluate.
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, 33, 44);
            glUseProgram(0);
            glBindVertexArray(0);
            for (int i = 0; i < 10; ++i) pm.evaluate(ctx);
            GLint fb = -1, vp[4] = {0, 0, 0, 0}, prog = -1, vao = -1;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
            glGetIntegerv(GL_VIEWPORT, vp);
            glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            std::fprintf(stderr, "gl_smoke projectM status: %s\n", pm.statusLine().c_str());
            if (!(fb == 0 && vp[2] == 33 && vp[3] == 44 && prog == 0 && vao == 0)) { glfwTerminate(); return fail("projectM live: GL state leaked out of evaluate"); }

            // (b) it rendered something.
            TexRef t = std::get<TexRef>(outs[0]);
            std::vector<unsigned char> px((size_t)t.w * t.h * 4);
            glBindTexture(GL_TEXTURE_2D, t.id);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            bool lit = false;
            for (size_t i = 0; i + 3 < px.size() && !lit; i += 4) lit = (px[i] + px[i + 1] + px[i + 2]) > 60;
            if (!lit) { glfwTerminate(); return fail("projectM live: output is black (see the status line above)"); }

            // (c) the status shows the playlist position after a step.
            pm.onButtonPressed(1);
            pm.evaluate(ctx);
            if (pm.statusLine().find("(2/3)") == std::string::npos) { glfwTerminate(); return fail("projectM live: status does not show (2/3) after next"); }

            // (d) burn: a texture whose TOP half is red and BOTTOM half green (GL rows are bottom-up).
            const int S = 64;
            std::vector<unsigned char> img((size_t)S * S * 4);
            for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) {
                unsigned char* p = &img[((size_t)y * S + x) * 4];
                bool top = (y >= S / 2);
                p[0] = top ? 255 : 0; p[1] = top ? 0 : 255; p[2] = 0; p[3] = 255;
            }
            GLuint src = 0;
            glGenTextures(1, &src);
            glBindTexture(GL_TEXTURE_2D, src);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            ins[ProjectMNode::kTexIn] = Value(TexRef{src, S, S});
            ins[ProjectMNode::kBurn]  = Value(1.0f);
            for (int i = 0; i < 5; ++i) pm.evaluate(ctx);
            t = std::get<TexRef>(outs[0]);
            int r1, g1, b1, a1, r2, g2, b2, a2;
            readAtUV(t, 0.2f, 0.85f, r1, g1, b1, a1);   // upper area, clear of the border and the centre wave
            readAtUV(t, 0.2f, 0.15f, r2, g2, b2, a2);   // lower area
            std::fprintf(stderr, "gl_smoke projectM burn: upper=(%d,%d,%d) lower=(%d,%d,%d)\n", r1, g1, b1, r2, g2, b2);
            glDeleteTextures(1, &src);
            bool upperRed = r1 > g1 + 40, lowerGreen = g2 > r2 + 40;
            bool upperGreen = g1 > r1 + 40, lowerRed = r2 > g2 + 40;
            if (upperGreen && lowerRed) { glfwTerminate(); return fail("projectM live: burn is vertically flipped (plan Task 9 Step 4)"); }
            if (!(upperRed && lowerGreen)) { glfwTerminate(); return fail("projectM live: burned texture not visible in the output"); }
            std::fprintf(stderr, "gl_smoke OK: projectM live (renders, GL state preserved, status, burn upright)\n");
        }
    }
```

- [ ] **Step 2: Build and run**

Run: `cmake --build build -j --target gl_smoke && ./build/gl_smoke 2>&1 | tail -6`

Expected, with the library installed (Task 1): `gl_smoke OK: projectM live (...)`, exit code 0.
Expected, without it: `gl_smoke SKIP: projectM render/burn checks (projectM not found)`, exit code 0.

To point at a build elsewhere: `OSS_PROJECTM_LIB=/path/to/libprojectM-4.dylib ./build/gl_smoke`.

- [ ] **Step 3: If "output is black"**

Read the `gl_smoke projectM status:` line printed just above the failure. If it starts with `failed:`, projectM rejected the test preset and the message says why — fix `kTestPreset` accordingly (it is plain `key=value` Milkdrop text) and re-run. If the status looks normal, raise the frame count in `for (int i = 0; i < 10; ++i)` to 30 and re-run; report to the user if it is still black.

- [ ] **Step 4: If "burn is vertically flipped"** (the spec's declared unknown)

projectM's API doc says a negative height flips the image vertically. In `src/modules/ProjectMNode.cpp` change

```cpp
            api.burnTexture(pm_, tin.id, 0, 0, w, h);
```

to

```cpp
            api.burnTexture(pm_, tin.id, 0, 0, w, -h);       // negative height = vertical flip (GL rows are bottom-up)
```

Re-run. If the burn is now missing entirely ("burned texture not visible"), the flip is anchored at `top`, so use `api.burnTexture(pm_, tin.id, 0, h, w, -h);` instead and re-run. Keep whichever makes the check pass, and keep the comment.

- [ ] **Step 5: Note the alpha behaviour (no code)**

The second declared unknown — whether `burn_texture` respects alpha — is answered by observation in Task 11's manual run, not here. Nothing to do in this step beyond leaving `burn` as a gate.

- [ ] **Step 6: Commit**

```bash
git add tests/gl_smoke.cpp src/modules/ProjectMNode.cpp && git commit -m "$(cat <<'EOF'
test(gl_smoke): projectM live checks (render, GL state, status, burn orientation)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

### Task 10: Register the node in the app

**Files:**
- Modify: `src/app/Application.cpp` (includes ~line 8, `makeNode` ~line 61, `nodeCategories` lines 103-113, ctor line 121-122, preferences callback ~line 248)

- [ ] **Step 1: Includes**

After `#include "modules/HsvAdjustNode.h"` add:

```cpp
#include "modules/ProjectMNode.h"
#include "gfx/ProjectMApi.h"
```

If `<algorithm>` is not already included in this file, add `#include <algorithm>` (for `std::find`).

- [ ] **Step 2: `makeNode` — always constructible, so projects load everywhere**

After the line `if (type == "HSV Adjust") return std::make_unique<HsvAdjustNode>();` add:

```cpp
    if (type == "projectM") return std::make_unique<ProjectMNode>();   // inert when the library is absent
```

- [ ] **Step 3: `nodeCategories` — list it only while the library is available**

Replace the whole function

```cpp
const std::vector<NodeCategory>& nodeCategories() {
    static const std::vector<NodeCategory> cats = {
        ...
    };
    return cats;
}
```

with (keep the six category rows exactly as they are today):

```cpp
const std::vector<NodeCategory>& nodeCategories() {
    // Rebuilt when projectM's availability changes (the library can be found after startup, when
    // the Preferences path is set). The only caller iterates immediately and keeps no reference.
    static std::vector<NodeCategory> cats;
    static int builtFor = -1;                                    // -1 = never built, else 0/1
    const int avail = ProjectMApi::instance().available() ? 1 : 0;
    if (builtFor != avail) {
        builtFor = avail;
        cats = {
            { "Texture", { "Colour", "Image Streamer", "Image Sequencer", "Video", "Mix", "Compositor", "Kaleidoscope", "HSV Adjust", "Recorder", "Output" } },
            { "Audio",   { "Sine", "Acid Bass", "Spirograph Synth", "Audio File", "Audio In", "Audio Mix", "Mono to Stereo", "Stereo to Mono", "Crossover Filter", "Spectrograph", "Oscilloscope", "Drum Machine", "Audio Out" } },
            { "MIDI",    { "MIDI In", "MIDI File", "Step Seq", "Chord Player", "Arpeggiator", "MIDI Merge", "MIDI Out", "Pitch Graph" } },
            { "3D",      { "Mesh Loader", "Text 2D", "Text 3D", "World Transform", "Wireframe", "Shaded Render", "Skybox", "Vertex Trail" } },
            { "Control", { "Automation", "LFO" } },
            { "Shader",  { "Vertex Shader", "Deform" } },
        };
        if (avail) {
            std::vector<std::string>& tex = cats[0].types;
            tex.insert(std::find(tex.begin(), tex.end(), "Recorder"), "projectM");
        }
    }
    return cats;
}
```

- [ ] **Step 4: Load the library at startup, after Preferences**

In `Application::Application`, replace

```cpp
    loadPreferences();
    graph_.setPreferences(&prefs_);
```

with

```cpp
    loadPreferences();
    graph_.setPreferences(&prefs_);
    ProjectMApi::instance().load(prefs_.projectMLibraryPath);   // optional; the projectM node is inert without it
```

- [ ] **Step 5: Retry when the preference changes**

Replace

```cpp
    preferences_.draw(prefs_, [this]{ savePreferences(); }, &showPreferences_);
```

with

```cpp
    preferences_.draw(prefs_, [this]{
        savePreferences();
        ProjectMApi::instance().load(prefs_.projectMLibraryPath);   // no-op once loaded
    }, &showPreferences_);
```

Existing inert projectM nodes pick the library up on their next `evaluate` (`ensureInstance` is lazy).

- [ ] **Step 6: Build everything and run all tests**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: build succeeds; `core_tests` and `gl_smoke` both pass.

- [ ] **Step 7: Commit**

```bash
git add src/app/Application.cpp && git commit -m "$(cat <<'EOF'
feat(app): register the projectM node (Texture; listed only when libprojectM 4.2+ loads)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

### Task 11: Docs + manual verification

**Files:**
- Modify: `CLAUDE.md`
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-09-21-projectm-node-design.md` (only if Step 4 finds something)

- [ ] **Step 1: CLAUDE.md — architecture bullet**

In `CLAUDE.md`, directly after the **HSV Adjust** bullet (the one beginning `- **HSV Adjust** — \`HsvAdjustNode\``), insert:

```markdown
- **projectM** — `ProjectMNode` (`src/modules/ProjectMNode.{h,cpp}`) runs the Milkdrop-compatible
  projectM visualizer: `left`/`right` audio in → `texture` out. **libprojectM 4.2+ is loaded at
  runtime, never linked or bundled** (LGPL-2.1): the GL-free `core/DynLib` (`dlopen`/`LoadLibrary`,
  platform headers confined to its `.cpp`) backs `gfx/ProjectMApi` — a process-wide table of the
  ~17 C-API functions we re-declare ourselves (no projectM headers), gated to major 4 / minor ≥ 2
  because `render_frame_fbo`, `burn_texture`, `set_frame_time` and `create_with_opengl_load_proc`
  are all `@since 4.2.0` (4.1.x always draws into framebuffer 0). `Application` loads it after
  Preferences (`projectMLibraryPath`, then the platform library names, then `~/.local/lib`,
  `/usr/local/lib`, `/opt/homebrew/lib`) and retries when the preference changes. Binding is
  all-or-nothing (a local table adopted only on success, so a rejected library leaves no dangling
  pointers), a rejection names the file it rejected, and the singleton is deliberately leaked so
  the library is never `dlclose`d during static destruction. `makeNode` always
  builds the node so projects open anywhere; `nodeCategories()` lists it (Texture) only while the
  library is available, and without it the node is inert (black texture + status). The asset-backed
  `preset` input (the sixth `AssetType`, **Preset**, `.milk`) is the single source of truth: its
  folder is the playlist for the **prev / next / random** buttons and the bar-synced step, all
  decided by the GL-free, unit-tested `PresetSelector` (`core/PresetPlaylist.h`) — an incoming
  change loads, steps write the path back through `inputDefault()`, and `sync` is edge-triggered
  and primed (stateless `floor(bars/N)`; sequential = absolute position, shuffle = a hash with a
  fixed seed). `texture in` is stamped into projectM's canvas while the `burn` gate is > 0.5
  (`burn_texture`). projectM renders straight into the node's FBO inside a `gfx/GLStateGuard`
  (RAII save/restore of the state it disturbs) and runs on the app's clock (`set_frame_time` from
  accumulated `dt`). `PresetPlaylist`, `DynLib` and `ProjectMApi` are unit-tested — the last through
  two fake projectM modules (`tests/pm_fake.c`, built as 4.1 and 4.2) — and
  `tests/projectm_sigcheck.cpp` is a compile-only object that `static_assert`s the 17 hand-written
  signatures against the real headers wherever they are installed (**add a `SIGCHECK` line and a
  `pm_fake.c` no-op with every new binding**); the
  inert path + playlist write-back are always `gl_smoke`-checked, and the render / GL-state / burn
  checks run only where the library is installed (they print SKIP in CI).
```

- [ ] **Step 2: CLAUDE.md — Preferences and Assets mentions**

Append to the end of the **Preferences** bullet (after the sentence about the `projectsDir` / `assetLibraryDir` location prefs):

```markdown
  The same tab holds the projectM node's `projectMLibraryPath` (a file) and `projectMTexturesDir`,
  plus the loader's status line.
```

In the **Assets / media library** bullet, change `(Audio/Video/Midi/Mesh/Image, the five tabs; \`Image\` is appended = 4, so the codec's type int stays backward-compatible)` to:

```markdown
(Audio/Video/Midi/Mesh/Image/Preset, the six tabs; new types are appended — `Image` = 4, `Preset` = 5 — so the codec's type int stays backward-compatible)
```

- [ ] **Step 3: README — optional projectM section**

In `README.md`, add a new section immediately before `## Test`:

````markdown
## projectM (optional)

The **projectM** node (Texture menu) runs the Milkdrop-compatible
[projectM](https://github.com/projectM-visualizer/projectm) visualizer on the graph's audio. It is
optional: the app loads `libprojectM-4` at runtime if it finds it, and the node only appears in the
add-node menu when it does. Nothing is linked or bundled.

It needs **projectM 4.2 or later**. 4.2 is not released yet (the latest release, 4.1.7, cannot
render into a framebuffer), so build it from source. This commit is the one the node was developed
against:

```bash
git clone --recurse-submodules https://github.com/projectM-visualizer/projectm.git && cd projectm && git checkout 1e7ef7803b69024d1e0656705670adda2ffac817 && git submodule update --init --recursive
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DBUILD_SHARED_LIBS=ON && cmake --build build -j && cmake --install build
```

The app looks in `~/.local/lib`, then `/usr/local/lib` and `/opt/homebrew/lib`, and asks the system
loader by bare name (on Linux that covers the usual library paths). On Windows put
`projectM-4.dll` beside the app or on `PATH`. For anywhere else, set **Preferences → Locations →
projectM library**; the line underneath shows what the loader found, including the path of any
library it rejected. Homebrew's `projectm` formula is 3.1.12 and will not work.

Presets are not included. Add `.milk` files in **View → Assets → Presets** and pick one on the
node; **prev / next / random** and the bar-synced `sync` step through the other presets in the same
folder. Milkdrop texture packs can be pointed to with **projectM textures** in the same
Preferences tab.

projectM is LGPL-2.1. Because it is loaded at runtime from your own installation and never
distributed with this app, that license places no conditions on this project's.
````

(The inner ```` ```bash ```` fences are part of the README text.)

- [ ] **Step 4: Manual run (needs Task 1)**

Run: `./build/shader_streamer`

Check, and report each result to the user:
1. **Add menu:** Texture → **projectM** is present (it is absent if the library was not found; Preferences → Locations shows why).
2. **Picture:** add `Audio File` (or `Sine`) → projectM `left`; projectM `texture` → `Output`. Add a preset under View → Assets → Presets (e.g. from `/usr/local/opt/projectm/share/projectM/presets`), pick it on the node. The Output window animates and reacts to the audio, and is **upright** (not mirrored or upside down).
3. **Stepping:** **next** changes the preset and the `preset` field + status line (`n/N`) follow. With `sync` on and the transport playing, it changes every `bars` bars and not before the first boundary.
4. **Burn:** wire a `Colour` or `Image Streamer` into `texture in`, raise `burn` above 0.5: the image appears and melts when `burn` drops. Note whether a partly transparent image (a PNG with alpha) blends or overwrites — that answers the spec's alpha question.
5. **Rest of the UI is intact** after projectM renders (ImGui panels draw normally, other texture nodes still render) — i.e. `GLStateGuard` covers what projectM disturbs.

If (4) shows that alpha is respected, record it in the spec's "Unknowns" section as resolved ("`burn_texture` blends with alpha; `burn` stays a gate in v1"); if it overwrites, record that instead. If anything in (2) or (5) is wrong, stop and report — do not paper over it.

- [ ] **Step 5: Full verification**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: both tests pass.

- [ ] **Step 6: Commit**

```bash
git add CLAUDE.md README.md docs/superpowers/specs/2026-09-21-projectm-node-design.md && git commit -m "$(cat <<'EOF'
docs: projectM node (optional runtime-loaded visualizer)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

## Integration note (not a task)

`main` has the audio-worker work (`Graph::evaluate` no longer runs audio nodes; visual nodes read an audio→visual boundary snapshot). It is not merged into `develop`. When it is, `ProjectMNode` needs no code change, but it must be classed as a **visual consumer of audio** by the audio-subgraph compiler, as the Spectrograph is — check that during that merge.
