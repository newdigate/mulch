# Cross-platform build/package CI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add three GitHub Actions workflows that build the app and produce a platform-native package per OS — AppImage (Linux), `.app` zip (macOS arm64 + Intel x64), Inno Setup installer (Windows) — uploaded as artifacts on push/PR and attached to a GitHub Release on `v*` tags.

**Architecture:** Three workflow files under `.github/workflows/`, plus small committed packaging helper files under `packaging/` (AppRun, .desktop, Info.plist, launcher, Inno `.iss`). Each workflow installs the OS-specific system deps FFmpeg/GTK/X11/ALSA (Linux), brew FFmpeg (macOS), vcpkg FFmpeg+pkgconf (Windows), builds Release, runs `core_tests` (fatal) + `gl_smoke` (best-effort), packages, uploads, and on a tag attaches to the Release. Every package launches the app with `shaders/` in its CWD (the app resolves shader paths relative to CWD).

**Tech Stack:** GitHub Actions, CMake, FFmpeg (apt/brew/vcpkg), linuxdeploy (AppImage), dylibbundler (macOS), Inno Setup (Windows), softprops/action-gh-release.

**Spec:** `docs/superpowers/specs/2026-07-02-cross-platform-ci-design.md`

**Verification reality:** these workflows CANNOT run in the dev environment — they only run on GitHub's hosted runners. Each task's automated gate is therefore: the YAML parses, and the shell/plist helpers are syntactically valid. The real green-build gate is the live GitHub run after merge+push (expect 1-2 rounds of fixups, most likely the Windows vcpkg/pkg-config wiring, `dylibbundler`, and `linuxdeploy`). There are no code changes and no C++ to compile in this feature, so `ctest`/`cmake` are not run by these tasks.

---

### Task 1: Linux workflow + AppImage helpers

**Files:**
- Create: `.github/workflows/build-linux.yml`
- Create: `packaging/linux/AppRun`
- Create: `packaging/linux/shader-streamer.desktop`

- [ ] **Step 1: Create `packaging/linux/shader-streamer.desktop`**

```ini
[Desktop Entry]
Type=Application
Name=Shader Streamer
Exec=shader_streamer
Icon=shader-streamer
Categories=AudioVideo;Graphics;
Terminal=false
```

- [ ] **Step 2: Create `packaging/linux/AppRun`**

```sh
#!/bin/sh
# AppImage entry point. The app loads shaders via paths relative to the CWD, so cd into
# the bundled bin dir (which contains shaders/) before launching.
HERE="$(dirname "$(readlink -f "$0")")"
cd "$HERE/usr/bin" || exit 1
exec ./shader_streamer "$@"
```

- [ ] **Step 3: Create `.github/workflows/build-linux.yml`**

```yaml
name: build-linux

on:
  push:
    branches: [main, develop]
    tags: ['v*']
  pull_request:

permissions:
  contents: write

jobs:
  build:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4

      - name: Compute version
        id: ver
        run: |
          if [ "${GITHUB_REF_TYPE}" = "tag" ]; then
            echo "version=${GITHUB_REF_NAME}" >> "$GITHUB_OUTPUT"
          else
            echo "version=dev-$(git rev-parse --short HEAD)" >> "$GITHUB_OUTPUT"
          fi

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends \
            build-essential cmake ninja-build pkg-config git wget file \
            libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
            xorg-dev libgl1-mesa-dev \
            libgtk-3-dev \
            libasound2-dev libpulse-dev \
            xvfb libgl1-mesa-dri \
            libfuse2 imagemagick desktop-file-utils

      - name: Cache FetchContent deps
        uses: actions/cache@v4
        with:
          path: build/_deps
          key: linux-deps-${{ hashFiles('CMakeLists.txt') }}

      - name: Configure
        run: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

      - name: Build
        run: cmake --build build -j

      - name: Test (core, fatal)
        run: ctest --test-dir build -R core_tests --output-on-failure

      - name: Test (gl_smoke, best-effort)
        continue-on-error: true
        run: xvfb-run -a ctest --test-dir build -R gl_smoke --output-on-failure

      - name: Assemble AppDir
        run: |
          set -eux
          APPDIR=AppDir
          mkdir -p "$APPDIR/usr/bin" \
                   "$APPDIR/usr/share/applications" \
                   "$APPDIR/usr/share/icons/hicolor/256x256/apps"
          cp build/shader_streamer "$APPDIR/usr/bin/"
          cp -r build/shaders "$APPDIR/usr/bin/shaders"
          cp packaging/linux/shader-streamer.desktop "$APPDIR/usr/share/applications/shader-streamer.desktop"
          cp packaging/linux/AppRun "$APPDIR/AppRun"
          chmod +x "$APPDIR/AppRun"
          convert -size 256x256 xc:'#1a1c22' \
            "$APPDIR/usr/share/icons/hicolor/256x256/apps/shader-streamer.png"

      - name: Build AppImage
        run: |
          set -eux
          wget -q https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
          chmod +x linuxdeploy-x86_64.AppImage
          export APPIMAGE_EXTRACT_AND_RUN=1
          ./linuxdeploy-x86_64.AppImage \
            --appdir AppDir \
            --desktop-file AppDir/usr/share/applications/shader-streamer.desktop \
            --icon-file AppDir/usr/share/icons/hicolor/256x256/apps/shader-streamer.png \
            --output appimage
          built=$(ls *.AppImage | grep -v linuxdeploy | head -1)
          mv "$built" "Shader_Streamer-${{ steps.ver.outputs.version }}-x86_64.AppImage"

      - name: Upload artifact
        uses: actions/upload-artifact@v4
        with:
          name: linux-appimage
          path: Shader_Streamer-*-x86_64.AppImage

      - name: Attach to release
        if: startsWith(github.ref, 'refs/tags/v')
        uses: softprops/action-gh-release@v2
        with:
          files: Shader_Streamer-*-x86_64.AppImage
```

- [ ] **Step 4: Verify (YAML + shell syntax)**

Run:
```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/build-linux.yml')); print('yaml ok')"
sh -n packaging/linux/AppRun && echo "AppRun ok"
```
Expected: `yaml ok` and `AppRun ok` (no syntax errors).

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/build-linux.yml packaging/linux/AppRun packaging/linux/shader-streamer.desktop
git commit -m "ci: Linux build + AppImage packaging workflow"
```

---

### Task 2: macOS workflow (arm64 + x64) + .app helpers

**Files:**
- Create: `.github/workflows/build-macos.yml`
- Create: `packaging/macos/Info.plist`
- Create: `packaging/macos/launch`

- [ ] **Step 1: Create `packaging/macos/launch`**

```sh
#!/bin/sh
# .app entry point (CFBundleExecutable). The app loads shaders via paths relative to the
# CWD, so cd into Contents/MacOS (which contains shaders/) before launching.
cd "$(dirname "$0")" || exit 1
exec ./shader_streamer "$@"
```

- [ ] **Step 2: Create `packaging/macos/Info.plist`**

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Shader Streamer</string>
  <key>CFBundleDisplayName</key><string>Shader Streamer</string>
  <key>CFBundleIdentifier</key><string>com.newdigate.shaderstreamer</string>
  <key>CFBundleVersion</key><string>1.0</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>launch</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
```

- [ ] **Step 3: Create `.github/workflows/build-macos.yml`**

```yaml
name: build-macos

on:
  push:
    branches: [main, develop]
    tags: ['v*']
  pull_request:

permissions:
  contents: write

jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        include:
          - { runner: macos-14, arch: arm64 }
          - { runner: macos-13, arch: x64 }
    runs-on: ${{ matrix.runner }}
    steps:
      - uses: actions/checkout@v4

      - name: Compute version
        id: ver
        run: |
          if [ "${GITHUB_REF_TYPE}" = "tag" ]; then
            echo "version=${GITHUB_REF_NAME}" >> "$GITHUB_OUTPUT"
          else
            echo "version=dev-$(git rev-parse --short HEAD)" >> "$GITHUB_OUTPUT"
          fi

      - name: Install dependencies
        run: brew install ffmpeg pkg-config dylibbundler cmake ninja

      - name: Cache FetchContent deps
        uses: actions/cache@v4
        with:
          path: build/_deps
          key: macos-${{ matrix.arch }}-deps-${{ hashFiles('CMakeLists.txt') }}

      - name: Configure
        run: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

      - name: Build
        run: cmake --build build -j

      - name: Test (core, fatal)
        run: ctest --test-dir build -R core_tests --output-on-failure

      - name: Test (gl_smoke, best-effort)
        continue-on-error: true
        run: ctest --test-dir build -R gl_smoke --output-on-failure

      - name: Assemble .app bundle
        run: |
          set -eux
          APP="Shader Streamer.app"
          mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/libs"
          cp packaging/macos/Info.plist "$APP/Contents/Info.plist"
          cp build/shader_streamer "$APP/Contents/MacOS/shader_streamer"
          cp -r build/shaders "$APP/Contents/MacOS/shaders"
          cp packaging/macos/launch "$APP/Contents/MacOS/launch"
          chmod +x "$APP/Contents/MacOS/launch" "$APP/Contents/MacOS/shader_streamer"
          dylibbundler -od -b \
            -x "$APP/Contents/MacOS/shader_streamer" \
            -d "$APP/Contents/libs" \
            -p "@executable_path/../libs"

      - name: Zip .app
        run: |
          ditto -c -k --keepParent "Shader Streamer.app" \
            "Shader_Streamer-${{ steps.ver.outputs.version }}-macos-${{ matrix.arch }}.zip"

      - name: Upload artifact
        uses: actions/upload-artifact@v4
        with:
          name: macos-app-${{ matrix.arch }}
          path: Shader_Streamer-*-macos-${{ matrix.arch }}.zip

      - name: Attach to release
        if: startsWith(github.ref, 'refs/tags/v')
        uses: softprops/action-gh-release@v2
        with:
          files: Shader_Streamer-*-macos-${{ matrix.arch }}.zip
```

- [ ] **Step 4: Verify (YAML + shell + plist)**

Run:
```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/build-macos.yml')); print('yaml ok')"
sh -n packaging/macos/launch && echo "launch ok"
python3 -c "import plistlib; plistlib.load(open('packaging/macos/Info.plist','rb')); print('plist ok')"
```
Expected: `yaml ok`, `launch ok`, `plist ok`.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/build-macos.yml packaging/macos/Info.plist packaging/macos/launch
git commit -m "ci: macOS build + .app packaging (arm64 + Intel x64)"
```

---

### Task 3: Windows workflow + Inno Setup installer

**Files:**
- Create: `.github/workflows/build-windows.yml`
- Create: `packaging/windows/installer.iss`

- [ ] **Step 1: Create `packaging/windows/installer.iss`**

The workflow copies the vcpkg release DLLs into the build dir before packaging, so this script
sources everything (exe + DLLs + shaders) from a single `SourceDir`. The Start-menu shortcut's
`WorkingDir` is the install dir so the app finds `shaders/`.

```pascal
; Inno Setup script for Shader Streamer.
; Invoke: iscc /DMyVersion=<ver> /DSourceDir=<abs path to build> packaging\windows\installer.iss
#ifndef MyVersion
  #define MyVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build"
#endif

[Setup]
AppName=Shader Streamer
AppVersion={#MyVersion}
AppPublisher=newdigate
DefaultDirName={autopf}\Shader Streamer
DefaultGroupName=Shader Streamer
UninstallDisplayIcon={app}\shader_streamer.exe
OutputDir=dist
OutputBaseFilename=Shader_Streamer-{#MyVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Files]
Source: "{#SourceDir}\shader_streamer.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\*.dll";               DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\shaders\*";           DestDir: "{app}\shaders"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; WorkingDir: {app} so the app's CWD contains shaders\ at launch.
Name: "{group}\Shader Streamer";           Filename: "{app}\shader_streamer.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall Shader Streamer"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\shader_streamer.exe"; WorkingDir: "{app}"; Description: "Launch Shader Streamer"; Flags: nowait postinstall skipifsilent
```

- [ ] **Step 2: Create `.github/workflows/build-windows.yml`**

```yaml
name: build-windows

on:
  push:
    branches: [main, develop]
    tags: ['v*']
  pull_request:

permissions:
  contents: write

env:
  VCPKG_BINARY_SOURCES: "clear;x-gha,readwrite"

jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4

      - name: Compute version
        id: ver
        shell: bash
        run: |
          if [ "${GITHUB_REF_TYPE}" = "tag" ]; then
            echo "version=${GITHUB_REF_NAME}" >> "$GITHUB_OUTPUT"
          else
            echo "version=dev-$(git rev-parse --short HEAD)" >> "$GITHUB_OUTPUT"
          fi

      - name: Export GitHub Actions cache env for vcpkg
        uses: actions/github-script@v7
        with:
          script: |
            core.exportVariable('ACTIONS_CACHE_URL', process.env.ACTIONS_CACHE_URL || '');
            core.exportVariable('ACTIONS_RUNTIME_TOKEN', process.env.ACTIONS_RUNTIME_TOKEN || '');

      - name: Install FFmpeg + pkgconf via vcpkg
        shell: pwsh
        run: |
          & "$env:VCPKG_INSTALLATION_ROOT\vcpkg.exe" install ffmpeg pkgconf --triplet x64-windows

      - name: Set up MSVC
        uses: ilammy/msvc-dev-cmd@v1

      - name: Configure
        shell: pwsh
        run: |
          $vcpkg = $env:VCPKG_INSTALLATION_ROOT
          $installed = "$vcpkg\installed\x64-windows"
          $env:PKG_CONFIG_PATH = "$installed\lib\pkgconfig"
          $env:PATH = "$installed\tools\pkgconf;$env:PATH"
          cmake -S . -B build -G Ninja `
            -DCMAKE_BUILD_TYPE=Release `
            -DCMAKE_TOOLCHAIN_FILE="$vcpkg\scripts\buildsystems\vcpkg.cmake" `
            -DVCPKG_TARGET_TRIPLET=x64-windows

      - name: Build
        shell: pwsh
        run: cmake --build build

      - name: Test (core, fatal)
        shell: pwsh
        run: ctest --test-dir build -R core_tests --output-on-failure

      - name: Test (gl_smoke, best-effort)
        continue-on-error: true
        shell: pwsh
        run: ctest --test-dir build -R gl_smoke --output-on-failure

      - name: Stage runtime DLLs next to the exe
        shell: pwsh
        run: |
          $installed = "$env:VCPKG_INSTALLATION_ROOT\installed\x64-windows"
          Copy-Item "$installed\bin\*.dll" build\ -Force

      - name: Build installer (Inno Setup)
        shell: pwsh
        run: |
          choco install innosetup --no-progress -y
          $iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
          & "$iscc" `
            "/DMyVersion=${{ steps.ver.outputs.version }}" `
            "/DSourceDir=$(Resolve-Path build)" `
            packaging\windows\installer.iss
          Get-ChildItem packaging\windows\dist

      - name: Upload artifact
        uses: actions/upload-artifact@v4
        with:
          name: windows-installer
          path: packaging/windows/dist/*setup.exe

      - name: Attach to release
        if: startsWith(github.ref, 'refs/tags/v')
        uses: softprops/action-gh-release@v2
        with:
          files: packaging/windows/dist/*setup.exe
```

- [ ] **Step 3: Verify (YAML + installer script present)**

Run:
```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/build-windows.yml')); print('yaml ok')"
grep -q 'WorkingDir: "{app}"' packaging/windows/installer.iss && echo "iss shortcut workdir ok"
grep -q 'OutputBaseFilename=Shader_Streamer-{#MyVersion}-setup' packaging/windows/installer.iss && echo "iss output name ok"
```
Expected: `yaml ok`, `iss shortcut workdir ok`, `iss output name ok`.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/build-windows.yml packaging/windows/installer.iss
git commit -m "ci: Windows build (MSVC+vcpkg) + Inno Setup installer"
```

---

### Task 4: Documentation

**Files:**
- Modify: `README.md`
- Modify: `CLAUDE.md`

- [ ] **Step 1: Add a Downloads/CI section to README.md**

In `README.md`, after the build-from-source instructions (search for the `cmake -S . -B build` /
Build section), add this section:

```markdown
## Downloads & CI

Three GitHub Actions workflows (`.github/workflows/build-{linux,macos,windows}.yml`) build the app
and produce native packages on every push to `main`/`develop` and on pull requests (downloadable
from the Actions run), and attach them to a GitHub Release when a `v*` tag is pushed:

- **Linux** → AppImage (`ubuntu-22.04`; bundles FFmpeg/GTK via `linuxdeploy`).
- **macOS** → `.app` zip for both Apple-Silicon **arm64** (`macos-14`) and Intel **x64** (`macos-13`);
  FFmpeg dylibs bundled with `dylibbundler`, so no Homebrew needed at runtime.
- **Windows** → Inno Setup installer (`windows-latest`, MSVC + vcpkg FFmpeg).

Packages are **unsigned**, so first launch goes through the OS "unidentified developer" prompt
(macOS: right-click → Open, or `xattr -dr com.apple.quarantine`); code-signing / notarization can be
added later by wiring the relevant repo secrets. Packaging keeps `shaders/` beside the executable and
launches with that directory as the working directory, so the shipped app finds its shaders.
```

- [ ] **Step 2: Add a CLAUDE.md note**

In `CLAUDE.md`, under the "## Build, run, test" section (after the `ctest` line / the FFmpeg
system-dep paragraph), add this paragraph:

```markdown
**CI / packaging.** `.github/workflows/build-{linux,macos,windows}.yml` build + package the app per
OS (Linux AppImage via `linuxdeploy`; macOS `.app` for arm64 `macos-14` + Intel x64 `macos-13`,
dylibs bundled via `dylibbundler`; Windows Inno Setup installer via MSVC + vcpkg FFmpeg). They run on
push/PR (artifacts) and attach to a Release on `v*` tags. Packaging helper files live in
`packaging/{linux,macos,windows}/`. Because `ShaderNode` loads shaders by CWD-relative path, each
package launches the app with `shaders/` as the working directory (AppRun / `.app` launcher /
installer shortcut `WorkingDir`).
```

- [ ] **Step 3: Verify + Commit**

Run:
```bash
grep -q "Downloads & CI" README.md && grep -q "CI / packaging" CLAUDE.md && echo "docs ok"
git add README.md CLAUDE.md
git commit -m "docs: document the cross-platform build/package CI"
```
Expected: `docs ok`, then the commit.

---

## Notes for the implementer

- **These workflows cannot be run locally** — the automated gate per task is "YAML parses + helper scripts/plist are syntactically valid". No `cmake`/`ctest` is run (there is no C++ change). The real validation is the live GitHub run after merge+push.
- **Highest-risk spots** (call out in your report, don't try to pre-fix beyond what's written): the Windows vcpkg + pkg-config wiring (`PKG_CONFIG_PATH`, pkgconf on PATH), `dylibbundler` resolving the FFmpeg dylibs without prompting, and `linuxdeploy` gathering the GTK/FFmpeg `.so` deps + honoring the custom `AppRun`.
- **`.iss` architecture keyword:** `x64compatible` requires Inno Setup 6.3+. choco's current `innosetup` provides 6.x ≥ 6.3, so it's fine; if a live run errors here, the fixup is to use `x64` instead.
- **Do NOT** `git add -A` / `git add .`. Stage only the files listed in each commit step. Leave the untracked `build.sh`, `preferences.oss`, `project.oss`, `examples/`, `imgui.ini` alone.
- Reproduce the file contents **exactly** as written (YAML is whitespace-sensitive; the Inno `.iss` and the shell scripts must be verbatim). Do not add extra steps or "improvements".
