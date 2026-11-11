# Building from source

This guide describes the repository's current CMake paths. Build outputs belong under `build/`; do not place tests or probes in a populated deployment. The project uses C++20, Qt Widgets and desktop OpenGL; it does not use Qt Quick/RHI.

## Platforms and prerequisites

| Target | Toolchain | Qt | Playback runtime |
|---|---|---|---|
| Windows x64 | MinGW-w64, MinGW Makefiles. CI pins the Qt kit's GCC 13.1.0; a local build uses any MinGW matching the installed Qt kit | Qt 6.12.0 `win64_mingw` kit and LinguistTools | Pinned mpv DEV archive provides headers, import library and `libmpv-2.dll`; the separate CLI/updater archive is not used |
| Linux x86_64 | GCC or Clang, Unix Makefiles | Qt 6.12+ (`Core`, `Gui`, `Widgets`, `OpenGL`, `OpenGLWidgets`, `Network`, `LinguistTools`; `Test` when probes/tests enabled) | Developer builds need libmpv 0.35+ (client API 2) headers and a `libmpv.so.2` library. Runtime may use configured host `libmpv.so.2` or the opt-in pinned private bundle |

- `project(mikai VERSION 0.2.2)` and `find_package(Qt6 6.12 REQUIRED ...)` in `CMakeLists.txt` are the authority for the version and minimum Qt; `CMakePresets.json` is schema v6 with `cmake_minimum_required(VERSION 3.25)`.
- The Windows compiler must match the Qt kit it links against. The CI lane provisions Qt's own MinGW 13.1.0 (`packaging/ci/provision.py`); the recorded local package payload was built with GNU 16.2.0 from a w64devkit recipe, as `packaging/provenance/windows-runtime.json` states under `build.mingw`. The public packaging path deploys `libgcc_s_seh-1.dll`, `libstdc++-6.dll` and `libwinpthread-1.dll` from the Qt SDK when they exist there and only then falls back to the compiler directory, so the shipped runtime is the kit's, not an arbitrary PATH copy.

Both targets need a desktop OpenGL-capable environment. Optional AI paths require separate GPU-specific components and do not define the baseline player's requirements.

On Debian/Ubuntu, install the compiler/build tools, desktop OpenGL development headers, Qt tools and mpv development headers/library. Even the bundled-runtime profile still needs development headers when compiling; add the distro mpv development package, for example:

```bash
sudo apt-get install build-essential cmake qt6-base-dev qt6-tools-dev qt6-tools-dev-tools libgl1-mesa-dev libmpv-dev p7zip-full
```

The opt-in `-DMIKAI_LINUX_BUNDLE_MPV=ON` profile needs build-only `patchelf` to set private-library `$ORIGIN` RUNPATHs. On Debian/Ubuntu, install it when enabling that profile:

```bash
sudo apt-get install patchelf
```

- The default Linux developer configuration uses system-lib-mpv mode: it needs development headers and an ABI 2 library at configure/build time, and selects the canonical configured host `libmpv.so.2` module at runtime.
- System mode validates that module itself, not its dependency closure; the host dynamic loader and runtime dependencies remain the host's responsibility.
- Set `-DMIKAI_LINUX_BUNDLE_MPV=ON` to build against the developer headers while selecting the pinned Linux archive for runtime; this stages `libmpv` and its private dependencies under `MIKAI_LINUX_MPV_INSTALL_SUBDIR` (`bin/mpv` by default) and applies `$ORIGIN` RUNPATH to owned copies using build-time `patchelf`.
- A packaged build sets the subdirectory to a path that cannot collide with the distribution's `/usr/bin/mpv`.
- A requested bundle fails rather than silently falling back to system `libmpv`.
- Neither runtime mode removes host glibc/loader, graphics-driver (including relevant EGL/Vulkan components) or audio requirements.
- The bundled profile is not a universal/standalone Linux package.
- Linux package identity is recorded in [packaging provenance](../packaging/provenance/linux-runtime.json) and [releasing](releasing.md); neither establishes runtime usability or broad distribution portability.

- The build needs a Qt 6.12+ development kit matching the active compiler/toolchain.
- Linux package staging can optionally deploy Qt with `-DMIKAI_LINUX_DEPLOY_QT=ON`; configure fails without `patchelf`, which the Qt deployment script uses to give the copied plugins a relative RUNPATH. This option is independent of `MIKAI_LINUX_BUNDLE_MPV` and does not itself establish redistribution clearance or eliminate the host glibc, graphics and audio baseline.
- When Qt deployment is off, runtime Qt libraries/plugins must be provided by the host.
- Actual compatibility also depends on plugins, drivers and the selected mpv runtime.

- CMake generates `mikai-runtime-$<CONFIG>.json` with schema `mikai.runtime.bundle/1` and the selected module path, dependency root, `bundled` boolean and platform.
- CI uses this build receipt to check that its renderer smoke is pointed at the configured profile; for bundled Linux smoke, `mikai_test_launcher --mpv-runtime-dir <path>` stages the private mpv dependency closure.
- This test-launcher argument is not an application command-line option.
- The Linux CI recipe configures both `MIKAI_LINUX_BUNDLE_MPV=ON` and `MIKAI_LINUX_DEPLOY_QT=ON`; that configuration alone is not evidence of a completed build, runtime check or redistribution clearance.

- On Windows, the pinned mpv DEV archive is `mpv-dev-x86_64-20261024-git-b4f9f5bfed.7z` (release `2026-10-24-b4f9f5bfed`, 33.0 MB (32,965,168 B), SHA-256 pin `2efe77b9e302`; full digest: `../packaging/provenance/windows-runtime.json`).
- It supplies headers, import library (`libmpv.dll.a`, 53,308 bytes) and `libmpv-2.dll` (122.6 MB (122,579,968 B), SHA-256 pin `d22a8a765abe`; full digest: `../packaging/provenance/windows-runtime.json`).
- Its version is `mpv v0.41.0-dev-gb4f9f5bfe` (FFmpeg `N-128018-ga881f0a9a`, upstream commit `b4f9f5bfed18d5ce610af28024986410dd9bdaae`).

- CMake fetches the DEV archive or accepts a local copy through `MIKAI_DIST_MPV_ARCHIVE`, verifying the archive digest.

- The Anime4K shaders are third-party content and are not stored in this repository: `packaging/shaders/anime4k-manifest.json` pins one `bloc97/Anime4K` commit with a SHA-256 per file, and `cmake/FindOrFetchAnime4K.cmake` materialises them into `<build>/shaders` during configure through `cmake -P cmake/FetchAnime4K.cmake` (no interpreter beyond CMake). One tarball of the pinned commit is downloaded into `<build>/_dist-cache/anime4k/`; a staged tree whose pin already matches is reused with no network access, and `-DMIKAI_ANIME4K_ARCHIVE=<local tar.gz>` builds fully offline. Every staged file is digest-checked against the manifest, and the `mikai_anime4k_shaders` CTest entry re-checks the staged tree. A source checkout therefore contains no shader blobs at all; a pin bump is one `cmake -DMODE=pin -DCOMMIT=<sha> -P cmake/FetchAnime4K.cmake` run, documented in that README. See [`packaging/shaders/README.md`](../packaging/shaders/README.md).
- This is the build/runtime input; the separate CLI/updater archive is neither used nor bundled.
- The CLI archive, `mpv-x86_64-20261024-git-b4f9f5bfed.7z`, is 33.8 MB (33,755,796 B) with SHA-256 pin `3ba568335b83`; its full digest is in `../packaging/provenance/windows-runtime.json`.
- It contains no libmpv DLL.

- The pinned DEV archive is not corresponding source, which remains a redistribution gate.
- The mpv DLL imports `vulkan-1.dll`, normally supplied by graphics drivers.
- HTTPS trust comes from the platform, not from the mpv archive: the deployed Windows Qt TLS plugin is Schannel, which uses the OS certificate store, and no OpenSSL/LibreSSL payload is shipped (see `tls` in `../packaging/provenance/windows-runtime.json`).
- A deployed Mikai Ja Nai app also needs Qt, image-format/TLS, MinGW and mpv runtime files; use the packaging path rather than copying the executable alone.

- For CI, `packaging/ci/provision.py` acquires Qt 6.12.0 for `win64_mingw` or `linux_gcc_64`.
- On Linux it uses `aqtinstall` 3.3.0.
- On Windows it does not: aqt resolves `Updates.xml` one directory above where Qt now publishes it, so every `install-qt windows` call fails before downloading anything.
- The Windows path therefore fetches five pinned archives (qtbase, qtsvg, qttranslations, the imageformats addon and the MinGW 13.1.0 toolchain), verifies each SHA-256 and size, and extracts them into the layout the installer produces; the verified list is written to `evidence/toolchain.json`.

Re-derive a digest only from the same URL.

The Windows CI build also passes `-DMIKAI_DISABLE_IPO=ON`: GCC 13.1 MinGW's LTO rejects the duplicated weak destructor thunks this tree's multiple-inheritance widgets emit, and the link fails on `multiple definition` where a non-LTO link merges them. Pass the same flag by hand for a local MinGW GCC 13 build; it is the compiler version that matters, not whether the build is native or cross. `MIKAI_DISABLE_IPO=OFF` remains the default, so a newer MinGW keeps LTO.

- The Windows kit's official repository metadata identifies MinGW 13.1.0; its matching compiler package is `qt.tools.win64_mingw1310` from tool group `tools_mingw1310`.
- The corresponding Qt repository is [Qt 6.12.0 MinGW](https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6120/qt6_6120_mingw/), and the compiler is published in [Qt's MinGW 13.1.0 tools repository](https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_mingw1310/).
- The pinned acquisition client is documented at [aqtinstall](https://aqtinstall.readthedocs.io/en/latest/cli.html); it retrieves Qt archives from Qt's repositories and does not provide …

## Configure and build

The Windows `release` preset fixes the generator (`MinGW Makefiles`), `CMAKE_BUILD_TYPE=Release`, `MIKAI_BUILD_PROBES=ON` and `MIKAI_WITH_MODEL_INSPECTION=OFF` under `build/release`, but it deliberately does not choose a compiler or Qt: its description requires the matching MinGW/Qt tools on `PATH` and `CMAKE_PREFIX_PATH` (or `Qt6_DIR`). Configure explicitly when a workstation needs exact tool paths. In PowerShell, substitute the actual locations:

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

`CMakePresets.json` provides: `release`/`debug` (Windows MinGW, model inspection off), `native` (model inspection on, needs `-DORT_INCLUDE_DIR=<onnxruntime>/include`, not a public package), `linux-release`/`linux-debug`, and `linux-tsan`/`linux-asan`/`linux-ubsan` (Debug with the sanitizer flags on `CMAKE_CXX_FLAGS`). Build presets are `app`, `tests`, `app-native`, `tests-native`, `app-linux`, `tests-linux` and the three sanitizer test presets.

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
  -DMIKAI_LINUX_MPV_ARCHIVE=/path/to/mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst
cmake --build build --parallel 2 --target mikai
```

The pinned Linux runtime is `mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst`, 39.6 MB (41,533,948 B), SHA-256 pin `6796d922cfd3`; full digest: `../packaging/provenance/linux-runtime.json`. It comes from mpv source commit `c493f1c7e10a6b2301653596e7deeb91295d3148`.

- It is produced by `packaging/mpv/build-tls-archive.sh` from the archive published at that commit: the published archive was configured without an FFmpeg TLS backend, so every https URL failed with "No protocol handler found".
- The script rebuilds FFmpeg at the same tag with the same flags plus `--enable-openssl`, replaces that family inside the archive and adds OpenSSL 3.0 from the Ubuntu 22.04 baseline.
- Everything else — the mpv fork with `vf_animejanai`, libplacebo, shaderc, libass, and the Vulkan 1.4.363 loader (upstream mpv needs ≥ 1.4.309; `vkEnumerateInstanceVersion` reports `1.4.363`) — is the published archive's bytes.

- `release-artifacts/` is the default search location; `-DMIKAI_LINUX_MPV_ARCHIVE=` overrides it, and configure fails rather than falling back to the TLS-less archive.
- CI builds the file on demand (`packaging/ci/build.py` runs the script when it is absent); it is not committed.
- The bundle installs to `MIKAI_LINUX_MPV_INSTALL_SUBDIR` (`bin/mpv` by default, `lib/mikai/mpv` where a distribution package owns `/usr/bin/mpv`).
- The mode requires `patchelf`; requested bundle failures are fatal and never fall back to system libmpv.
- Its developer build still uses system development headers; the archive is a runtime bundle, not a Linux development SDK.

- The archive carries OpenSSL 3.0 and no CA bundle; certificate trust is whatever the host's OpenSSL default paths resolve, so a packaged player on a machine without a system CA store cannot verify `https` endpoints. That is a host requirement like glibc and the graphics driver, not something the bundle ends.
- A rebuild of the TLS runtime is not byte-reproducible — `packaging/mpv/build-tls-archive.sh` compiles FFmpeg inside a distribution image whose package set drifts — so CI builds its own, passes `-DMIKAI_LINUX_MPV_EXPECTED_SHA256=<digest of that build>` and records the digest in `evidence/linux-mpv-runtime.json`.
- The runtime receipt then reports `archivePinnedRelease: false`, and configure still refuses any archive that does not match the digest it was given.
- Without the override the shipped pin applies, so an ordinary configure cannot bundle a different runtime.

`MIKAI_DIST_MPV_ARCHIVE` is Windows-only. On Linux it is rejected by CMake; Linux developer builds use system headers/library selection or the separate `MIKAI_LINUX_MPV_ARCHIVE` override. The two archive variables and their SHA checks are intentionally separate.

- Both platforms have a local `dist` target; it writes into `MIKAI_DIST_DIR` (`-DMIKAI_DIST_DIR=...`, default `<source>/dist-mikai`) and refuses a populated path rather than refreshing one.
- On Windows `dist` runs `windeployqt`, copies the pinned `libmpv-2.dll` and assembles the self-contained folder. It is developer convenience output, not a shippable package: `packaging/windows/package.ps1` stages its own payload through the generated `build/mikai-package-Release.cmake`, which includes the same `cmake/dist.cmake` with `MIKAI_PUBLIC_RELEASE` set.
- On Linux `dist` only runs the install rules into that staging tree (`packaging/linux/Stage.cmake`); it produces no archive. Use `packaging/linux/package.sh` for the tarball.
- Linux public releases must use Release configuration; `linux-release` does this for the single-config Unix Makefiles generator.
- Public Linux staging and archive commands reject optional model inspection, Native support and native distribution because their redistribution status is not established.
- These packaging restrictions do not disable those features in developer builds; native inference still requires its supported platform, external source, rights and runtime prerequisites.

```sh
cmake --build build/release --target dist
# or
cmake --build build/linux-release --target dist
```

- The base Linux archive excludes acquired AI components, models and user state; AI acquisition is separate from packaging.
- Linux Qt deployment (`MIKAI_LINUX_DEPLOY_QT`) and pinned mpv bundling (`MIKAI_LINUX_BUNDLE_MPV`) are independent build options.
- Enabling Qt deployment does not imply mpv is bundled; enabling mpv bundling does not imply Qt is deployed.
- Neither profile is a claim of universal Linux portability or cleared redistribution rights.
- The target assembles a fresh staging directory and does not wipe an existing deployment; treat its configured output as generated output, not user data.
- Packaging commands require a fresh output directory: the CI builder only creates the build directory and ensures the output parent exists, while the Windows and Linux package scripts create and populate the final output directory themselves.
- Do not point packaging output at user data, a populated release directory, or an existing deployment.
- For release artifacts, follow [Releasing](releasing.md), which uses owned staging and names the output package.

The Windows distribution assembly prefers SDK C++ runtime files and then verifies the pair the Windows loader actually uses: if `Qt6Core.dll` imports `__cxa_call_terminate`, the deployed `libstdc++-6.dll` must export it. `cmake/dist.cmake` does this with the configured `objdump` for the public package, and `packaging/windows/assemble-payload.py` (the runtime-contract check used by CI) applies the same rule. A successful package assembly does not establish that the installer has been installed or that the package works on a clean machine; see [release status](release-status.md) for the locally recorded startup check and its limits.

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

- The build includes optional probes and the `mikai_tests` target when configured with probe support.
- Some probes require real local media, a desktop session, a disposable runtime store or optional installed components.
- Do not run mutable test/probe executables beside an existing deployment or native store.
- For the repository's focused-suite launcher rules, consult [AGENTS.md](../AGENTS.md#running-individual-test-suites) when available.
- A successful build/test is not inference-performance evidence.

Every build stamps its own identity, so two builds of the same `0.1.0` can be told apart:

```bash
./build/linux-release/mikai --version
mikai 0.1.0 (9372c03, built 2026-10-28T21:55:18Z)
```

`--version`/`-V` is answered in `src/app/main.cpp` before `QApplication` exists, so it works over ssh and without a display.

The revision comes from `git rev-parse --short HEAD` with `-dirty` when tracked files are modified, and the stamp is UTC at configure time. A cross build whose container cannot read the checkout states it instead: `-DMIKAI_BUILD_REVISION=<short-rev>`; `packaging/arch/PKGBUILD` passes it that way. Startup logs the same three values as `mikai: build <version> (<revision>, <stamp>)`, and `applicationVersion` is the shorter `<version> (<revision>)` that benchmark reports and crash logs carry.

A MinGW GCC 13 build needs `-DMIKAI_DISABLE_IPO=ON`: LTO refuses the duplicated weak destructor thunks this tree produces for a `QObject` with two bases, which a non-LTO link merges. The default stays LTO for other toolchains.

- On Unix, a desktop locale with a decimal comma can conflict with `mpv_create`'s requirement for the C numeric locale.
- After `QApplication` initializes and before workers start, the player sets `LC_NUMERIC` to `C`; direct libmpv core creation also enforces it.
- Compare rendering workers no longer change the process locale themselves.
- This does not change Qt's UI locale and addresses only the numeric-locale startup conflict, not graphics or other playback failures.

To check this on Linux, build the player and tests as above, then run the focused suite and play a known-good local video in the normal player:
```bash
ctest --test-dir build/linux-release -R '^mikai_tests_TestMpvAsync$' -V
```
- The numeric-locale test needs a usable configured libmpv runtime and an installed decimal-comma locale (`uk_UA.UTF-8`, `de_DE.UTF-8`, or `fr_FR.UTF-8`); it skips when those prerequisites are unavailable.
- The suite's CTest output alone does not distinguish a skipped case from a pass.
- For individual QTest results, use `ctest --test-dir build/linux-release -N -V -R '^mikai_tests_TestMpvAsync$'` to print the launcher command, then run that command with `-o -,txt` appended and check for skips.

### Linux desktop acceptance

On the target Linux desktop, build as above and run the focused suites from a disposable build tree, not an existing deployment:

```bash
cmake --build build/linux-release --target mikai mikai_tests --parallel
ctest --test-dir build/linux-release --output-on-failure -R '^mikai_tests_(TestMpvAsync|TestCompareGif|TestSystemIntegration|TestAnimeJanaiRuntime|TestRuntimeSelection|TestBenchmarkReport|TestFeedRender)$'
./build/linux-release/mikai
```

- In the running app, open a known-good local video with Original/processing off; no component download is needed.
- Require actual visible, moving video frames, not just a successful build, test exit code or diagnostic counters.
- Open Compare, select Original and an available Anime4K candidate, switch modes, recapture and close it during capture; verify both rendered frames and continued player playback.
- In PiP, move/resize where the desktop permits, restore the player window, and check that a window-manager close matches the close button without leaving hidden playback.
- Repeat on the intended X11 or Wayland session; Wayland compositor policy may prevent moving, resizing or keeping PiP on top.

- In Settings → Customization → General → System integration, check Save/Cancel for launch at sign-in and Open With, then check the desktop's application list/default-app settings; defaults require the user's choice.
- Undo any integrations after testing.
- In Settings → Processing and Components, verify unsupported choices are reported; on supported NVIDIA hardware with compatible fork libmpv, CUDA 13 and TensorRT 11, acquire the current official AnimeJaNai v0.22.2 API/ABI-9 runtime/model, select it explicitly and verify real processed frames on the same video.
- The current Windows DEV archive's runtime exposes ABI-9 host entrypoint markers (`aji_ingest`, `aji_infer_seq`, `aji_temporal_radius`, and peers); those are string markers in the binary, not a behaviour guarantee.
- The `mpv-fork` source entry still points to ABI-8 commit `63d127174d8f70336c82a22408929e2bc301edcb`; the Windows archive has separate source commit `b4f9f5bfed18d5ce610af28024986410dd9bdaae`, while corresponding-source/build evidence for the Linux runtime is not established.
- Archive identity does not establish hardware operation or inference.
- The old official v0.9.0 ABI-8 pair and separately patched private MSVC AJI ABI-8 build are historical, distinct builds, not substitutes.
- Manager temporal frame count, scene threshold, upscale enable/disable and temporal-fix controls are available in Processing; v0.22.2 source maps RIFE code 4262 to regular `rife_v4.26`, not heavy RIFE, which is not inference evidence.
- The separate ~80 MB Linux encoder archive is not needed for playback, is not automatically acquired/adopted, and has unresolved FFmpeg/redistribution requirements.
- The opt-in Windows `TensorRT for RTX` backend has no Linux asset or download path: a restored or imported RTX choice must fail by name (installation reports `TensorRT for RTX is available only on Windows.`, runtime preparation `TensorRT for RTX is not available on this platform.`) and must not clamp to TensorRT or DirectML. On Windows it is acquired only after explicit user action (standard dispatcher plus the five overlay files, ~96.25 MB of archives) and is absent from the base installer and distribution; it builds engines in-process without `trtexec`.
- The pinned classic component packs stay at TensorRT 11.3 (the Windows RTX overlay carries its own TRT-RTX 1.7 runtime); upstream's 11.4 build line is a source-build fact, not a statement of the deployed runtime.
- This AI check requires a writable executable-adjacent `native/` directory; use a user-owned portable/build deployment, not a read-only system binary directory.
- A skipped or unavailable hardware check is not a pass.
- For Benchmark asset/report requirements, see the [user guide](user-guide.md#benchmarking).
- These instructions are a test plan, not a claim that the current source has passed on Linux.

## Local API credentials and state

- The app can run without an API key.
- Never embed one in source or commit `.env`; use **Settings → Application → Access** or a private local `.env`.
- Settings keep the compatibility identity `Mikai` / `Mikai Wrapper`.
- Native/model data resolves to `MIKAI_NATIVE_ROOT`, else an executable-adjacent `native/` that accepts a write, else Qt's `AppLocalDataLocation/native` — so an AppImage or a root-owned prefix keeps its store in the user's data directory rather than failing.
- Logs and crash reports are written beside the executable when that location is writable; otherwise they use Qt's `AppLocalDataLocation`.
- Do not reuse or overwrite these user-state locations as build/test staging.

On start the application removes abandoned work from that store: `scratch/` entries and `*.previous` backups older than a day, and `aji-stage-*` trees and `*.partial` copies older than an hour. Installed components, imported models and configuration are untouched; the cleanup exists so an update does not need the folder deleted by hand.

## Further references

- [Quick start & catalogue](user-guide.md).
- [Player & Compare](player-guide.md).
- [Processing & benchmarks](processing-guide.md).
- [Settings & privacy](settings-guide.md).
- [Public API reference](https://api.mikai.me/public/docs).
- [Native bundle/build documentation](../native/README.md), [MSVC native build guide](../native/build-msvc.md), and [native component manifests](../native/manifest/).
- [Benchmark results](../bench_results.md), with explicit historical workload limitations.