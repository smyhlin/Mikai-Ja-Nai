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
     - `components/` (`NativeComponentCatalog`, `NativeComponentStore`, `NativeComponentProvisioning`, `NativeComponentToolchain`, `NativeGpuProbe`, `AnimeJanaiRuntime`, `AnimeJanaiInstallation`)
     - `models/` (`NativeCatalog`, `NativeModelInventory`, `NativeModelInspect`, `NativeModelImport`, `NativeInspectedPlan`, `NativeProfiles`, `NativeRecipe`, `NativeRife`, `NativeSpatialAssets`, `NativeUserRecipes`)
     - `prepare/` (`NativeArtifactStore`, `NativeBuilder`, `NativeBuilderTools`, `NativeFrameFixture`, `NativeFrameReference`, `NativePreparationContext`, `NativePreparationPlan`, `NativePreparationReferences`, `NativePreparedPlan`, `NativeWorkLease`)
     - `status/` (`NativeFilterCommand`, `NativeMenu`, `NativePlaybackController`, `NativeStatus`, `NativeStatusConsumer`, `NativeTypes`)
4. **`src/app/` (`mikai::app`) — Application Pages, Settings, Support & Worker Dispatch:**
   - `src/app/support/` — `CrashLog`, `I18n`, `Labels`, `WindowRaisePolicy`.
   - `src/app/pages/` — `MainWindow`, `SearchPage`, `TitlePage`, `TitlePagePlayerSession`, `EpisodeGrid`.
   - `src/app/dialogs/` — `SettingsDialog` (`settings/` tabs: General, Presets, Processing, Components, Benchmark), `ComponentStoreTab` (`ComponentStoreTabAcquisition`), `NativeAssetsPanel` (`NativeAssetsPanelWorkflow`), `NativeRifeControls`, `CoverViewer`.
   - `src/app/workers/` — `Runner` (`src/app/workers/Runner.hpp`) manages 5 dedicated `QThreadPool` queues (`shared`, `stream`, `transfer`, `cover`, `probe`) and `Runner::GuiDispatch` (`runner.post()`).

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
cmake --build --preset app --parallel
cmake --build --preset tests --parallel
ctest --test-dir build/release --output-on-failure
```

*Note on test runtime:* Ensure `Qt6Test.dll` is present in `build/release/` or accessible via `PATH` when running standalone probe executables.

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

# Validate source and component manifest schemas, commit pins, and digests
./build/release/mikai_manifest_check --sources native/manifest/sources.json --components native/manifest/components.json
```

### Running Individual Test Suites

`mikai_tests` runs all 35 hermetic test suites by default, or a single named suite (with optional QTest flags in any position):

```bash
./build/release/mikai_tests TestNativeComponents
./build/release/mikai_tests -v2 TestI18n
./build/release/mikai_tests -v2 TestDownloads
```

### Internationalization (`translations/mikai_uk.ts`)

All user-facing strings use `tr()` or `QCoreApplication::translate()`. After adding or modifying UI strings, refresh and compile the Ukrainian translation catalog:

```bash
cmake --build build/release --target update_translations release_translations
./build/release/mikai_tests TestI18n
```
