# Cross-platform build/package CI (GitHub Actions) — design

**Date:** 2026-07-02
**Status:** Approved (brainstorm)
**Branch:** `feat/cross-platform-ci` (off `develop`)

## Goal

Three GitHub Actions workflows — one per OS — that build the app and produce a **platform-native
package**: an **AppImage** (Linux), a **`.app` bundle** (macOS — both Apple-Silicon **arm64** and Intel
**x64**), and an **Inno Setup installer** (Windows). They run on push/PR (uploading the package as a workflow artifact) and, on a version
tag, attach the package to a GitHub Release.

## Decisions (from brainstorm)

- **Three separate workflow files** (`build-linux.yml`, `build-macos.yml`, `build-windows.yml`),
  not a single matrix — each OS diverges enough (deps, packaging, toolchain) that separate files
  are clearer.
- **Trigger = Both:** `push` to `main`/`develop` + `pull_request` → build + upload artifact; `push`
  of a `v*` tag → build + attach the package to the GitHub Release.
- **Packages = platform-native:** AppImage / `.app` / installer.
- **macOS ships both arches** as separate native `.app` packages (arm64 via `macos-14`, Intel x64
  via `macos-13`) — a 2-leg matrix in the one macOS workflow, not a universal binary (Homebrew
  FFmpeg is native-arch-only).
- **Windows toolchain = MSVC + vcpkg** (FFmpeg + pkgconf from vcpkg, with GH Actions binary caching).
- **Signing is optional and secret-gated** — unsigned by default; notarization/Authenticode steps
  run only when the relevant secrets exist.

## Critical constraint: shader path resolution

`ShaderNode` loads shaders with plain **relative** paths (e.g. `readFile("shaders/colour.frag")`
in `src/gfx/ShaderNode.cpp`), resolved only against the **current working directory** — there is no
executable-relative fallback. Therefore **every package must launch the app with `shaders/` in its
CWD.** This is handled per-package (no app code change):

- **AppImage:** a custom `AppRun` that `cd`s into the bundled `usr/bin` (which holds `shaders/`).
- **macOS `.app`:** a launcher script set as `CFBundleExecutable` that `cd`s into `Contents/MacOS`
  (which holds `shaders/`) before exec'ing the real binary.
- **Windows installer:** the Start-menu shortcut's `WorkingDir` is the install dir (which holds
  `shaders/`).

(A ~10-line "resolve `shaders/` next to the executable" change in the app would remove the need for
all three CWD tricks — noted as an optional future cleanup, out of scope here.)

## Shared architecture

`.github/workflows/{build-linux,build-macos,build-windows}.yml`. Each workflow:

1. **Triggers:**
   ```yaml
   on:
     push:
       branches: [main, develop]
       tags: ['v*']
     pull_request:
   ```
2. **Permissions:** `contents: write` (needed by the Release step; harmless otherwise).
3. **Steps:** checkout → install OS deps → cache `build/_deps` (FetchContent sources, keyed on the
   `CMakeLists.txt` hash — draco etc. are slow to fetch/build) → configure (Release) → build →
   **`ctest -R core_tests` (fatal)** → **`gl_smoke` best-effort** (`continue-on-error`; self-skips
   with no GL context) → build the native package → `actions/upload-artifact` → **on a tag,
   `softprops/action-gh-release@v2` with the package**.
4. **Version string:** a step computes `VERSION` = the tag (`v1.2.0`) on tag builds, else
   `dev-<short-sha>`, used in the package filename.

**Dependencies reference (from `CMakeLists.txt`):** FFmpeg is a hard-required system dep via
`pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale
libswresample)`. `OpenGL` is `find_package`-required. NFD (`nativefiledialog-extended`) needs GTK3
on Linux and Cocoa on macOS. GLFW needs X11 dev on Linux. rtmidi/libsoundio need ALSA/PulseAudio on
Linux. Everything else (glm, glfw, imgui, imgui-node-editor, draco, meshoptimizer, tinyobjloader,
doctest, libsoundio, rtmidi, nfd, earcut, stb) is FetchContent and builds from source. The app
target is `shader_streamer`; `shaders/` is copied next to the binary at build time.

**Test story:** `core_tests` is GL-free/deterministic → runs and must pass on all three OSes.
`gl_smoke` needs a GL context → on Linux it gets one via `xvfb` + Mesa software GL; on macOS/Windows
CI (no window server) it self-skips. It is always non-fatal so it never blocks a release.

## Linux workflow — AppImage (`ubuntu-22.04`)

Pinned to 22.04 (older glibc → more portable AppImage than 24.04).

**Install (apt):**
```
build-essential cmake ninja-build pkg-config git
libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
xorg-dev libgl1-mesa-dev            # GLFW + OpenGL
libgtk-3-dev                        # NFD
libasound2-dev libpulse-dev         # rtmidi / libsoundio
xvfb libgl1-mesa-dri                # gl_smoke (software GL)
libfuse2 imagemagick desktop-file-utils   # AppImage runtime + placeholder icon
```

**Build:** `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`.

**Test:** `ctest --test-dir build -R core_tests --output-on-failure`; then
`xvfb-run -a ctest --test-dir build -R gl_smoke --output-on-failure || true`.

**Package (AppImage):**
1. Assemble `AppDir/usr/bin/` with `shader_streamer` + a copy of `shaders/`.
2. Generate a placeholder icon (`convert -size 256x256 xc:'#1a1c22' AppDir/usr/share/icons/hicolor/256x256/apps/shader-streamer.png`) and a `.desktop` file
   (`AppDir/usr/share/applications/shader-streamer.desktop`, `Exec=shader_streamer`,
   `Categories=AudioVideo;`).
3. Write a custom `AppDir/AppRun` (executable) that resolves its own dir and `cd`s to `usr/bin`
   before exec:
   ```sh
   #!/bin/sh
   HERE="$(dirname "$(readlink -f "$0")")"
   cd "$HERE/usr/bin" || exit 1
   exec ./shader_streamer "$@"
   ```
4. Download `linuxdeploy-x86_64.AppImage`, run
   `./linuxdeploy-x86_64.AppImage --appdir AppDir --output appimage` (it gathers the FFmpeg/GTK/X11
   `.so` deps and, seeing the existing `AppRun`, keeps it). Rename output to
   `Shader_Streamer-${VERSION}-x86_64.AppImage`.

## macOS workflow — `.app` bundle (two native arches: arm64 + Intel x64)

Ships **both** an Apple-Silicon and an Intel package as **separate native `.app` zips** (not a
universal binary — Homebrew FFmpeg is native-arch-only, so `lipo`-ing a universal binary is
impractical). The single `build-macos.yml` uses a 2-leg matrix, each leg building natively on the
matching runner:

```yaml
strategy:
  fail-fast: false
  matrix:
    include:
      - { runner: macos-14, arch: arm64 }   # Apple Silicon
      - { runner: macos-13, arch: x64 }     # Intel
runs-on: ${{ matrix.runner }}
```

Each leg runs the same steps below (native brew FFmpeg for that arch) and produces
`Shader_Streamer-${VERSION}-macos-${{ matrix.arch }}.zip`.

**Install (brew):** `ffmpeg pkg-config dylibbundler` (cmake/ninja are preinstalled on the runner;
`brew install cmake ninja` if not).

**Build:** `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`.

**Test:** `ctest --test-dir build -R core_tests --output-on-failure`;
`ctest --test-dir build -R gl_smoke --output-on-failure || true` (self-skips headless).

**Package (`.app`):**
1. Create `Shader Streamer.app/Contents/{MacOS,Resources,libs}`.
2. `Info.plist` with `CFBundleName=Shader Streamer`, `CFBundleIdentifier=com.newdigate.shaderstreamer`,
   `CFBundleExecutable=launch`, `CFBundlePackageType=APPL`.
3. Copy `build/shader_streamer` → `Contents/MacOS/shader_streamer` and `build/shaders` →
   `Contents/MacOS/shaders`.
4. Write `Contents/MacOS/launch` (chmod +x), the bundle's entry point:
   ```sh
   #!/bin/sh
   cd "$(dirname "$0")" || exit 1
   exec ./shader_streamer "$@"
   ```
5. `dylibbundler -od -b -x Contents/MacOS/shader_streamer -d Contents/libs -p @executable_path/../libs`
   — copies the linked FFmpeg dylibs into `Contents/libs` and rewrites install names so the `.app`
   is self-contained (no brew at runtime).
6. **Optional signing/notarization**, gated on secrets (`if: ${{ secrets.MACOS_CERT_P12 != '' }}`):
   import the cert, `codesign --deep --force --options runtime`, `xcrun notarytool submit`, `staple`.
   When secrets are absent, skip → unsigned bundle.
7. `ditto -c -k --keepParent "Shader Streamer.app" "Shader_Streamer-${VERSION}-macos-${{ matrix.arch }}.zip"`
   (`arm64` or `x64` per matrix leg).

## Windows workflow — Inno Setup installer (`windows-latest`, MSVC + vcpkg)

**vcpkg deps + cache:** enable GH Actions binary caching, then install FFmpeg + pkgconf:
```yaml
env:
  VCPKG_BINARY_SOURCES: "clear;x-gha,readwrite"
# expose the GH Actions cache backend to vcpkg:
- uses: actions/github-script@v7
  with:
    script: |
      core.exportVariable('ACTIONS_CACHE_URL', process.env.ACTIONS_CACHE_URL || '');
      core.exportVariable('ACTIONS_RUNTIME_TOKEN', process.env.ACTIONS_RUNTIME_TOKEN || '');
- run: vcpkg install ffmpeg pkgconf --triplet x64-windows
```
(The first run builds FFmpeg from source — slow, ~20-40 min; subsequent runs restore from the GHA
binary cache.)

**MSVC env + configure:** set the MSVC dev environment (`ilammy/msvc-dev-cmd@v1`), then configure
with the vcpkg toolchain and point pkg-config at vcpkg's `.pc` files:
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows
# with PKG_CONFIG_PATH = <vcpkg installed>/x64-windows/lib/pkgconfig and pkgconf.exe on PATH
```
**This pkg-config-under-MSVC wiring is the highest-risk step and the most likely to need a
post-first-push tweak.**

**Build:** `cmake --build build --config Release`. The vcpkg toolchain's app-local deploy
(`VCPKG_APPLOCAL_DEPS`, on by default) copies the FFmpeg DLLs (`avcodec-*.dll`, `swscale-*.dll`,
their transitive deps) next to `shader_streamer.exe` in `build/`.

**Test:** `ctest --test-dir build -C Release -R core_tests --output-on-failure`;
`ctest --test-dir build -C Release -R gl_smoke --output-on-failure` allowed to fail (best-effort).

**Package (installer):** `choco install innosetup`; write `packaging/windows/installer.iss`
(generated in-workflow or committed) that takes everything in `build/` matching `shader_streamer.exe`
+ `*.dll` + the `build/shaders/` tree, installs to `{autopf}\Shader Streamer`, and creates a
Start-menu shortcut with **`WorkingDir: {app}`**. `iscc` produces
`Shader_Streamer-${VERSION}-setup.exe`. **Optional Authenticode signing** gated on a cert secret.

## Release wiring

Non-tag builds: only `actions/upload-artifact@v4` (name e.g. `linux-appimage`,
`macos-app-${{ matrix.arch }}` — distinct per arch so the two macOS legs don't collide —
`windows-installer`). Tag builds additionally:
```yaml
- if: startsWith(github.ref, 'refs/tags/v')
  uses: softprops/action-gh-release@v2
  with:
    files: <the one package file>
```
GitHub creates the release for the tag on first asset and each workflow appends its own package, so
one tag yields a release with all three packages.

## Error / edge handling

- **No GL context in CI** → `gl_smoke` self-skips (it already guards on `glfwCreateWindow`); the
  step is non-fatal regardless.
- **Missing signing secrets** → signing steps are `if`-gated on secret presence and skipped; the
  build still produces an unsigned, runnable package.
- **vcpkg FFmpeg first-build cost** → mitigated by GHA binary caching; the first run is slow but
  correct.
- **FetchContent slowness (draco)** → `build/_deps` cached across runs keyed on `CMakeLists.txt`.
- **Package can't find shaders** → prevented by the per-package CWD handling described above.

## Testing / verification

These workflows **cannot be executed in the development environment** (they only run on GitHub's
hosted runners). Verification is therefore:

1. **YAML validity** — each workflow parses as valid YAML (checked with a YAML parser; `actionlint`
   if available).
2. **Dependency cross-check** — the apt/brew/vcpkg package lists are checked against the actual
   `CMakeLists.txt` requirements (FFmpeg 5 modules, GTK, X11, ALSA/Pulse, OpenGL).
3. **Post-merge live run** — after merge + push, watch the Actions runs and iterate. **Expect 1-2
   rounds of fixups**, most likely around the Windows pkg-config/vcpkg wiring, `dylibbundler` paths,
   and `linuxdeploy` dependency gathering. This is normal for first-time cross-platform native CI.

The plan will note that the "green build" gate is deferred to the live GitHub run, not a local
command.

## Out of scope (YAGNI — flag to pull in)

Universal (fat) macOS binary — both arches ship as **separate** native packages instead, since
Homebrew FFmpeg isn't universal; configuring the actual signing/notarization secrets;
`.deb`/`.rpm` or Homebrew formula; ARM Linux / Linux-on-`ubuntu-latest`; auto-update; caching the
full build tree (only `_deps` is cached); the executable-relative shader-resolution code change.

## Decided defaults (flag to change)

- Three files, per-OS; triggers on push(main/develop)+PR+`v*` tags; native packages; unsigned by
  default.
- Linux `ubuntu-22.04` + AppImage; macOS matrix (`macos-14` arm64 + `macos-13` x64), a per-arch
  `.app` zip each; Windows `windows-latest` MSVC+vcpkg + Inno installer.
- `core_tests` fatal everywhere; `gl_smoke` best-effort (xvfb on Linux, self-skip elsewhere).
- Version = tag or `dev-<sha>`; `build/_deps` + vcpkg GHA cache.
