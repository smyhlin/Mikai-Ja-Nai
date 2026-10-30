# Repository & Engineering Guidelines (`AGENTS.md`)

## 1. Architecture & Layering Invariants

`mikai` is a single-process C++20 / Qt 6.12 desktop client and `libmpv` OpenGL video player supporting **Windows x64 (MinGW-w64)** and **Linux x64 (GCC / Clang)**.

Strict architectural layering is enforced by `TestLayering` (`tests/cpp/app/TestLayering.cpp`):

1. **`src/backend/` (`mikai::backend`) — Pure Domain & Network Layer:**
   - Contains `Models.hpp`, `Errors.hpp`, `Releases.hpp`, `MikaiClient.hpp`, `Streams.hpp`, `Downloads.hpp`, `Runtime.hpp`, `Cancellation.hpp`, and `Diagnostics.hpp`.
   - **Hard Rule:** Must never `#include` any Qt GUI/Widgets headers (`QWidget`, `QImage`, `QPixmap`, `QColor`, etc.) and must never reference `src/app/`, `src/gui/`, or `src/player/`.
2. **`src/gui/` (`mikai::gui`) — Shared GUI Helpers:**
   - Lightweight Qt GUI utilities shared across `src/app/` and `src/player/`: `Clipboard.hpp`, `ImageDecode`, `XdgDesktopIntegration`, `VideoDropTarget`, and `IdleInhibitor` (per-playing-surface session idle block through logind's `idle` lock, with the screen-saver interface as the fallback when logind is absent).
3. **`src/player/` (`mplayer`) — Video Surface, Shader/AI Pipelines & Compare Workstation:**
   - `src/player/core/` — Dynamic `libmpv` loader (`MpvApi`), async transport (`MpvHandle`), diagnostics (`PlaybackDiagnostics`), and OpenGL render bridge (`MpvRenderContext`, `VideoSurface`, `VideoSurfaceAnimeJanai`, `VideoSurfaceNative`). `MpvApi`, `MpvHandle`, and `PlaybackDiagnostics` are GUI-free and compiled in `mikai_native_support` so the static library graph is strictly acyclic (`mikai_backend` → `mikai_native_support` → `mikai_player` → `mikai_app`).
   - `src/player/shaders/` — `Anime4K` shader chain builder (`Anime4K.{hpp,cpp}`) and `PresetShortcuts.{hpp,cpp}`.
   - `src/player/shell/` — Player UI chrome (`PlayerDialog`, `PlayerDialogProcessing`, `PipFloat`, `PlayerShell`, `PlayerPopups`, `ShellWidgets`, `ShellSliders`, `ShellTheme`, `EpisodeNavButton`) and the media-information views (`MediaInformationDialog`, `MediaInformationPresentation`, `MediaInformationTree`, `MediaInformationOverview`). The player library must not include `src/app/` state: accent and corner radius arrive as pushed tokens (`shell::setAccent`, `shell::setCornerRadius` from `ShellTheme.hpp`), which the application seeds at startup and refreshes on an appearance change or save. `ShellTheme.cpp` holds the last pushed value. Custom-painted chrome enables antialiasing, `SmoothPixmapTransform` and device-pixel-ratio scaling; icons are positioned on integer pixels.
   - `src/player/compare/` — Visual Compare Workstation (`CompareWindow`, `CompareCanvas`, `CompareGrid`, `ComparePane`, `CompareLayout`, `CompareRenderer`).
   - `src/player/native/` (`mplayer::native`) — Subdivided into:
     - `bundle/` (`NativeBundle`, `NativeManifest`, `NativePaths`, `NativePeImage`, `NativeRuntimeSelection`, `NativeCapabilities`, `NativePrefs`, `NativeDiagnostics`)
     - `components/` (`NativeComponentCatalog`, `NativeComponentStore`, `NativeComponentProvisioning`, `NativeComponentToolchain`, `NativeGpuProbe`, `AnimeJanaiConfig`, `AnimeJanaiRuntime`, `AnimeJanaiInstallation`). A pack's declared `files` list is the store's contract: `storedPack()` checks existence and size, `verifyPack()` re-hashes. `trtexec` is not a member of the Linux `trt-runtime` pack — provisioning copies it once into the staged AJI runtime — and adding it back makes that second copy fail against `QIODevice::NewOnly`.
     - Store root resolution and cleanup live in `NativePaths`: `MIKAI_NATIVE_ROOT` → an executable-adjacent `native/` that accepts a real create → `AppLocalDataLocation/native`; trust is anchored at the store's parent. `pruneNativeStore()` runs at startup and removes only abandoned work (`scratch/` and `*.previous` older than a day, `aji-stage-*` and `*.partial` older than an hour), never an installed component, model or configuration.
     - `models/` (`NativeCatalog`, `NativeModelInventory`, `NativeModelInspect`, `NativeModelImport`, `NativeInspectedPlan`, `NativeProfiles`, `NativeRecipe`, `NativeRife`, `NativeSpatialAssets`, `NativeUserRecipes`)
     - `prepare/` (`NativeArtifactStore`, `NativeBuilder`, `NativeBuilderTools`, `NativeFrameFixture`, `NativeFrameReference`, `NativePreparationContext`, `NativePreparationPlan`, `NativePreparationReferences`, `NativePreparedPlan`, `NativeWorkLease`)
     - `status/` (`NativeFilterCommand`, `NativeMenu`, `NativePlaybackController`, `NativeStatus`, `NativeStatusConsumer`, `NativeTypes`)
4. **`src/app/` (`mikai::app`) — Application Pages, Settings, Support & Worker Dispatch:**
   - `src/app/support/` — `CrashLog`, `I18n`, `Labels`, `WindowRaisePolicy`.
   - `src/app/pages/` — `MainWindow`, `SearchPage`, `TitlePage`, `TitlePagePlayerSession`, `EpisodeGrid`.
   - `src/app/state/` — `AppearanceStore` (load/save, `currentAppearance()`, change notifier), `WatchHistory` (per-title `current`, `watched`, and `offsets`/`durations` keyed by episode; resume offsets below 30 s are not stored, 90 % of a known length marks the episode watched and drops its offset), `QualityCache`.
   - `src/app/dialogs/` — `SettingsDialog` (`settings/` tabs: General, Presets, Processing, Components, Benchmark), `AnimeJanaiManagerPanel`, `ComponentStoreTab` (`ComponentStoreTabAcquisition`), `NativeAssetsPanel` (`NativeAssetsPanelWorkflow`), `NativeRifeControls`, `CoverViewer`.
   - `src/app/workers/` — `Runner` (`src/app/workers/Runner.hpp`) manages 5 dedicated `QThreadPool` queues (`shared`, `stream`, `transfer`, `cover`, `probe`) and `Runner::GuiDispatch` (`runner.post()`).

---

### Standard AJI Configuration & Model Ownership

- `AnimeJanaiConfig` and `AnimeJanaiRuntime` belong to GUI-free `mikai_native_support`; `AnimeJanaiManagerPanel` belongs to `mikai_app`. The Standard AJI manager is not the Prepared Native recipe/verification workflow and needs no optional MSVC shim build.
- Manager settings use schema v3 at `<nativeRoot>/animejanai/animejanai_manager.conf`. Custom profile IDs are `builtin_slot:<1..10>`; custom spatial model IDs are `spatial:custom:<id>`. Existing player, Compare and Benchmark consumers share `animeJanaiPresets()`.
- Imported graphs and provenance are app-owned under `animejanai/models/custom/`; borrowed external graphs are pinned in `animejanai/models/external-models.json`. Never modify/delete the borrowed graph or require a user-made sidecar beside it.
- Enumeration reads bounded metadata, not whole-model hashes. Run import/registration/hash work on a Runner worker, honor cancellation, and validate the recorded digest before publishing SHA-named staged graphs/configuration. A missing/changed reference or missing RIFE ensemble is an explicit error, not permission to substitute another graph.
- Profile/config edits are deferred until Save; model import, registration and removal are immediate. Commit the current chain's editor values before changing slot/chain or retranslating. Failed config import must leave current edits intact.
- ONNX container validation is not tensor/operator inspection or inference proof. Architecture/scale fields are filename hints. TensorRT Static/Dynamic helpers emit the input name `input`; preserve the distinction between shape controls, raw flags and actual backend compatibility.
- **Fork loader contract (Unix):** the fork's `aji_trt` backend resolves `libnvinfer*.so` / `libcudart*.so` by bare soname when it creates its inference context, and the store is not on the process loader path. `prepareAnimeJanaiPlan` records the closure folder in `AnimeJanaiPlan::backendLibDirs` and `VideoSurfaceAnimeJanai` preloads every `.so` there with `RTLD_NOW | RTLD_GLOBAL` immediately before `vf add`. Adding an AJI stage that skips that preload fails only as the opaque `Failed to create inference context`.
- **Fork shell-safety contract (Unix):** the fork runs its TensorRT engine build through `/bin/sh -c` as `LD_LIBRARY_PATH=<trtexec-libdir> "<trtexec>" …` with that prefix unquoted. `trtexec-libdir` must therefore never contain a space, quote or tab: pass `detail::shellSafeLibDir(installation.root)`, never the installation root directly. The store lives under `…/Mikai Wrapper/`, so the raw path splits the word and the build dies with `sh: line 1: Wrapper/…: No such file or directory` (exit 127) before TensorRT starts. `animejanai/models/spatial/*.engine.build.log` under the store is the authoritative diagnostic for a failed build; failure modes and reproduction commands are in [`docs/animejanai.md`](docs/animejanai.md).
- All model/artifact writes remain subject to `NativePaths` ownership and ancestor checks. Volume roots must be recognized as ancestors without skipping filesystem-root or reparse-point checks.

---

## 2. Cross-Platform Portability Rules (Windows MinGW64 + Linux GCC/Clang)

All application, player, native-support, and test targets must compile cleanly on both **Windows (MinGW-w64)** and **Linux (GCC/Clang)** with `-Wall -Wextra` and `QT_DISABLE_DEPRECATED_UP_TO=0x060C00`:

- **Header Case Sensitivity:** Always use lowercase `#include <windows.h>` (never `#include <Windows.h>`), guarded by `#ifdef _WIN32` or `#ifdef Q_OS_WIN`.
- **Dynamic Library Loading:**
  - `MpvApi` (`src/player/core/MpvApi.cpp`): Uses `AddDllDirectory` + `LoadLibraryExW` + `GetProcAddress` on Windows and `dlopen(RTLD_NOW | RTLD_LOCAL)` + `dlsym` on Linux.
  - `MpvRenderContext` (`src/player/core/MpvRenderContext.cpp`): Uses `wglGetProcAddress` + `opengl32.dll` on Windows and `QOpenGLContext::currentContext()->getProcAddress` + `libGL.so.1` (`dlopen`/`dlsym`) on Linux.
  - `NativeGpuProbe` (`src/player/native/components/NativeGpuProbe.cpp`): Loads `nvml.dll` from `System32` on Windows and `libnvidia-ml.so.1` / `libnvidia-ml.so` on Linux.
  - `AnimeJanaiInstallation` & `NativeBundle`: Validates x64 PE headers (`MZ` / `PE\0\0` / `0x8664`) for `.dll`/`.exe` binaries and 64-bit x86_64 ELF headers (`\x7fELF\x02\x01` / `EM_X86_64`) for `.so` / `trtexec` binaries.
- **Locale-Invariant Formatting for `libmpv`:**
  - `libmpv` requires `LC_NUMERIC` to be `"C"`. Never pass `std::to_string(double)` (which formats `,` under `uk_UA` locale) to `mpv` options or commands; always use `QString::number(value, 'f', 6).toStdString()`.

---

## 3. Threading, Cancellation & Lifetime Rules

- **Never block the GUI thread** with network I/O, archive extraction, SHA-256 hashing, or model preparation.
- **Never touch `QWidget` or construct/copy `QPointer<QWidget>` on a worker thread.** Capture `Runner::GuiDispatch post = runner_->post();` and an `std::shared_ptr<std::atomic<bool>> alive_` flag on the GUI thread before submitting work, and resolve widget pointers only inside the `post(...)` callback on the GUI thread.
- **Honor `WorkCancellationScope`:** Blocking network helpers (`MikaiClient`, `Streams::runBlocking`, `Downloads::fetchUrlBytes`, `Downloads::downloadToFile`) poll `WorkCancellationScope::cancellationRequested()` every 200 ms so `Runner::cancel()` and `Runner::shutdownAll()` unwind immediately.
- **Video Surface Painting (`src/player/core/VideoSurface.cpp`):** Must use `setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);`. `paintGL()` unconditionally invokes `render_->render(...)` to populate the backing FBO on every pass, eliminating black-frame strobing during UI repaints or focus changes. The auto-hide timer is armed on pause as well as playback.
- **Window Raise Policy & Menu Tracking (`src/app/support/WindowRaisePolicy.cpp`):** `WindowRaisePolicy::isExempt()` must exempt `Qt::Popup` and `Qt::ToolTip`. Win32 foreground activation must never be requested on popup menus, which would drop Windows mouse capture (`ReleaseCapture()`) and freeze menu interaction.
- **Submenu Action Forwarding & Dismissal (`src/player/shell/PlayerPopups.cpp`, `src/player/compare/CompareWindow.cpp`):** `forwardChoiceLeaf` forwards nested `QAction::triggered` directly to the root menu, while receivers guard re-entrancy with `if (id.isEmpty() || id == current_) return;`. `dismissCandidateMenus()` closes all `QMenu` instances across the window tree and active popups.

---

## 4. Build, Test & Verification Commands

### Windows (MinGW64)

```bat
cmake --preset release
cmake --build --preset app --parallel 4
cmake --build --preset tests --parallel 4
ctest --test-dir build/release --output-on-failure
```

The four-job examples bound peak compiler memory. Unbounded parallel test builds can exhaust compiler memory; increase the job count only when the machine has headroom.

CTest uses `mikai_test_launcher` to copy the test executable into an owned disposable directory with the required shaders, mpv library and Qt runtime/plugin paths. Merely changing CWD, application name or QSettings does **not** isolate `<applicationDir>/native`. Never run mutable probes/tests next to a populated native store or deploy test executables into the user's distribution to obtain DLLs.

### Linux (GCC / Clang)

```bash
cmake --preset linux-release
cmake --build --preset app-linux --parallel
cmake --build --preset tests-linux --parallel
ctest --test-dir build/linux-release --output-on-failure
```

A build names itself, which is how a replaced AppImage is told from an older running process: `./build/linux-release/mikai --version` prints `mikai <version> (<short-rev>[-dirty], built <UTC stamp>)` without a display, the same line is logged at startup, and `applicationVersion` carries it. A cross build that cannot read the checkout passes `-DMIKAI_BUILD_REVISION=<short-rev>`. A MinGW cross build with GCC 13 needs `-DMIKAI_DISABLE_IPO=ON`; LTO refuses this tree's duplicated weak destructor thunks, which a non-LTO link merges, and the default stays LTO elsewhere.

### Linux runtime and distribution packages

A Linux artifact never borrows the host's mpv. `MIKAI_LINUX_BUNDLE_MPV=ON` installs the pinned runtime and its private closure into `MIKAI_LINUX_MPV_INSTALL_SUBDIR` — `bin/mpv` by default, `lib/mikai/mpv` where `/usr/bin/mpv` is the distribution's file — and the compile-time module path is derived from that same directory, so the install rule and the lookup cannot drift.

The runtime must be the TLS rebuild (`packaging/mpv/build-tls-archive.sh` → `mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst`): the published archive was configured without an FFmpeg TLS backend, so it cannot open any https URL. Configure checks `MIKAI_LINUX_MPV_TLS` from libavformat's dynamic requirements, not from the "recompile FFmpeg with openssl" text that every build contains. `libvulkan.so.1` stays in the bundle — the runtime links it, and the loader is the 1.4.363 SDK the bundle was built against.

`MpvHandle` names the host CA bundle as `tls-ca-file` unless the caller set one; the bundled OpenSSL was compiled against the build distribution's paths, so Arch and Fedora would fail certificate verification without it. `MIKAI_TLS_CA_FILE` overrides the search.

Windows packaging cannot use `windeployqt.exe` (it needs Windows and crashes under wine), so the payload is assembled by name: `packaging/windows/assemble-payload.py` copies the executable, the Qt modules its import table asks for, a fixed plugin list, the MinGW runtime and the pinned `libmpv-2.dll`, then generates the Inno Setup `Source:` list from what it copied. Never replace it with a recursive glob or a deployed tree.

Distribution packages: `packaging/linux/arch-package.sh` exports `HEAD` and builds with `makepkg` (runtime archive as a `source`, `qt6-base`/`libglvnd` instead of `mpv`, runtime at `/usr/lib/mikai/mpv`). `packaging/linux/deb-package.sh` builds inside Debian 13 with the Qt 6.12 SDK mounted at `/qt` and the runtime at `/mpv/runtime.tar.zst`; the package installs under `/usr`, ships ICU 73 beside Qt because the SDK links it, and derives `Depends` from what each shipped object can resolve through its own `$ORIGIN`/RUNPATH — a union of every shipped directory would drop a real dependency such as `libdbus-1-3`, whose copy exists only inside the mpv closure. The Windows dev archive `mpv-dev-x86_64-*.7z` supplies `libmpv-2.dll`; the `mpv-x86_64-*.7z` CLI archive does not contain it.

### Out-of-Band CLI Tools & Manifest Verification

```bash
# Build manifest checker and bundle inspector
cmake --build build/release --target mikai_manifest_check mikai_bundle --parallel

# Without source roots this inspects metadata, not source-file integrity.
./build/release/mikai_manifest_check --sources native/manifest/sources.json --components native/manifest/components.json
```

The current rights manifest intentionally contains redistribution-blocked components, so this invocation returns **3**, not a clean approval. Without explicit `--source-root id=directory` arguments it also cannot recompute source-file digests. See the [manifest exit-code contract](native/README.md#manifest-checker-exit-codes); do not suppress these refusals or describe format-checked observation hashes as verified files.

### Running Individual Test Suites

`mikai_tests` runs its registered hermetic suites by default. The launcher accepts **one suite name per invocation**, followed by optional QTest flags. The configured CTest command is the safe default; use the launcher for a focused run rather than a test executable beside an existing native store.

Windows PowerShell example from the repository root, using the preset's Qt/MinGW paths:

```powershell
$qt = "C:/temp/qt6/6.12.0/mingw_64"
$env:PATH = "$qt/bin;C:/temp/w64dev/w64devkit/bin;$env:PATH"
.\build\release\mikai_test_launcher.exe `
    (Resolve-Path build/release/mikai_tests.exe).Path `
    (Resolve-Path shaders).Path `
    (Resolve-Path build/release/mpv/libmpv-2.dll).Path `
    "$qt/bin" "$qt/plugins" "$PWD/build/release/_test-runs" `
    TestAnimeJanaiRuntime -v2
```

Replace the final suite with `TestLayering`, `TestNativeManifest`, `TestI18n`, or another registered name as needed. Adjust the Qt/MinGW paths if the build uses a different installation. The launcher stages and removes only its own scratch tree; the test refuses a populated or unsafe executable-adjacent `native/`.

### AJI Manager UI Smoke

Build `native_settings_probe` and follow the [Windows manager smoke command](tests/fixtures/README.md#windows-manager-smoke). It exercises real Settings actions, config/profile persistence, player/Compare menus, Benchmark catalogue entries, Standard/Native visibility, and compact/wide English/Ukrainian layouts. Its JSON/screenshots are functional evidence, not native inference or throughput proof. The generated Identity ONNX and Windows test dispatcher are described in [`tests/fixtures/README.md`](tests/fixtures/README.md).

Do not launch new TensorRT builds or RIFE benchmarks merely to verify manager documentation. Native inference and performance require separate, explicitly recorded model/backend/GPU runs; see [`bench_results.md`](bench_results.md).

### Internationalization (`translations/mikai_uk.ts`)

All user-facing strings use `tr()` or `QCoreApplication::translate()`. After adding or modifying UI strings, refresh and compile the Ukrainian translation catalog:

```bash
cmake --build build/release --target update_translations release_translations
ctest --test-dir build/release --output-on-failure
```
