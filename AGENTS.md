# Repository & Engineering Guidelines (`AGENTS.md`)

## 1. Architecture & Layering Invariants

`mikai` is a single-process C++20 / Qt 6.12 desktop client and `libmpv` OpenGL video player supporting **Windows x64 (MinGW-w64)** and **Linux x64 (GCC / Clang)**.

Strict architectural layering is enforced by `TestLayering` (`tests/cpp/app/TestLayering.cpp`):

1. **`src/backend/` (`mikai::backend`) — Pure Domain & Network Layer:**
   - Contains `Models.hpp`, `Errors.hpp`, `Releases.hpp`, `MikaiClient.hpp`, `Streams.hpp`, `Downloads.hpp`, `Runtime.hpp`, `Cancellation.hpp`, and `Diagnostics.hpp`.
   - **Hard Rule:** Must never `#include` any Qt GUI/Widgets headers (`QWidget`, `QImage`, `QPixmap`, `QColor`, etc.) and must never reference `src/app/`, `src/gui/`, or `src/player/`.
2. **`src/gui/` (`mikai::gui`) — Shared GUI Helpers:**
   - Lightweight Qt GUI utilities (`Clipboard.hpp`) shared across `src/app/` and `src/player/`.
3. **`src/player/` (`mplayer`) — Video Surface, Shader/AI Pipelines & Compare Workstation:**
   - `src/player/core/` — Dynamic `libmpv` loader (`MpvApi`), async transport (`MpvHandle`), diagnostics (`PlaybackDiagnostics`), and OpenGL render bridge (`MpvRenderContext`, `VideoSurface`, `VideoSurfaceAnimeJanai`, `VideoSurfaceNative`). `MpvApi`, `MpvHandle`, and `PlaybackDiagnostics` are GUI-free and compiled in `mikai_native_support` so the static library graph is strictly acyclic (`mikai_backend` → `mikai_native_support` → `mikai_player` → `mikai_app`).
   - `src/player/shaders/` — `Anime4K` shader chain builder (`Anime4K.{hpp,cpp}`) and `PresetShortcuts.{hpp,cpp}`.
   - `src/player/shell/` — Player UI chrome (`PlayerDialog`, `PlayerDialogProcessing`, `PipFloat`, `PlayerShell`, `PlayerPopups`, `ShellWidgets`, `ShellSliders`, `ShellTheme`).
   - `src/player/compare/` — Visual Compare Workstation (`CompareWindow`, `CompareCanvas`, `CompareGrid`, `ComparePane`, `CompareLayout`, `CompareRenderer`).
   - `src/player/native/` (`mplayer::native`) — Subdivided into:
     - `bundle/` (`NativeBundle`, `NativeManifest`, `NativePaths`, `NativePeImage`, `NativeRuntimeSelection`, `NativeCapabilities`, `NativePrefs`, `NativeDiagnostics`)
     - `components/` (`NativeComponentCatalog`, `NativeComponentStore`, `NativeComponentProvisioning`, `NativeComponentToolchain`, `NativeGpuProbe`, `AnimeJanaiConfig`, `AnimeJanaiRuntime`, `AnimeJanaiInstallation`)
     - `models/` (`NativeCatalog`, `NativeModelInventory`, `NativeModelInspect`, `NativeModelImport`, `NativeInspectedPlan`, `NativeProfiles`, `NativeRecipe`, `NativeRife`, `NativeSpatialAssets`, `NativeUserRecipes`)
     - `prepare/` (`NativeArtifactStore`, `NativeBuilder`, `NativeBuilderTools`, `NativeFrameFixture`, `NativeFrameReference`, `NativePreparationContext`, `NativePreparationPlan`, `NativePreparationReferences`, `NativePreparedPlan`, `NativeWorkLease`)
     - `status/` (`NativeFilterCommand`, `NativeMenu`, `NativePlaybackController`, `NativeStatus`, `NativeStatusConsumer`, `NativeTypes`)
4. **`src/app/` (`mikai::app`) — Application Pages, Settings, Support & Worker Dispatch:**
   - `src/app/support/` — `CrashLog`, `I18n`, `Labels`, `WindowRaisePolicy`.
   - `src/app/pages/` — `MainWindow`, `SearchPage`, `TitlePage`, `TitlePagePlayerSession`, `EpisodeGrid`.
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
