# Mikai Desktop Client

A cross-platform C++20 / Qt 6.12 desktop client and integrated video player for the [Mikai public API](https://api.mikai.me/public/docs) (`https://api.mikai.me/public/v1`), supporting **Windows x64 (MinGW-w64)** and **Linux x64 (GCC / Clang)**.

## Key Features

- **Discovery, Search & Schedule:**
  - **Home navigation:** **Home** (**На головну**) is available in the catalogue/team header and beside **Back** on title pages. Home skips related-title history and returns directly to discovery. Repeated `Esc` closes the active menu/dialog or details sheet first, then walks title history and leaves search/team results for discovery; further presses keep the main window open. Late team responses cannot reopen a page after returning home.
  - Home page with **Recently opened**, **Ongoing**, and **Popular** carousels, plus **Recent updates** and day-of-week **Release schedule** feeds.
  - **Catalogue filters:** genre/tag inclusion and exclusion with all/any matching, specific years or a year range, season, format, status, studio, translation team, and sorting. **Filters** groups those choices into compact scrolling lists, and each section has its own search field that narrows the genres, tags or release options by either the shown caption or the API value; **Teams…** uses the same compact rows, with load-more separated from browse and catalogue-filter actions. Both use live dictionaries; selections survive pagination and language changes. Text search uses the API's relevance ordering rather than the selected sort.
  - Instant Ukrainian (`uk_UA`) / English (`en_US`) runtime localization. The interface-language selector previews across already-open pages, the player, and Compare windows; **Save** persists the choice, while **Cancel**, `Esc`, or closing Settings restores the language the dialog opened with. Switching does not restart the application, refetch API data, rebuild pages, or interrupt playback.
  - Redesigned **Title page**: a poster-led hero (320×480, 280×420 when compact) with the studio caption, status/season/type facts, Mikai and MAL ratings and the synopsis, all inside one centred 1180 px column that fills the viewport down to compact widths. The same single scroll carries the related/recommended carousels, the release selector and the per-release episode grids; a dismissible **Details** sheet (action, scrim or `Esc`) shows formatted metadata without leaving the page, and **Watch** opens the app's own `PlayerDialog` on the selected — or first playable — provider episode. Dub/sub releases stay sorted by playable episode count, with asynchronous HLS stream extraction (`ASHDI`, `MoonAnime`, `Tortuga`).
  - **Episode metadata:** manga canon, anime canon, filler and mixed badges remain separate from current/watched marks. Explicit episode classifications take precedence over title ranges; fractional episode identifiers remain distinct. Aired, localized and planned totals are shown separately (`0` planned means unknown), and the next scheduled broadcast is not treated as a playable or resume episode.
  - **Licensing and poster attribution:** a hard-licensed title shows the licensing reason and supplied legal information instead of an empty-player message. A license object alone does not block playback. The selected usable Ukrainian poster keeps its author/team credit; credit is hidden when the displayed artwork falls back to a different image.
  - **Translation teams:** **Teams…** offers searchable, paginated teams, their profile details and their own anime catalogue. Team names on releases and poster credits open the same browsing view. Team links show service names such as **Telegram**, **YouTube**, **Patreon** and **Monobase** instead of raw addresses. Click a link, or focus it with `Tab` and press `Enter`, to open it in the browser; displaying the profile never opens links automatically. Descriptions preserve their line breaks and display supplied markup as plain text.
  - **Episode watch history:** the grid outlines the last successfully loaded episode, underlines watched episodes, and dims unavailable slots when the API supplies a known episode total. **Mark as watched** / **Watched** toggles individual marks. The episode right-click menu provides **Watch all previous**, **Unwatch all previous**, and an explicit per-episode **Unwatch**. Previous-episode actions follow the selected provider's catalogue order across pages, including known earlier unavailable slots, and exclude the clicked episode and all later episodes. History-only actions do not extract stream URLs or change current playback. Normal mpv EOF also marks an episode watched; clicks, failed loads, early closes, episode/track replacements and PiP handoffs do not. Current and watched identifiers are saved locally in QSettings (`title/watchHistory`), per title and shared across dubs/providers, and survive reopening, pagination and language changes. This stores episode state, not a playback timestamp.
- **Integrated `libmpv` OpenGL Player (`PlayerDialog`, `VideoSurface`, `PlayerShell`):**
  - Single-process OpenGL render surface (`QOpenGLWidget` + `mpv_render_context`) with dynamic `libmpv` symbol loading (`LoadLibraryExW` on Windows, `dlopen` on Linux).
  - Flicker-free continuous compositor pipeline (`QOpenGLWidget::NoPartialUpdate`) unconditionally blitting into the backing FBO on every `paintGL()` pass.
  - Auto-hiding HUD controls on both playback and idle paused state, timeline thumbnail/chapter scrubbing, audio & subtitle track popups with live probed quality badges (`1080p`, `720p`, etc.), episode drawer (`S`), Picture-in-Picture mode, and multi-level `Esc` navigation.
- **Real-Time Video Enhancement Pipelines:**
  - **Anime4K GLSL Shaders:** 6 composite modes (`A`, `B`, `C`, `A+A`, `B+B`, `C+A` in Fast and High Quality variants) plus individual Restore, Upscale, Denoise, Deblur, Darken, and Thin shaders.
  - **AnimeJaNai (`3.7.0` / Aji `v0.9.0` ABI-8):** Powered by the [mpv-AnimeJaNai](https://github.com/the-database/mpv-AnimeJaNai) project and the [animejanai-inference (AJI)](https://github.com/the-database/animejanai-inference) stack. Built-in adaptive profiles (`Quality`, `Balanced`, `Performance`), 5 pinned HD/SD spatial ONNX models, and 47 pinned RIFE frame-interpolation models (`v4.7`–`v4.26`).
  - **[AnimeJaNai Model & Profile Manager](#animejanai-custom-models--profiles):** Import or register custom ONNX models, choose Standard/Sharp built-in variants, tune TensorRT compilation, and edit ten multi-chain profile slots with optional RIFE.
  - **Prepared Native Bundle Track:** Out-of-process model inspection (`native_inspect`), TensorRT engine preparation (`mikai_native_prep`), numerical verification (`mikai_native_verify`), and bundle verification (`mikai_bundle`).
- **Workstation Compare Window (`CompareWindow`, `CompareCanvas`, `CompareGrid`, `CompareRenderer`):**
  - Side-by-side and split-screen visual inspection of **Original** vs **Anime4K** vs **AnimeJaNai** vs **Native** models on the exact paused video frame.
  - Five comparison modes: **Split H**, **Split V**, **Side-by-side** (2-pane canvas or 3–4 candidate synchronized grid), **Flicker**, and **Difference** (`1x`–`16x` gain).
  - Unified top workstation toolbar with Mode, Filter, Fit (`F`), live Zoom percentage pill, Swap A ⇄ B (`S`), Recapture (`R`), Always-on-Top Pin (`P`), and an accessible fullscreen toggle; inline Split/Flicker/Gain sliders + spinboxes and live cursor `(X, Y) RGB + #HEX` pixel readout remain available in fullscreen.
  - Full-width 50/50 bottom model selector bar with Steel Blue (`#3b82f6`, Side A) and Amber (`#f59e0b`, Side B) accents and automatic `2x` FBO rendering for `Anime4K_Upscale_*` shaders.

---

## Supported Platforms & Prerequisites

| Platform | Compiler & Generator | Qt Version | `libmpv` Runtime | GPU / Enhancement Support |
|---|---|---|---|---|
| **Windows x64** | MinGW-w64 GCC 16.2 (`MinGW Makefiles`) | Qt 6.12.x (`mingw_64` + `LinguistTools`) | Pinned `libmpv-2.dll` (`the-database/mpv-winbuild` `2026-10-07-d6d93599d5`) | Anime4K (OpenGL), AnimeJaNai TensorRT (CUDA 13 / NVML) & DirectML (D3D11VA) |
| **Linux x64** | GCC 12+ or Clang 15+ (`Unix Makefiles`) | Qt 6.12+ (`Core`, `Gui`, `Widgets`, `OpenGL`, `OpenGLWidgets`, `Network`, `Test`, `LinguistTools`) | System `libmpv-dev` (`libmpv.so.2`) or local `MIKAI_DIST_MPV_ARCHIVE` | Anime4K (OpenGL), AnimeJaNai TensorRT (`-linux-x64` CUDA 13 / NVML `.so` packs) |

### Windows `libmpv` & Vulkan Loader Prerequisite
On Windows, CMake automatically downloads (or verifies via `MIKAI_DIST_MPV_ARCHIVE`) the pinned `mpv-dev-x86_64-20261007-git-d6d93599d5.7z` archive (`SHA-256: 077cb75fed47b185428f97224e1d798e2d4c2f4063fd8cda2662b74f2892327c`), compiles against its headers, and stages `mpv/libmpv-2.dll` under the application build directory. Because `libmpv-2.dll` imports `vulkan-1.dll` in its PE import table, `%SystemRoot%\System32\vulkan-1.dll` (installed by standard NVIDIA, AMD, and Intel graphics drivers) must be present on Windows.

### Linux Dependencies
On Debian/Ubuntu-based distributions, the following are the required package families. The selected Qt installation must provide **6.12 or newer**; a distribution's older Qt 6 packages alone do not satisfy CMake:
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
cmake --build --preset app --parallel 4

rem 3. Build and run the unit & contract test suite
cmake --build --preset tests --parallel 4
ctest --test-dir build/release --output-on-failure

rem 4. Assemble a self-contained portable distribution in dist-mikai/
cmake --build build/release --target dist
```

If your Qt 6 or MinGW-w64 installation lives outside the default preset paths (`C:/temp/qt6/6.12.0/mingw_64` and `C:/temp/w64dev/w64devkit`), override `CMAKE_PREFIX_PATH`, `CMAKE_CXX_COMPILER`, and `CMAKE_MAKE_PROGRAM` on the command line. The four-job examples limit peak compiler memory; raise the parallelism only if the machine has sufficient headroom.

CTest stages the Qt test executable in an owned disposable directory. Do not run tests beside an existing native store or copy them into a populated `dist-mikai/` just to find DLLs. See [focused-suite and runtime setup](AGENTS.md#running-individual-test-suites) for a safe launcher invocation.

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

## Public API Transport

Requests identify the running application and version (`Mikai-Wrapper/0.1.0` for the current release), rather than impersonating a browser. Provider stream extraction retains its separate transport behavior.

The client keeps a bounded, in-memory JSON cache keyed by the full request URL and credential generation. Fresh `Cache-Control` entries avoid requests; stale entries use `ETag` / `If-None-Match`, and a `304` reuses the cached body. `no-store` responses are not retained, errors are not cached, and `/me`, `/health` and `/anime/random` bypass this cache. A `304` still consumes an API request; existing rate-limit headers, `Retry-After`, cancellation and 429 handling remain authoritative. API keys remain local and are never bundled into application code.

**Recent updates** refreshes through `/updates?since=…` and follows every reported page before publishing the new watermark. A one-second overlap catches timestamp-boundary arrivals; duplicate events are merged while distinct providers, teams, track kinds and fractional episodes remain separate. Metadata for an existing event can refresh without adding a duplicate row. A failed or cancelled refresh leaves the last published feed intact.

Data source: [Mikai](https://mikai.me). The [public API documentation](https://api.mikai.me/public/docs) defines the read-only contract; local watch history is not synchronized to the service.

---

## Player Rendering Contract

The player uses **Qt Widgets + desktop OpenGL**, not Qt Quick's RHI. Rendering stays on the GUI thread; no QML backend override or shared-context render thread is required.

- `VideoSurface` enables libmpv advanced control. Native update callbacks queue a coalesced GUI-thread notification; render updates and skipped frames run with the surface's OpenGL context current.
- Each `paintGL()` redraws the video into `defaultFramebufferObject()` at Qt-rounded physical-pixel dimensions, including paused HUD-only repaints. `NoPartialUpdate` makes rendering only on newly decoded frames incorrect.
- Render calls pass an **`int` zero** for `MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME`; the interactive core also uses `video-timing-offset=0`. libmpv never sleeps the GUI thread waiting for a target presentation time.
- TensorRT transitions retain one clean GPU video snapshot, not a copy on every normal frame. The held picture survives UI repaints and is released only after an actual new output frame, excluding mpv redraw/repeat requests. Build feedback is indeterminate, includes elapsed time, and can replay an already-active build when its UI owner changes.
- The **+85** control beside Play seeks the active player forward 85 seconds, clamps to known media duration, and preserves pause intent.
- PiP controls remain visible while the pointer is on the float and fade after it leaves. The existing hide timer checks the actual cursor boundary so a missed layered-window leave event cannot hide controls mid-hover or leave them permanently visible.
- `frameSwapped()` reports a swap only when the surface rendered an unacknowledged frame. Hidden, minimized, and fully covered surfaces consume updates with `SKIP_RENDERING` outside `paintGL()` and acknowledge the discarded frame. Decoding and native inference can still consume GPU resources.
- On-video controls paint through `QPainter(this)` in the surface's GL pass. Existing widgets retain input, focus, accessibility, and layout roles; popovers and off-video chrome remain normal widgets. The native block restores Qt's FBO, VAO, and pixel-unpack state before painter drawing resumes.
- Renderer cleanup runs both on `aboutToBeDestroyed` and during destruction with the correct context current. A recreated context reloads the current source at its observed position and pause state. GUI playback commands are asynchronous.
- Frame exports and compare inputs use `grabVideoFramebuffer()` to exclude chrome; ordinary `grabFramebuffer()` includes the HUD.

`hwdec=auto-safe` remains enabled for interactive playback. It requests a safe hardware path; it is not a universal zero-copy guarantee. See the [Qt OpenGL widget contract](https://doc.qt.io/qt-6/qopenglwidget.html) and [libmpv render API](https://github.com/mpv-player/mpv/blob/master/include/mpv/render.h).

### Rendering Regression Probes

Build the opt-in `mpv_surface_smoke` and `native_hud_probe` targets, then place their executables beside the deployed application runtime. In PowerShell:

```powershell
$env:QT_QPA_PLATFORM = "windows"
$env:QT_OPENGL = "desktop"
.\mpv_surface_smoke.exe --render-lifecycle C:\media\moving-video.mp4 --dpr 1.0 --out lifecycle100
.\mpv_surface_smoke.exe --render-lifecycle C:\media\moving-video.mp4 --dpr 1.5 --out lifecycle150
.\native_hud_probe.exe --framebuffer-capture
```

Use a moving, non-black local clip. The lifecycle probe checks frame progression, pause/repaint stability, HUD-free exports, rapid pause toggles, hidden/covered recovery, DPR resizing, context recreation, teardown, and recovery after a real source error. It saves framebuffer evidence. The transition-hold check requires observable motion, proves that the picture freezes while the media clock advances, and reports an explicit inconclusive result if motion is insufficient. A companion `.srt` exercises subtitle texture uploads.

`--dpr` sets Qt's global scale multiplier, not an absolute monitor ratio. Check the reported device-pixel ratio; on a native 2× monitor, `--dpr 0.75` exercises a fractional 1.5× framebuffer.

The HUD probe uses synthetic status snapshots to check **presentation, not inference**. Its explicit framebuffer mode verifies GPU-painted text without claiming desktop presentation. Run it without that flag on an active desktop to check the presented screen. Neither offscreen benchmark fps nor framebuffer captures establish display-vsync smoothness.

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

Each downloadable item shows its own progress bar and status on its card, so parallel downloads remain independent. The status names the item and reports bytes when the transfer provides a known total; extraction, installation, and verification are shown as separate stages. A download reaching 100% does not mean installation is complete: a card reports **Installed** only after the selected component has been installed and verified. The main window shows a shared activity hint with **Components** and **Cancel** actions while acquisition runs, but does not combine items into a global percentage. Closing Settings does not cancel work; reopening Settings → Components resumes viewing the same in-flight progress. Use **Cancel** to request cancellation; failures and cancellation are reported on the affected item.

---

## AnimeJaNai Custom Models & Profiles

In **Settings → Processing**, select the **Standard** runtime to use the AnimeJaNai model and profile manager. Selecting **Native** shows the separate prepared-bundle workflow instead; it does not convert AnimeJaNai profiles into private Native recipes.

### Custom ONNX Models

- Use **Import .onnx Model…** to select self-contained spatial graphs (up to 2 GiB each). The manager checks the ONNX protobuf structure, copies each graph atomically into `<nativeRoot>/animejanai/models/custom/`, and records its size and SHA-256 digest.
- Imported models appear in the player's **ANIMEJANAI → Spatial models** menu, the Compare candidate menu, the Benchmark **Spatial** group, and profile-chain model choices.
- An optional external models directory can be used without copying its graphs. Select an unregistered external candidate and use **Import .onnx Model…** to validate it and record its digest in the app-owned registry; the external directory is not modified. After replacing a graph, remove its registration and register it again deliberately.
- A graph pasted directly into the app-owned custom folder also appears as an unregistered candidate, with its validation reason shown. It is not an executable model or a usable preset until **Import .onnx Model…** registers it in place by checking the graph and publishing its integrity record; registration never renames, moves, or rewrites the pasted file.
- Model import, external registration, and model hashing run off the GUI thread. Menu enumeration reads metadata rather than hashing every model. Preparation verifies the recorded digest before publishing a staged graph.
- Removing an imported model removes its app-owned copy and matching staged artifacts. Removing an external registration never copies, modifies, or deletes the borrowed external graph; the file returns to the unregistered candidate list. Unregistered candidates cannot be removed until they are registered.
- The manager watches the app-owned custom folder and the configured external folder for direct-child model additions, renames, deletions, and replacements, including atomic replacements and in-place edits that preserve size and modification time. Registered pin paths are observed as well. Open model tables and catalogue lists update from re-enumeration after a short coalescing delay; observation is metadata-only and never runs inference, TensorRT compilation, or a benchmark. Nested subfolders and private Prepared Native bundle folders are not scanned.

A model marked ready in the inventory has local metadata; it has not necessarily run on the GPU. Removing a model does not repair profiles that reference it. Select another graph or remove the affected step before using those profiles. Re-importing a replacement requires removing the old identifier first; modifying a pinned file in place is refused during preparation rather than silently accepted.

Filename-derived architecture and scale values are hints, not ONNX tensor inspection. Spatial models need an AJI-compatible 4D RGB tensor contract (`1×3×H×W`) and operators supported by the selected backend. The current TensorRT path binds FP16 buffers directly, so its input/output tensors must remain FP16; accepting a file at import does not establish compatibility with another boundary dtype. Import validation is not proof that an arbitrary RealESRGAN, Compact, SPAN, DAT, or SwinIR graph will execute. External-weight ONNX packages are not imported as multi-file bundles.

Optional FP8, sparse, fused, architecture-folding, luma, and RIFE coordinate-specialization experiments were evaluated and retired; no Python optimization feature or dedicated optimization probe is shipped. Only production correctness fixes remain. The [historical RTX 5090 findings](bench_results.md#rtx-5090--retired-model-graph-experiments-2026-10-18) retain measured results and limitations; they are not supported workflows or product-performance claims.

### Built-in Variants & Engine Settings

Settings supports edge/corner resizing and maximization. Processing captions wrap without overlapping their editors; longer form rows put the editor below its caption when needed. Scrollable tab bodies yield to smaller work areas while **Save / Cancel** remain outside the scroll area.

Quality, Balanced, and Performance each have a **Standard / Sharp** selector. Sharp changes the HD model; the SD Compact branch remains unchanged. Required pinned models must be installed through **Settings → Components**.

The manager also exposes backend logging and TensorRT engine settings:

- **Static:** match the current video resolution through `%video_resolution%`.
- **Static ONNX:** use the graph's fixed input shape.
- **Dynamic:** configure minimum, optimal, and maximum input shapes.
- Builder optimization level **0–5**, plus the raw `trtexec` argument string.

The default raw settings are:

```text
--builderOptimizationLevel=5 --optShapes=input:%video_resolution% --skipInference
```

Static/Dynamic helpers target a tensor named `input` and write NCHW shapes. They do not discover ONNX tensor names. For a differently named input, edit the raw shape arguments to match the graph, or remove shape overrides for a suitable fixed-shape graph. These are global AJI settings, so they must be compatible with every graph used by a selected chain.

Engine flags affect TensorRT, not DirectML. Backend choice remains in the player's AnimeJaNai menu. Engine compilation is runtime- and GPU-specific; changing these controls does not guarantee that an existing engine can be reused. Do not move another GPU's compiled engines as a substitute for building locally.

### Custom Slots 1–10

1. Select a slot from **1–10**, then create/reset it or clone a built-in/custom profile. Give it a recognizable name.
2. Add chains and set their resolution/FPS activation bounds. `0x0` or `infxinf` represents an unbounded resolution; `0` represents an unbounded FPS limit.
3. Add processing steps in the required order: **ONNX model** or **Anime4K preset**. Anime4K supports A/B/C Fast and HQ plus A+A/B+B/C+A HQ combinations, with a per-step **1×/2×/4× output scale**. Both step types expose input-height and percentage resizing. Move steps up/down to place shaders before, after, or between ONNX runs. Model names must resolve locally; a profile export is not a model download.
4. Optionally enable per-chain RIFE and select the model, numerator/denominator ratio, scene threshold, ensemble, and before/after-upscale position. A chain needs at least one model or an enabled RIFE step.
5. Press **Save**. Configured slots appear beside the built-in player presets and in Benchmark's **AnimeJaNai** group.

Cloning copies the current model steps. Later Standard/Sharp changes to the original built-in preset do not rewrite the cloned custom profile.

Mixed chains select the first matching chain in ascending chain order using the **original source** pixel area and FPS, with inclusive bounds. Missing source metadata defers preparation rather than guessing a frame rate. Consecutive ONNX steps share an AJI segment; Anime4K steps split the graph into real ordered video filters. RIFE runs once at the configured before/after position. Shader-only profiles require no AJI installation.

These mixed filters are not the player's final `glsl-shaders` overlay. CUDA/D3D11 frames are negotiated to software NV12 before Anime4K; later AJI stages upload them again. This can introduce GPU/host transfers and reduces the boundary to 8-bit NV12, so mixed processing is not a zero-copy or HDR-preservation promise. Existing model-only profiles retain their prior path.

Custom profile IDs are `builtin_slot:<N>`; custom spatial IDs are `spatial:custom:<id>`. Chain and slot switches retain editor changes, including across interface-language changes. Saving the manager does not hot-reload an already running AJI filter; switch to another preset and back, or reopen playback, to prepare the updated configuration.

Use `.pconf` import/export for one profile, or `animejanai.conf` import/export for the complete manager configuration. Importing a profile replaces the selected slot in the editor. Export captures current edits without saving application settings. Neither format embeds model graphs, external registrations, runtime components, or compiled engines; install/register the required models separately on the receiving machine.

**Save** persists settings and profiles; **Cancel** discards those edits but does not undo model imports, registrations, removals, or files already exported. Model import, external registration, and removal take effect immediately; profile and manager-setting edits stay with the editor until **Save**. Resetting manager defaults removes custom slot/settings edits, not the model files or external registrations.

Manager settings remain schema v3 in `<nativeRoot>/animejanai/animejanai_manager.conf`, with `mikai_chain_steps_version=1` for typed ordered steps. Legacy model-only configurations migrate on read; mixed profiles require an app version that understands the extension. Preparation retains provenance and digest checks, stages owned shaders, and emits separate configs/stats for each AJI segment. Stable content-addressed stage directories preserve existing GPU/shape-specific engine reuse. Concurrent preparations of the same owned cache are explicitly refused while its build lease is held. Missing, modified, or unsupported referenced models produce a preparation error rather than silently selecting another graph.

External registrations are stored in `<nativeRoot>/animejanai/models/external-models.json`. Clearing the browse folder does not unregister pinned models; use **Remove Selected** for that.

### Preparation Errors & Recovery

| Symptom | Action |
|---|---|
| Duplicate model identifier | Remove the old imported model or external registration, then import/register the replacement. |
| Graph changed since registration | Restore the pinned bytes, or explicitly remove and re-register the replacement. Do not edit provenance to bypass the digest check. |
| Missing profile model or RIFE variant | Install the required component or select an available graph/variant; the planner does not silently substitute another ensemble. |
| Configuration import/save fails | Read the Processing error banner; a failed import preserves current edits. A malformed saved manager config must be repaired or reset before preparation. |
| TensorRT rejects shapes/operators or engine building fails | Read the full failure reason and builder tail. Check backend compatibility, tensor names, shape settings and the local GPU builder resource; an import check is not inference validation. |

For a non-RIFE benchmark, leave the RIFE group unchecked **and** exclude custom slots with chain interpolation enabled. See [benchmark selection and reporting](bench_results.md#reproducing-and-submitting-another-machine). Functional manager smoke results are not native throughput measurements.

### Windows Manager Smoke

Build `native_settings_probe` and run it with Qt/MinGW DLLs on `PATH`. It uses an isolated executable-local store, exercises the real Settings/player/Compare/Benchmark surfaces, and writes `native-animejanai-smoke-evidence.json` plus screenshots to its working directory. The probe uses Qt widget file pickers for deterministic actions; it does not invoke AJI inference, TensorRT compilation, or a RIFE benchmark.

The existing smoke command remains isolated: it runs from an owned executable copy with its own native store and never touches a user's deployed `native/` data. Extended live scenarios against disposable folders cover catalogue additions, renames, replacements, removals, unregistered candidates, and language preview/restore. The JSON and screenshots capture UI and file-discovery outcomes only; they are not inference, engine-build, playback-performance, or benchmark evidence.

The smoke also checks compact/wide English and Ukrainian Processing captions, captures the preset/TensorRT form, and exercises Settings maximization/restoration and the fixed action dock. The log records the actual device-pixel ratio; `QT_SCALE_FACTOR` multiplies the operating system's existing display scale.

From the repository root in PowerShell:

```powershell
cmake --build build/release --target native_settings_probe --parallel 4
$probe = (Resolve-Path build/release/native_settings_probe.exe).Path
$env:MIKAI_MPV_MODULE = (Resolve-Path build/release/mpv/libmpv-2.dll).Path
$env:MIKAI_PROBE_TRANSLATION = (Resolve-Path build/release/mikai_uk.qm).Path
$env:QT_QPA_PLATFORM = "windows"
$env:QT_OPENGL = "desktop"
New-Item -ItemType Directory -Force .probe-out/aji-manager | Out-Null
Push-Location .probe-out/aji-manager
& $probe
Pop-Location
```

---

## Compare Window Keyboard Shortcuts & Controls

Open the Compare Window from the player toolbar (`Compare` button) while playing any video:

| Key / Action | Function |
|---|---|
| Mode selector | Choose the comparison mode |
| `S` | Swap Side A and Side B candidate selections |
| `R` | Recapture the current video frame from the player |
| `F` | Reset zoom and pan to Fit-to-Window |
| `P` | Toggle Always-on-Top window pin (`Qt::WindowStaysOnTopHint` + Win32 `HWND_TOPMOST`) |
| `F11` or toolbar fullscreen button | Enter or leave fullscreen; the previous normal size or maximized state is restored on exit |
| `Esc` | Leave fullscreen; when windowed, close Compare and release the player pause hold |
| `Space` | Hold to peek Side A (original or reference) across all modes |
| Mouse Wheel / Drag | Synchronized cursor-anchored zoom (up to `32x`) and direct left-drag pan across all panes |

---

## Playback Benchmark

See [benchmark results by machine](bench_results.md) for measured per-model throughput, test conditions, and interpretation limits, including the [170-cell RTX 2070 SUPER capture](bench_results.md#2026-10-20--rtx-2070-super-settings-capture) (23 Anime4K presets, three AJI profiles, five Spatial graphs, baseline, and two RIFE models). Compare matching input resolutions and settings; that independent capture is not a controlled RTX 5090 comparison, and a failed runtime check is not a GPU performance measurement.

The [controlled RTX 5090 AnimeJaNai follow-up](bench_results.md#controlled-animejanai-follow-up) documents a separate old/new comparison and its measurement controls. For repeat comparisons, keep the app foreground on a connected, non-minimized desktop, interleave old/new runs, and record session/focus and cached-engine versus first-use conditions. These controls do not imply stutter-free playback or zero GPU cost.

Settings → Benchmark compares selected clips and models, with an optional unfiltered baseline. Search and group selection keep their choices while filtering; the run action stays visible while the page scrolls.

Every completed Settings run automatically writes one atomic `bench/benchmark-<UTC timestamp>-<UUID>.json` beside the executable, including failed rows. It records stable model IDs, numeric FPS or a null measurement with the failure reason, source/target geometry, engine preparation time, timestamps, backend, GPU/driver/SM, OS and Qt. The completion message names the report; write errors remain visible. Report creation runs on a managed `Runner` worker. Save, Cancel, Escape, and window-close request cancellation even when the dialog is only hidden; the report retains completed rows and any active row interrupted by cancellation. Metadata reads, plan preparation, and sampling honor the cancellation scope. Application shutdown drains benchmark work before reporting completion, without blocking the GUI. A clip whose source metadata cannot be read reports that cause instead of a generic output-size error.

Each run renders through the real mpv OpenGL pipeline at twice the clip's input dimensions. A three-second sample counts new video output frames, not media timestamps, redraws, or repeated frames. Anime4K shaders execute in this render path. The first eligible frame warms up the renderer and is excluded.

TensorRT measurement waits for the selected slot's configured report and real video output. Engine compilation has separate indeterminate progress and elapsed time; compilation time is excluded, and compilation during a sample discards and restarts that sample. Select a failed result to read its full error.

mpv error-level logs are diagnostic only, including while an engine is compiling. Failures require an actual end-file error, a failed build gate, a render error, or a startup/build timeout. Error details retain the last three distinct diagnostic lines; an authoritative playback failure during compilation is headed “TensorRT engine build failed.” rather than aborting on the first downstream stream-selection message.

`benchmark_probe --diagnostic-log-smoke C:\media\moving-video.mp4` checks this policy with real libmpv playback and controlled Lua log messages, without compiling or claiming inference. It verifies that benign build-time chatter reaches a completed sample, while a genuine missing-source error aborts with its bounded, deduplicated cause tail.

From an owned scratch deployment with the bundled clips and runtime, `benchmark_probe --lifecycle-smoke` exercises cancellation without destroying Settings, reusing the dialog, and asynchronous application shutdown during real sampling. `benchmark_probe --source-error-smoke` verifies that a corrupt clip's actual libmpv error reaches both baseline/Anime4K rows and the automatic JSON report. Neither mode compiles engines or benchmarks inference.

The opt-in `benchmark_probe` drives the real Settings tab, defaulting to the baseline and Anime4K models. Run its executable from the deployed directory so the mpv runtime, models, shaders, and clips resolve. `--list-models` prints catalogue IDs; `--model <id,id,...>` and `--clip <filename>` select a subset. `--all-models --all-clips --output results.json` captures the complete matrix with stable model IDs, input clips, numeric fps or full failure reasons. A model failure does not prevent export; check `complete` before publishing the data. `--all-models` includes RIFE; the [published non-RIFE comparison](bench_results.md#reproducing-and-submitting-another-machine) includes a filtered invocation that excludes it. `--size WIDTHxHEIGHT` exercises another window size, and `--expect-failure` verifies an explicitly unsupported model's visible error.

---

## Appearance & Content Language (`Settings → Customization`)

- **Content titles language** (`ua` → `english` → `original`) and **Names language** are separate from the interface language. Names language defaults to **As the interface**; choosing Ukrainian or English relabels statuses, kinds, formats, ratings and relations in place, without restarting the app or refetching data. A payload missing the preferred title falls back to the next form rather than showing a blank.
- **Site theme, accent colour, corner radius** preview live while the dialog is open; rapid changes share one restyle after a 60 ms input pause. **Save** commits the final choice; **Cancel** restores the opening look, including any pending preview. The default corner radius is **XS (4 px)**; an explicitly saved radius stays selected. A light site theme re-derives sheets and painted surfaces from the same palette.
- **Background gradient** paints an accent wash behind the top of the discovery page and the title-page bed. Intensity, height and colour changes repaint only that glow: they do not rebuild stylesheets or cached posters. Related-carousel and episode header bands stay transparent over the glow, with divider lines and controls preserved; content cards remain opaque.
- **Gradient overlay** shades the bottom of poster and cover artwork so the card's own captions stay legible on bright images.
- **18+ content:** with the toggle off, opening an episode of a title rated `18+` asks for confirmation once per title; with it on, playback starts without the prompt.

---

## Configuration & Data Locations

- **Appearance (`customization/*`):** `customization/theme`, `customization/accent`, `customization/radiusStep`, `customization/titleLanguage`, `customization/nameLanguage`, `customization/coverGradient`, `customization/backgroundGradient`, `customization/gradientColor`, `customization/gradientIntensity`, `customization/gradientHeight`, and `customization/adultContent`. Missing values use the defaults (`dark`, `#bdacff`, XS radius, Ukrainian titles, names as the interface, cover overlay on, background gradient on, colour `#67e4e4`, 30 %, 100 %, no explicit content). Invalid colours and language IDs use their defaults; numeric ranges are clamped.
- **API Key (`.env` or Settings):** Optional `API_KEY=<token>` in `.env` (or configured in **Settings → General**) raises rate limits against `https://api.mikai.me/public/v1`.
- **Settings (`QSettings`):** Platform-native settings under organization `Mikai`, application `Mikai Wrapper`. Recently opened titles are JSON in the `home/recentTitles` setting, not a `recent.json` file.
- **Interface language (`ui/language`):** `system`, `en`, or `uk`. The selector previews immediately in the running process; only **Save** writes the preference. **Cancel**, `Esc`, and window close restore the language that was active when Settings opened, without refetching data or restarting playback.
- **Runtime acquisition cache:** `<LOCALAPPDATA>/MPVBackend/runtime`, or `<home>/MPVBackend/runtime` when `LOCALAPPDATA` is unset.
- **Native/model data:** `<applicationDir>/native`, independent of working directory or QSettings identity. AJI manager/model locations are described above; back up the manager configuration, owned models/provenance, and external registry together. Borrowed external graphs need their own backup and valid paths on the destination machine.
- **Diagnostics:** Executable-adjacent `mikai.log`, rotated `mikai.log.1`, and timestamped `mikai-crash-*` reports. They are not named `crash.log`.

---

## Repository Layout

- `src/backend/` — Domain models (`Models.hpp`), API client (`MikaiClient`), HLS stream extractors (`Streams`), verified downloader (`Downloads`), runtime discovery (`Runtime`), and diagnostic scrubbing (`Diagnostics`).
- `src/gui/` — Shared GUI utilities (`Clipboard`).
- `src/app/` — Subdivided into `support/` (`CrashLog`, `I18n`, `Labels`, `WindowRaisePolicy`), `pages/` (`MainWindow`, `SearchPage`, `TitlePage`, `TitlePagePlayerSession`, `EpisodeGrid`), `dialogs/` (`SettingsDialog`, `AnimeJanaiManagerPanel`, `ComponentStoreTab`, `NativeAssetsPanel`, `NativeRifeControls`, `CoverViewer`, and `settings/` tabs), `state/`, `widgets/`, `theme/`, and `workers/` (`Runner`).
- `src/player/` — Subdivided into `core/` (`MpvApi`, `MpvHandle`, `MpvRenderContext`, `PlaybackDiagnostics`, `VideoSurface`), `shaders/` (`Anime4K`, `PresetShortcuts`), `shell/` (`PlayerDialog`, `PipFloat`, `PlayerShell`, `PlayerPopups`, `ShellWidgets`), `compare/` (`CompareWindow`, `CompareCanvas`, `CompareGrid`, `ComparePane`, `CompareLayout`, `CompareRenderer`), and `native/` (`bundle/`, `components/`, `models/`, `prepare/`, `status/`).
  - `native/components/AnimeJanaiConfig` owns GUI-free custom-model metadata, schema-v3 manager configuration and profile serialization; `AnimeJanaiCatalogWatcher` publishes metadata-only model-catalogue changes; `AnimeJanaiRuntime` performs runtime planning/staging. They compile into `mikai_native_support`; the manager widget belongs to `mikai_app`.
- `src/probes/` — Standalone GUI and headless verification probes (`compare_probe`, `shell_probe`, `dialog_probe`, `native_component_probe`, `probe_runner`, etc.).
- `cmake/` — Modular CMake build helpers (`MinGWRuntime.cmake`, `FindOrFetchMpv.cmake`, `MikaiProbes.cmake`, `MikaiTests.cmake`, `dist.cmake`, `MikaiTestLauncher.cpp`).
- `native/` — Standalone MSVC + CUDA + TensorRT inference shim build (`native/CMakeLists.txt`), CLI tools (`mikai_bundle`, `mikai_manifest_check`, `native_inspect`), patches, and provenance manifests (`native/manifest/`).
- `tests/` — Hermetic Qt Test suites grouped into `tests/cpp/{backend,native,player,app}/` and CMake contract tests (`tests/cmake/`, `tests/fixtures/`). [Fixture documentation](tests/fixtures/README.md) distinguishes the Identity ONNX graph and test dispatcher from inference runtimes.
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
