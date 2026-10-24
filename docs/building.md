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

On Windows, the pinned mpv DEV archive is `mpv-dev-x86_64-20261009-git-19c0fcf895.7z` (release `2026-10-09-19c0fcf895`, SHA-256 `8801c722b5174a751d03f1cb435092c61304cd8605302fe7cae2cc77872b2d83`, 32,845,274 bytes). It supplies headers, import library and `libmpv-2.dll`; CMake fetches it or accepts a local copy through `MIKAI_DIST_MPV_ARCHIVE`, verifying the archive digest. This DEV archive is the build/runtime input; the separate mpv CLI/updater archive is neither used nor bundled. The pinned DEV archive is not corresponding source, which remains a redistribution gate. The mpv DLL imports `vulkan-1.dll`, normally supplied by graphics drivers. A deployed app also needs Qt, image-format/TLS, MinGW and mpv runtime files; use the packaging path rather than copying the executable alone.

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
  -DMIKAI_LINUX_MPV_ARCHIVE=/path/to/mpv-linux-x64-2026-10-09-19c0fcf.tar.zst
cmake --build build --parallel 2 --target mikai
```

The pinned Linux archive is `mpv-linux-x64-2026-10-09-19c0fcf.tar.zst`, SHA-256 `55af9ddbd694366ba016b9e421a6abae78f8559887b7591f7820eca8d5b2bf37`, 41,531,271 bytes, source commit `19c0fcf89595e6ab33908299207c555b67e81558`. It contains `libmpv.so`, `libmpv.so.2` and `libmpv.so.2.5.0` (each 3,675,576 bytes, SHA-256 `304482e267c6295b690c5cde7df62209172388d30e3235a7fd115ab67143f0c0`), an mpv CLI that is not bundled, and about 75 shared dependencies. It contains no headers or notices. The bundle mode requires `patchelf` at configure/build time; requested bundle failures are fatal and do not silently fall back to system libmpv. Its developer build still uses system development headers; the archive is a runtime bundle, not a Linux development SDK.

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

## Local API credentials and state

The app can run without an API key. Never embed one in source or commit `.env`; use the app's General settings or a private local `.env`. Settings keep the compatibility identity `Mikai` / `Mikai Wrapper`. Native/model data is executable-adjacent under `native/`; logs and crash reports are executable-adjacent as well. Do not reuse or overwrite these user-state locations as build/test staging.

## Further references

- [User guide](user-guide.md) for application behavior and optional processing.
- [Public API reference](../docs.md).
- [Native bundle/build documentation](../native/README.md), [MSVC native build guide](../native/build-msvc.md), and [native component manifests](../native/manifest/).
- [Benchmark results](../bench_results.md), with explicit historical workload limitations.