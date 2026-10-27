# Building from source

This guide describes the repository's current CMake paths. Build outputs belong under `build/`; do not place tests or probes in a populated deployment. The project uses C++20, Qt Widgets and desktop OpenGL; it does not use Qt Quick/RHI.

## Platforms and prerequisites

| Target | Toolchain | Qt | Playback runtime |
|---|---|---|---|
| Windows x64 | MinGW-w64 GCC 13.1.0, MinGW Makefiles | Qt 6.12.0 `win64_mingw` kit and LinguistTools | Pinned mpv DEV archive provides headers, import library and `libmpv-2.dll`; the separate CLI/updater archive is not used |
| Linux x86_64 | GCC or Clang, Unix Makefiles | Qt 6.12+ (`Core`, `Gui`, `Widgets`, `OpenGL`, `OpenGLWidgets`, `Network`, `LinguistTools`; `Test` when probes/tests enabled) | Developer builds need libmpv headers and ABI 2 library. Runtime may use configured host `libmpv.so.2` or the opt-in pinned private bundle |

CMake 3.25+ is required (preset schema v6). Both targets need a desktop OpenGL-capable environment. Optional AI paths require separate GPU-specific components and do not define the baseline player's requirements.

On Debian/Ubuntu, install the compiler/build tools, desktop OpenGL development headers, Qt tools and mpv development headers/library. Even the bundled-runtime profile still needs development headers when compiling; add the distro mpv development package, for example:

```bash
sudo apt-get install build-essential cmake qt6-base-dev qt6-tools-dev qt6-tools-dev-tools libgl1-mesa-dev libmpv-dev p7zip-full
```

The opt-in `-DMIKAI_LINUX_BUNDLE_MPV=ON` profile needs build-only `patchelf` to set private-library `$ORIGIN` RUNPATHs. On Debian/Ubuntu, install it when enabling that profile:

```bash
sudo apt-get install patchelf
```

The default Linux developer configuration uses system-lib-mpv mode: it needs development headers and an ABI 2 library at configure/build time, and selects the canonical configured host `libmpv.so.2` module at runtime. System mode validates that module itself, not its dependency closure; the host dynamic loader and runtime dependencies remain the host's responsibility. Set `-DMIKAI_LINUX_BUNDLE_MPV=ON` to build against the developer headers while selecting the pinned Linux archive for runtime; this stages `libmpv` and its private dependencies under `bin/mpv` and applies `$ORIGIN` RUNPATH to owned copies using build-time `patchelf`. A requested bundle fails rather than silently falling back to system `libmpv`. Neither runtime mode removes host glibc/loader, graphics-driver (including relevant EGL/Vulkan components) or audio requirements. The bundled profile is not a universal/standalone Linux package. A current fresh-tree build and archive receipt is recorded in [release status](release-status.md); this does not establish runtime usability or broad distribution portability.

The build needs a Qt 6.12+ development kit matching the active compiler/toolchain. Linux package staging can optionally deploy Qt with `-DMIKAI_LINUX_DEPLOY_QT=ON`; this is independent of `MIKAI_LINUX_BUNDLE_MPV` and does not itself establish redistribution clearance or eliminate the host glibc, graphics and audio baseline. When Qt deployment is off, runtime Qt libraries/plugins must be provided by the host. Actual compatibility also depends on plugins, drivers and the selected mpv runtime.

CMake generates `mikai-runtime-$<CONFIG>.json` with schema `mikai.runtime.bundle/1` and the selected module path, dependency root, `bundled` boolean and platform. CI uses this build receipt to check that its renderer smoke is pointed at the configured profile; for bundled Linux smoke, `mikai_test_launcher --mpv-runtime-dir <path>` stages the private mpv dependency closure. This test-launcher argument is not an application command-line option. The Linux CI recipe configures both `MIKAI_LINUX_BUNDLE_MPV=ON` and `MIKAI_LINUX_DEPLOY_QT=ON`; that configuration alone is not evidence of a completed build, runtime check or redistribution clearance.

On Windows, the pinned mpv DEV archive is `mpv-dev-x86_64-20261024-git-b4f9f5bfed.7z` (release `2026-10-24-b4f9f5bfed`, SHA-256 `2efe77b9e3028c32cd74c9d871c5d2cd8315758d0da3f59bf64c867c8e7e6d0c`, 32,965,168 bytes). It supplies headers, import library (`libmpv.dll.a`, 53,308 bytes) and `libmpv-2.dll` (122,579,968 bytes, SHA-256 `d22a8a765abe26ac94614b0f09d1d78b3ceafb4f2d70fd904909232e0784c4a3`); its version is `mpv v0.41.0-dev-gb4f9f5bfe` (FFmpeg `N-128018-ga881f0a9a`, upstream commit `b4f9f5bfed18d5ce610af28024986410dd9bdaae`). CMake fetches it or accepts a local copy through `MIKAI_DIST_MPV_ARCHIVE`, verifying the archive digest. This DEV archive is the build/runtime input; the separate CLI/updater archive is neither used nor bundled. The CLI archive is distinct (`mpv-x86_64-20261024-git-b4f9f5bfed.7z`, 33,755,796 bytes, SHA-256 `3ba568335b83fa19f2b3c2a63b5264ffd57406e173e0401faebb1ac2f59b3154`) and contains no libmpv DLL. The pinned DEV archive is not corresponding source, which remains a redistribution gate. The mpv DLL imports `vulkan-1.dll`, normally supplied by graphics drivers. A deployed app also needs Qt, image-format/TLS, MinGW and mpv runtime files; use the packaging path rather than copying the executable alone.

For CI, `packaging/ci/provision.py` acquires Qt 6.12.0 for `win64_mingw` or `linux_gcc_64` with `aqtinstall` 3.3.0. The Windows kit's official repository metadata identifies MinGW 13.1.0; its matching compiler package is `qt.tools.win64_mingw1310` from tool group `tools_mingw1310`. The corresponding Qt repository is [Qt 6.12.0 MinGW](https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6120/qt6_6120_mingw/), and the compiler is published in [Qt's MinGW 13.1.0 tools repository](https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_mingw1310/). The pinned acquisition client is documented at [aqtinstall](https://aqtinstall.readthedocs.io/en/latest/cli.html); it retrieves Qt archives from Qt's repositories and does not provide …

## Configure and build

The repository's Windows `release` preset encodes machine-local tool paths, so do not assume those defaults exist on another workstation. Configure explicitly with installed tool paths instead. In PowerShell, substitute the actual locations:

```powershell
cmake -S . -B build/release -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER="C:/path/to/mingw64/bin/g++.exe" `
  -DCMAKE_MAKE_PROGRAM="C:/path/to/mingw64/bin/mingw32-make.exe" `
  -DCMAKE_PREFIX_PATH="C:/path/to/Qt/6.12.x/mingw_64"
cmake --build build/release --target mikai --parallel 4
```

For Linux, configure with the Linux preset (or explicit Unix Makefiles configuration):

```bash
cmake --preset linux-release
cmake --build --preset app-linux --parallel
```

The preset builds under `build/linux-release`. If Qt is not in the system CMake search path, pass `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.12+` at configure time, for example with an explicit `cmake -S . -B build/linux-release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=...` invocation. Ensure the Qt installation matches the active compiler/toolchain.

To build in system-libmpv mode explicitly:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.12+/gcc_64 \
  -DCMAKE_CXX_COMPILER=/path/to/Qt/6.12+/gcc_64/bin/g++ \
  -DMIKAI_LINUX_BUNDLE_MPV=OFF
cmake --build build --parallel 2 --target mikai
```

To build in pinned private-bundle mode:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.12+/gcc_64 \
  -DCMAKE_CXX_COMPILER=/path/to/Qt/6.12+/gcc_64/bin/g++ \
  -DMIKAI_LINUX_BUNDLE_MPV=ON \
  -DMIKAI_LINUX_MPV_ARCHIVE=/path/to/mpv-linux-x64-2026-10-24-c493f1c.tar.zst
cmake --build build --parallel 2 --target mikai
```

The pinned Linux archive is `mpv-linux-x64-2026-10-24-c493f1c.tar.zst`, SHA-256 `b6281a2c924157a01f4a66f49dda3029b11378ffd982a974cdcbdb474c0befc7`, 41,650,081 bytes, source commit `c493f1c7e10a6b2301653596e7deeb91295d3148`. It contains `libmpv.so`, `libmpv.so.2` and `libmpv.so.2.6.0` (each 3,708,024 bytes, SHA-256 `f50c5195aececc060202c2128113d88a9fc0a463c2e5fc9694f8e40349e96768`), an mpv CLI that is not bundled, and 78 files total. It contains no headers or notices. The bundle mode requires `patchelf` at configure/build time; requested bundle failures are fatal and do not silently fall back to system libmpv. Its developer build still uses system development headers; the archive is a runtime bundle, not a Linux development SDK.

`MIKAI_DIST_MPV_ARCHIVE` is Windows-only. On Linux it is rejected by CMake; Linux developer builds use system headers/library selection or the separate `MIKAI_LINUX_MPV_ARCHIVE` override. The two archive variables and their SHA checks are intentionally separate.

A local `dist` target assembles a staging tree at the configured distribution path. Linux public releases must use Release configuration; `linux-release` does this for the single-config Unix Makefiles generator. Public Linux staging and archive commands reject optional model inspection, Native support and native distribution because their redistribution status is not established. These packaging restrictions do not disable those features in developer builds; native inference still requires its supported platform, external source, rights and runtime prerequisites.

```sh
cmake --build build/release --target dist
# or
cmake --build build/linux-release --target dist
```

The base Linux archive excludes acquired AI components, models and user state; AI acquisition is separate from packaging. Linux Qt deployment (`MIKAI_LINUX_DEPLOY_QT`) and pinned mpv bundling (`MIKAI_LINUX_BUNDLE_MPV`) are independent build options. Enabling Qt deployment does not imply mpv is bundled; enabling mpv bundling does not imply Qt is deployed. Neither profile is a claim of universal Linux portability or cleared redistribution rights. The target assembles a fresh staging directory and does not wipe an existing deployment; treat its configured output as generated output, not user data. Packaging commands require a fresh output directory: the CI builder only creates the build directory and ensures the output parent exists, while the Windows and Linux package scripts create and populate the final output directory themselves. Do not point packaging output at user data, a populated release directory, or an existing deployment. For release artifacts, follow [Releasing](releasing.md), which uses owned staging and names the output package.

## Tests and probes

Testing is enabled by default through `MIKAI_BUILD_PROBES=ON` and requires Qt Test. Build and run the CTest suite from the corresponding build directory:

```powershell
cmake --build build/release --target mikai_tests --parallel 4
ctest --test-dir build/release --output-on-failure
```

```bash
cmake --build build/linux-release --target mikai_tests --parallel
ctest --test-dir build/linux-release --output-on-failure
```

The build includes optional probes and the `mikai_tests` target when configured with probe support. Some probes require real local media, a desktop session, a disposable runtime store or optional installed components. Do not run mutable test/probe executables beside an existing deployment or native store. For the repository's focused-suite launcher rules, consult [AGENTS.md](../AGENTS.md#running-individual-test-suites) when available. A successful build/test is not inference-performance evidence.

On Unix, a desktop locale with a decimal comma can conflict with `mpv_create`'s requirement for the C numeric locale. After `QApplication` initializes and before workers start, the player sets `LC_NUMERIC` to `C`; direct libmpv core creation also enforces it. Compare rendering workers no longer change the process locale themselves. This does not change Qt's UI locale and addresses only the numeric-locale startup conflict, not graphics or other playback failures.

To check this on Linux, build the player and tests as above, then run the focused suite and play a known-good local video in the normal player:
```bash
ctest --test-dir build/linux-release -R '^mikai_tests_TestMpvAsync$' -V
```
The numeric-locale test needs a usable configured libmpv runtime and an installed decimal-comma locale (`uk_UA.UTF-8`, `de_DE.UTF-8`, or `fr_FR.UTF-8`); it skips when those prerequisites are unavailable. The suite's CTest output alone does not distinguish a skipped case from a pass. For individual QTest results, use `ctest --test-dir build/linux-release -N -V -R '^mikai_tests_TestMpvAsync$'` to print the launcher command, then run that command with `-o -,txt` appended and check for skips.

### Linux desktop acceptance

On the target Linux desktop, build as above and run the focused suites from a disposable build tree, not an existing deployment:

```bash
cmake --build build/linux-release --target mikai mikai_tests --parallel
ctest --test-dir build/linux-release --output-on-failure -R '^mikai_tests_(TestMpvAsync|TestCompareGif|TestSystemIntegration|TestAnimeJanaiRuntime|TestRuntimeSelection|TestBenchmarkReport|TestFeedRender)$'
./build/linux-release/mikai
```

In the running app, open a known-good local video with Original/processing off; no component download is needed. Require actual visible, moving video frames, not just a successful build, test exit code or diagnostic counters. Open Compare, select Original and an available Anime4K candidate, switch modes, recapture and close it during capture; verify both rendered frames and continued player playback. In PiP, move/resize where the desktop permits, restore the player window, and check that a window-manager close matches the close button without leaving hidden playback. Repeat on the intended X11 or Wayland session; Wayland compositor policy may prevent moving, resizing or keeping PiP on top.

In Settings → General → System integration, check Save/Cancel for launch at sign-in and Open With, then check the desktop's application list/default-app settings; defaults require the user's choice. Undo any integrations after testing. In Settings → Processing and Components, verify unsupported choices are reported; on supported NVIDIA hardware with compatible fork libmpv, CUDA 13 and TensorRT 11, acquire the current official AnimeJaNai v0.17.1 API/ABI-9 runtime/model, select it explicitly and verify real processed frames on the same video. The app's pinned mpv source headers expose ABI-9 filter entry points; this is source compatibility, not hardware or inference proof. The old official v0.9.0 ABI-8 pair and separately patched private MSVC Native ABI-8 cohort are historical, distinct builds, not substitutes. Manager temporal frame count, scene threshold, upscale enable/disable and temporal-fix controls are available in Processing; v0.17.1 source maps RIFE code 4262 to regular `rife_v4.26`, not heavy RIFE, which is not inference evidence. The separate ~80 MB Linux encoder archive is not needed for playback, is not automatically acquired/adopted, and has unresolved FFmpeg/redistribution requirements. This AI check requires a writable executable-adjacent `native/` directory; use a user-owned portable/build deployment, not a read-only system binary directory. A skipped or unavailable hardware check is not a pass. For Benchmark asset/report requirements, see the [user guide](user-guide.md#benchmarking). These instructions are a test plan, not a claim that the current source has passed on Linux.

## Local API credentials and state

The app can run without an API key. Never embed one in source or commit `.env`; use the app's General settings or a private local `.env`. Settings keep the compatibility identity `Mikai` / `Mikai Wrapper`. Native/model data is executable-adjacent under `native/`; logs and crash reports are executable-adjacent as well. Do not reuse or overwrite these user-state locations as build/test staging.

## Further references

- [User guide](user-guide.md) for application behavior and optional processing.
- [Public API reference](../docs.md).
- [Native bundle/build documentation](../native/README.md), [MSVC native build guide](../native/build-msvc.md), and [native component manifests](../native/manifest/).
- [Benchmark results](../bench_results.md), with explicit historical workload limitations.