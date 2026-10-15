# Mikai Desktop Client

A cross-platform C++20 / Qt 6.12 desktop client and integrated video player for the [Mikai public API](https://api.mikai.me/public/docs) (`https://api.mikai.me/public/v1`), supporting **Windows x64 (MinGW-w64)** and **Linux x64 (GCC / Clang)**.

## Key Features

- **Discovery, Search & Schedule:**
  - Home page with **Recently opened**, **Ongoing**, and **Popular** carousels, plus **Recent updates** and day-of-week **Release schedule** feeds.
  - Multi-filter search (`format`, `genres`, `status`, `season`, `year`, `sort`) with keyboard-accessible pagination and instant Ukrainian (`uk_UA`) / English (`en_US`) runtime localization.
  - Detailed Title page with studio/genre/rating metadata, related series navigation, per-release dub/sub tabs sorted by playable episode count, and asynchronous HLS stream extraction (`ASHDI`, `MoonAnime`, `Tortuga`).
- **Integrated `libmpv` OpenGL Player (`PlayerDialog`, `VideoSurface`, `PlayerShell`):**
  - Single-process OpenGL render surface (`QOpenGLWidget` + `mpv_render_context`) with dynamic `libmpv` symbol loading (`LoadLibraryExW` on Windows, `dlopen` on Linux).
  - Flicker-free continuous compositor pipeline (`QOpenGLWidget::NoPartialUpdate`) unconditionally blitting into the backing FBO on every `paintGL()` pass.
  - Auto-hiding HUD controls on both playback and idle paused state, timeline thumbnail/chapter scrubbing, audio & subtitle track popups with live probed quality badges (`1080p`, `720p`, etc.), episode drawer (`S`), Picture-in-Picture mode, and multi-level `Esc` navigation.
- **Real-Time Video Enhancement Pipelines:**
  - **Anime4K GLSL Shaders:** 6 composite modes (`A`, `B`, `C`, `A+A`, `B+B`, `C+A` in Fast and High Quality variants) plus individual Restore, Upscale, Denoise, Deblur, Darken, and Thin shaders.
  - **AnimeJaNai (`3.7.0` / Aji `v0.9.0` ABI-8):** Powered by the [mpv-AnimeJaNai](https://github.com/the-database/mpv-AnimeJaNai) project and the [animejanai-inference (AJI)](https://github.com/the-database/animejanai-inference) stack. Built-in adaptive profiles (`Quality`, `Balanced`, `Performance`), 5 pinned HD/SD spatial ONNX models, and 47 pinned RIFE frame-interpolation models (`v4.7`–`v4.26`).
  - **Prepared Native Bundle Track:** Out-of-process model inspection (`native_inspect`), TensorRT engine preparation (`mikai_native_prep`), numerical verification (`mikai_native_verify`), and bundle verification (`mikai_bundle`).
- **Workstation Compare Window (`CompareWindow`, `CompareCanvas`, `CompareGrid`, `CompareRenderer`):**
  - Side-by-side and split-screen visual inspection of **Original** vs **Anime4K** vs **AnimeJaNai** vs **Native** models on the exact paused video frame.
  - Five comparison modes: **Split H**, **Split V**, **Side-by-side** (2-pane canvas or 3–4 candidate synchronized grid), **Flicker**, and **Difference** (`1x`–`16x` gain).
  - Unified top workstation toolbar with Mode, Filter, Fit (`F`), live Zoom percentage pill, Swap A ⇄ B (`S`), Recapture (`R`), Always-on-Top Pin (`P`), inline Split/Flicker/Gain sliders + spinboxes, and live cursor `(X, Y) RGB + #HEX` pixel readout.
  - Full-width 50/50 bottom model selector bar with Steel Blue (`#3b82f6`, Side A) and Amber (`#f59e0b`, Side B) accents and automatic `2x` FBO rendering for `Anime4K_Upscale_*` shaders.

---

## Supported Platforms & Prerequisites

| Platform | Compiler & Generator | Qt Version | `libmpv` Runtime | GPU / Enhancement Support |
|---|---|---|---|---|
| **Windows x64** | MinGW-w64 GCC 16.2 (`MinGW Makefiles`) | Qt 6.12.x (`mingw_64` + `LinguistTools`) | Pinned `libmpv-2.dll` (`the-database/mpv-winbuild` `2026-10-07-d6d93599d5`) | Anime4K (OpenGL), AnimeJaNai TensorRT (CUDA 13 / NVML) & DirectML (D3D11VA) |
| **Linux x64** | GCC 12+ or Clang 15+ (`Unix Makefiles`) | Qt 6.x (`Widgets`, `Network`, `OpenGLWidgets`, `Concurrent`, `Test`, `LinguistTools`) | System `libmpv-dev` (`libmpv.so.2`) or local `MIKAI_DIST_MPV_ARCHIVE` | Anime4K (OpenGL), AnimeJaNai TensorRT (`-linux-x64` CUDA 13 / NVML `.so` packs) |

### Windows `libmpv` & Vulkan Loader Prerequisite
On Windows, CMake automatically downloads (or verifies via `MIKAI_DIST_MPV_ARCHIVE`) the pinned `mpv-dev-x86_64-20261007-git-d6d93599d5.7z` archive (`SHA-256: 077cb75fed47b185428f97224e1d798e2d4c2f4063fd8cda2662b74f2892327c`), compiles against its headers, and stages `libmpv-2.dll` beside `mikai.exe`. Because `libmpv-2.dll` imports `vulkan-1.dll` in its PE import table, `%SystemRoot%\System32\vulkan-1.dll` (installed by standard NVIDIA, AMD, and Intel graphics drivers) must be present on Windows.

### Linux Dependencies
On Debian/Ubuntu-based distributions, install the standard Qt 6, OpenGL, and `libmpv` development packages:
```bash
sudo apt-get install build-essential cmake qt6-base-dev qt6-tools-dev qt6-tools-dev-tools libgl1-mesa-dev libmpv-dev p7zip-full
```

---

## Build & Run

`CMakePresets.json` (schema v6, CMake >= 3.25) provides ready-to-use configure and build presets for both Windows (MinGW64) and Linux.

### Windows (MinGW64)

```bat
rem 1. Configure Release build
cmake --preset release

rem 2. Build the application (build/release/mikai.exe)
cmake --build --preset app --parallel

rem 3. Build and run the unit & contract test suite
cmake --build --preset tests --parallel
ctest --test-dir build/release --output-on-failure

rem 4. Assemble a self-contained portable distribution in dist-mikai/
cmake --build build/release --target dist
```

If your Qt 6 or MinGW-w64 installation lives outside the default preset paths (`C:/temp/qt6/6.12.0/mingw_64` and `C:/temp/w64devkit`), override `CMAKE_PREFIX_PATH`, `CMAKE_CXX_COMPILER`, and `CMAKE_MAKE_PROGRAM` on the command line.

### Linux (GCC / Clang)

```bash
# 1. Configure Release build
cmake --preset linux-release

# 2. Build the application (build/linux-release/mikai)
cmake --build --preset app-linux --parallel

# 3. Build and run the unit & contract test suite
cmake --build --preset tests-linux --parallel
ctest --test-dir build/linux-release --output-on-failure
```

---

## Runtime Component Store (`Settings -> Components`)

Large GPU-specific runtimes and neural network graphs are never bundled into the repository or `dist-mikai/`. Instead, **Settings → Components** (`ComponentStoreTab`) inspects the local GPU via NVML (`nvml.dll` on Windows, `libnvidia-ml.so.1` on Linux) and downloads only the selected packs into `<applicationDir>/native/components/` with strict SHA-256 verification:

| Component Group | Windows x64 Assets (`3.7.0` / `v0.9.0`) | Linux x64 Assets (`3.7.0` / `v0.9.0`) | Destination |
|---|---|---|---|
| **Aji v0.9.0 Dispatcher** | `aji-windows-x64.zip` (`aji.dll`, `aji_trt.dll`, `aji_dml.dll`) | `aji-linux-x64.tar.zst` (`libaji.so`, `libaji_trt.so`) | `<nativeRoot>/components/aji/v0.9.0/<trt\|dml>` |
| **TensorRT 11.3 Runtime** | `component-trt-runtime.7z` (`cudart64_13.dll`, `nvinfer_11.dll`, `nvinfer_plugin_11.dll`, `nvonnxparser_11.dll`, `trtexec.exe`) | `component-trt-runtime-linux-x64.7z` (`libcudart.so.13`, `libnvinfer.so.11`, `libnvinfer_plugin.so.11`, `libnvonnxparser.so.11`, `trtexec`) | `<nativeRoot>/components/tensorrt/3.7.0` |
| **TensorRT GPU Builder Resource** | `component-trt-sm{75,80,86,89,90,100,120}.7z` or `component-trt-ptx.7z` (`nvinfer_builder_resource_*_11.dll`) | `component-trt-sm{75,80,86,89,90,100,120}-linux-x64.7z` or `component-trt-ptx-linux-x64.7z` (`libnvinfer_builder_resource_*_11.so`) | `<nativeRoot>/components/tensorrt/3.7.0` |
| **DirectML Closure (Windows)** | `Microsoft.ML.OnnxRuntime.DirectML.1.24.4.nupkg` + `Microsoft.AI.DirectML.1.15.4.nupkg` | *Windows-only (DirectX 12)* | `<nativeRoot>/components/aji/v0.9.0/dml` |
| **AnimeJaNai Upscaling Models** | 5 pinned HD/SD ONNX spatial graphs (`6,637,479` bytes total) | 5 pinned HD/SD ONNX spatial graphs (`6,637,479` bytes total) | `<nativeRoot>/assets/` |
| **RIFE 3.7.0 Interpolation Pack** | `component-rife.7z` (47 ONNX graphs, `462,444,035` bytes) | `component-rife-linux-x64.7z` (47 ONNX graphs, `~441 MB`) | `<nativeRoot>/components/rife/3.7.0` |
| **7-Zip 26.03 Extractor Bootstrap** | `7zr.exe` + `7z2603-x64.exe` (when system `7z.exe` is absent) | `7z2603-linux-x64.tar.xz` (when system `7zz`/`7z` is absent) | `<nativeRoot>/tools/7zip-26.03` |

---

## Compare Window Keyboard Shortcuts & Controls

Open the Compare Window from the player toolbar (`Compare` button) while playing any video:

| Key / Action | Function |
|---|---|
| `1` – `5` | Switch comparison mode (`1` Split H, `2` Split V, `3` Side-by-side, `4` Flicker, `5` Difference) |
| `S` | Swap Side A and Side B candidate selections |
| `R` | Recapture the current video frame from the player |
| `F` | Reset zoom and pan to Fit-to-Window |
| `P` | Toggle Always-on-Top window pin (`Qt::WindowStaysOnTopHint` + Win32 `HWND_TOPMOST`) |
| `Space` | Hold to peek Side A (original or reference) across all modes |
| `+` / `-` / `0` | Zoom in, zoom out, or reset zoom to Fit |
| Mouse Wheel / Drag | Synchronized cursor-anchored zoom (up to `32x`) and direct left-drag pan across all panes |
| `Esc` | Close the Compare Window and release the player pause hold |

---

## Configuration & Data Locations

- **API Key (`.env` or Settings):** Optional `API_KEY=<token>` in `.env` (or configured in **Settings → General**) raises rate limits against `https://api.mikai.me/public/v1`.
- **Settings (`QSettings`):** Stored under organization `Mikai`, application `MikaiQt`.
- **Cache & Watch State:** `<StandardPaths::CacheLocation>/covers` for poster thumbnails, `watched.json` for episode progress, `recent.json` for recently opened titles, and `<applicationDir>/crash.log` for unhandled fault traces.

---

## Repository Layout

- `src/backend/` — Domain models (`Models.hpp`), API client (`MikaiClient`), HLS stream extractors (`Streams`), verified downloader (`Downloads`), runtime discovery (`Runtime`), and diagnostic scrubbing (`Diagnostics`).
- `src/gui/` — Shared GUI utilities (`Clipboard`).
- `src/app/` — Main window (`MainWindow`), pages (`SearchPage`, `TitlePage`, `EpisodeGrid`), dialogs (`SettingsDialog`, `ComponentStoreTab`, `NativeAssetsPanel`, `NativeRifeControls`, `CoverViewer`); Settings includes a searchable, grouped Benchmark matrix for local clips and models. Runs use off-screen mpv, show actual TensorRT engine builds with indeterminate progress and elapsed time, wait for the backend's configured-slot report before measuring, and keep build time out of FPS results. Also contains custom widgets, dark QSS theme (`Theme`, `Metrics`), and worker pool (`Runner`).
- `src/player/` — `libmpv` wrapper (`MpvApi`, `MpvRenderContext`, `VideoSurface`), player chrome (`PlayerShell`, `PlayerDialog`, `PlayerPopups`, `ShellWidgets`), `Anime4K` shader resolver, and `CompareWindow` (`CompareCanvas`, `CompareGrid`, `ComparePane`, `CompareLayout`, `CompareRenderer`).
- `src/player/native/` — Native bundle, component catalog/store/provisioning, GPU NVML probe, ONNX model inventory, preparation pipeline, and runtime status controller.
- `src/probes/` — Standalone GUI and headless verification probes (`compare_probe`, `shell_probe`, `dialog_probe`, `native_component_probe`, etc.).
- `native/` — Standalone MSVC + CUDA + TensorRT inference shim build (`native/CMakeLists.txt`), CLI tools (`mikai_bundle`, `mikai_manifest_check`, `native_inspect`), patches, and provenance manifests (`native/manifest/`).
- `tests/` — 35 hermetic Qt Test suites (`tests/cpp/`) and CMake contract tests (`tests/cmake/`, `tests/fixtures/`).
- `translations/` — Ukrainian translation catalog (`translations/mikai_uk.ts`).
- `docs.md` — Reference documentation for the Mikai Public API v1.

---

## Acknowledgements & Credits

Special thanks to:
- **[mpv-AnimeJaNai](https://github.com/the-database/mpv-AnimeJaNai)** by the-database for the neural anime upscaling and frame interpolation architecture, component catalog, and profiles.
- **[animejanai-inference (AJI)](https://github.com/the-database/animejanai-inference)** for the high-performance ABI-8 TensorRT and DirectML video filter inference stack.
- **[Anime4K](https://github.com/bloc97/Anime4K)** by bloc97 for the open-source real-time anime upscaling and restoration GLSL shaders.
- **[mpv](https://mpv.io)** and **[libmpv](https://github.com/mpv-player/mpv)** for the core video playback engine and OpenGL render context.
- **[Mikai.me](https://mikai.me)** for the public Ukrainian anime streaming catalog API.
