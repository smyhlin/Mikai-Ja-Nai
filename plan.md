# Mikai Janai — native AnimeJaNai integration plan

Companion document: **[tasks.md](./tasks.md)** is the sole execution, status and
acceptance record. It carries the eight cohesive workstreams, the next runnable
batches, the critical path, the 52 implementation packages `M01`–`M52` with
per-task acceptance, and the full acceptance catalog (M/C/R/N). This plan carries
the durable architecture, contracts, decisions and source evidence. **PROPOSED**
sections retain the intended contracts, and every work package named here links
to its task. The independent baseline and media fixtures are verified; native
delivery remains gated by rights, preparation and producer prerequisites.

---

## 1. Conclusion and no-break scope

Mikai (`F:/ZKS/mikai_janai`, C++20 / Qt 6.8 / MinGW-w64, in-process `libmpv`)
gains the original Janai AnimeJaNai neural pipeline — generic spatial ONNX
chains, original RIFE v4.x interpolation, and combined recipes — by shipping a
**pinned, patched mpv fork that carries the native `animejanai` vf**, loading
that core through one explicit process-wide loader, and driving preparation,
recipes and status from new QtCore-only modules inside this repository.

Four rules are non-negotiable and are restated at every gate:

1. **This repository is independent of any other application's installation and
   mutable state** — specifically `F:/ZKS/Interp/AnimeInterp` and the Janai
   package. No Python, no planner, no registry, no service, no config, no cache,
   no asset, no subprocess `mpv`, no rawvideo handoff is read from or shared with
   them. Pinned, owned third-party sources and SDKs (Qt, the mpv fork, CUDA,
   TensorRT, ONNX Runtime, DirectML) are of course permitted and required — the
   rule is about *project-to-project coupling and borrowed mutable state*, not
   about third-party dependencies. The native C++ helpers proposed here live in
   `F:/ZKS/mikai_janai/native/` and are built from this repository's own
   manifests.
2. **The OpenGL render path is retained.** The player already renders mpv into a
   Qt `QOpenGLWidget` framebuffer (`VideoSurface` → `MpvRenderContext`) and the
   research proof in §3.3 shows CUDA frames reach the Qt framebuffer through it
   as a GPU-resident result with no decoder or render-path rewrite and no
   Vulkan and no HWND. The pixels are **not** unchanged — inference and scaling
   alter them by design — and no fidelity claim is made. No renderer rewrite.
3. **One `DLLMODULE`, many core handles.** One explicitly loaded `libmpv-2.dll`
   serves every handle (main player, PiP float, probes). The native filter lives
   *inside* that module, so there is no second DLL, no second loader path, and no
   mixed-core state anywhere in the process.
4. **No feature is deferred, narrowed, or renamed.** Spatial-only, RIFE-only and
   combined recipes are all in scope; runtime provisioning, preparation, status,
   settings, PiP, Compare, HLS, error and lifetime preservation are all in
   scope; DirectML is a capability-gated track with a real proof requirement, not
   a promise.

Explicitly **out of scope**: copying any reference player (Lua scripts, input
conf, `mpv.conf`, `mpvnet`, `AnimeJaNaiManager`), a CLI transcoder, a new API
surface, a home/history schema change, a new quality cache, or any change to the
Mikai API client contract in `docs.md`.

---

## 2. Evidence-first baseline of this repository

§2.1 describes the landed loader. §2.2–§2.6 preserve the pre-cutover source
snapshot that motivated the requirements, not current implementation claims.
§2.7 records assertion retirement and §2.8 inventories independent verification.
The observed audit ledger is [tasks.md §1.6](./tasks.md#status); task acceptance
is tracked in `tasks.md`.

### 2.1 Explicit libmpv module selection (landed)

The load-time libmpv import and startup `PATH` mutation are removed.
`CMakeLists.txt` requires only `MPV_INCLUDE_DIR` for build-time headers;
`MpvApi` loads and pins one selected module explicitly. `MpvHandle` and
`MpvRenderContext` use that table, including the multiple handles needed for PiP.
The isolated OFF/ON executable import lists recorded in
[tasks.md §1.6](./tasks.md#status) contain no libmpv import.

Startup order is `installCrashLog()` → `QApplication` → application/organization
identity → locale/theme setup → `selectPlayerRuntime()` /
`selectStartupRuntime()` → `MikaiClient` → `MainWindow`. Selection therefore
runs after the identity required by `QSettings`/`QStandardPaths`, and before any
`VideoSurface::ensureEngine()`. Missing or refused runtimes are named rather
than treated as a successful load.

This is Standard playback delivery, not Native inference delivery.
`VideoSurface::paintGL()` still renders libmpv directly into Qt's framebuffer.
`nativeAvailabilityFor()` in `NativePrefs.cpp` reports `engineAvailable=false`;
requested native choices and status consumers do not install an `animejanai`
inference chain. M03–M05's producer/bundle prerequisites and M08's remaining
recovery acceptance are not discharged by the loader proof. See §8.1 and
[tasks.md §1.6](./tasks.md#status).

### 2.2 Ownership, state and thread map of the playback core

| Owner | Owns | Thread |
|---|---|---|
| `VideoSurface` (`src/player/VideoSurface.cpp`) | `std::unique_ptr<MpvHandle> mpv_`, `std::unique_ptr<MpvRenderContext> render_`, `QOpenGLContext*`, engine options, `glsl-shaders` | GUI thread |
| `MpvHandle` (`src/player/MpvHandle.cpp`) | one `mpv_handle*`; `mpv_create` → `mpv_set_option_string` → `mpv_initialize`; `mpv_request_log_messages(handle,"info")` | constructed/destroyed on GUI thread; events drained by `drainEvents()` on the caller thread |
| `MpvRenderContext` (`src/player/MpvRenderContext.cpp`) | `mpv_render_context*`; `mpv_opengl_init_params` with a `get_proc` trampoline; update callback fires on an **arbitrary mpv thread** and is forwarded as `std::function` | teardown detaches the callback first |
| `PlayerDialog` (`src/player/PlayerDialog.cpp`) | shell, surface, status panel, retry button, `pip_` (active float), `pipPool_` (parked float) | GUI thread |
| `PipFloat` (`src/player/PlayerDialog.hpp:72-173`) | its **own** `VideoSurface` and therefore its **own** mpv core | GUI thread |
| `VideoSurface::statePump_` (`QTimer`) | playback clock, duration, `playing_` | GUI thread |
| `Runner` (`src/app/workers/Runner.hpp`) | three `QThreadPool`s: shared 4, bulk 3, image 12 | worker threads; every completion is discardable |

Events are **polled, not edge-triggered**: `MpvHandle` never calls
`mpv_set_wakeup_callback` (the header's opening comment mentions it, but no call
site exists in `src/`), and nothing sets a wakeup flag. `VideoSurface::pumpEvents()`
calls `drainEvents()` from its 50 ms `QTimer`, which drains with `mpv_wait_event(0)`
on the GUI thread. The **only** thread notification in the player is
`mpv_render_context_set_update_callback`, whose callback fires on an arbitrary
mpv thread and is forwarded as a plain `std::function`. Property observers
subscribe with `MPV_FORMAT_NODE_ARRAY` and fire *inside* `drainEvents()`; no
caller registers one today (see §7.8). Any native status consumer must respect
these facts: it may only touch widgets from the drain, and it must never poll
from `paintGL`.

### 2.3 ABI surface in the pre-cutover snapshot

* `mpv_handle`: `create`, `set_option_string`, `initialize`, `request_log_messages`,
  `terminate_destroy`, `command` (`loadfile`/`seek`/`stop`), `get_property`
  (DOUBLE/FLAG/STRING), `set_property` (DOUBLE/FLAG/STRING), `observe_property`
  (NODE_ARRAY), `wait_event(0)`, `client_api_version`.
  The Qt `MpvApi` resolves **only** the standard client and render API from the
  loaded `libmpv-2.dll`. It does **not** resolve the native snapshot export:
  that symbol belongs to the separate `aji.dll` provider dispatcher (which the
  filter loads via its `lib=` option and which itself `LoadLibraryExA`s the
  chosen backend `aji_trt`/`aji_dml`). The boundary is: **the native filter
  calls the snapshot export inside `aji.dll`/the backend and bridges the result
  into `MP_FILTER_COMMAND_GET_META`; the Qt side reads it as an ordinary mpv
  property and never links or resolves the native symbol directly.**
* `mpv_render_context`: `create` with `MPV_RENDER_API_TYPE_OPENGL`,
  `set_update_callback`, `update`, `report_swap`, `render` with
  `MPV_RENDER_PARAM_OPENGL_FBO` + `MPV_RENDER_PARAM_FLIP_Y`, `free`.
* GL resolver: `wglGetProcAddress` **plus** a `GetProcAddress` fallback on
  `opengl32.dll`, handed to mpv as the full `GetProcAddress` (core GL 1.1
  symbols live inside `opengl32.dll`). Hard-fails with
  `"OpenGL: required entry points missing"` when
  `genTextures`/`bindTexture`/`getTexLevelParameteriv`/`getIntegerv` are absent.

The loader introduced in §8 **must** supply the same symbol set from the loaded
module, plus the new additive snapshot export (§6.7). No existing signature
changes.

### 2.4 Engine options — the exact set the native work must preserve

`VideoSurface::ensureEngine()` sets, in this order, before `mpv_initialize()`:

```
vo=libmpv
user-agent=Mozilla/5.0 (Windows NT 10.0; Win64; x64) …Chrome/142.0.0.0 Safari/537.36
http-header-fields=Accept-Language: uk-UA
hwdec=auto-safe
keepaspect=yes
volume=100
input-default-bindings=no
input-vo-keyboard=no
input-cursor=no
osc=no
```

plus `glsl-shaders` written after initialize. `http-header-fields` must stay
comma-free (mpv splits on commas) and `Accept-Language: uk-UA` is load-bearing:
MoonAnime's CDN answers 400 without it. Native work may **override `hwdec` per
session** (§7.3) but must never drop the user agent, the language header, or the
input-binding options.

### 2.5 PiP lifetime in the pre-cutover snapshot

* `PlayerDialog::togglePip()` (public) delegates to the private
  `enterPip()`, which takes `pipPool_` when it exists and otherwise news a
  `PipFloat`; connections are made **once** (`if (fresh)`); the float's own
  `VideoSurface` gets its own Anime4K chain copied from the recorded candidate id
  (`anime4kChainId_` if set, else `anime4k::modeId(anime4kMode_)`), and the main
  core is paused (`pipResumePosition_`, `pipResumePlaying_`).
* `PlayerDialog::leavePip(bool closing)` reads the float's position **first**,
  `hide()`s it, and pauses its core. It does **not** call `closeStream()`.
* `PipFloat::closeEvent` does call `closeStream()` — but a parked float is
  `hide()`den, never closed, so that path is not taken on a normal hand-back.
* `PlayerDialog::~PlayerDialog()` does `delete pip_` and **never touches
  `pipPool_`**. `closeEvent()` calls `leavePip(/*closing=*/true)`, then
  `shell_->close()` and `surface_->close()`.

**Assessment (source-level risk, not a reproduced defect):** a float parked
hidden keeps a live mpv core, and the float behind it is not owned by the
destructor. With a plain stock core this costs one idle decode context; once the
core carries the native filter, the parked instance also pins a CUDA context, an
execution context, TensorRT/DirectML engine bindings and intermediate device
allocations. That is a resource-lifetime obligation this integration must
discharge deliberately (§7.6), and it must be *proved* by a new native close
probe ([M45](./tasks.md#m45)). No crash or leak is claimed here.

### 2.6 Compare, HUD, settings, menu and shortcuts — what must not move

* **Compare** (`src/player/CompareWindow.cpp`): `captureFrame()` pauses playback,
  `grabSettledFrame()` takes **one** frame without re-seeking, and both sides
  render from that single image through `renderWithChain()`, which writes the
  chain onto the borrowed live surface via `VideoSurface::setAnime4KChain`,
  forces `update()`, and waits for two identical grabs. `render()` reads the
  live chain id **before** both sides and writes it back **after** both, so the
  window never leaves the player running a preview chain. The noise floor is
  "two settled frames *without* re-seeking" — re-seeking turns the 0.000 floor
  into a frame-to-frame difference (~5/255 in the recorded runs).
* **HUD** (`PlayerShell::updateInfoChip`, `PlayerShell.cpp:1064`): throughput ×
  FPS from `cache-speed` and `estimated-vf-fps`, plus, while an Anime4K preset is
  active, `│ Anime4K <label> · WxH → WxH`. `estimated-vf-fps` is mpv's *measured
  output* rate; it is not an inference rate and not the source work rate.
* **Enhancement tree** (`Anime4K.cpp:205-268`): one declarative catalogue. The
  category table already contains `rife`, `realesrgan` and `other-models` rows
  whose child leaf is a placeholder carrying
  `"Planned: RIFE is not implemented yet"` / `"Planned: RealESRGAN is not
  implemented yet"`. The native work replaces the RIFE placeholder with a real
  capability subtree and **leaves the other two placeholders exactly as they
  are**. No seventh dock button; the sparkle menu remains the single entry point.
* **Anime4K gating**: presets carry `//!WHEN OUTPUT.w MAIN.w / 1.200 > …`, so an
  upscaler declines when mpv already displays the video at or above source size.
  That gate is evaluated against the **post-native** image once a spatial stage
  is active, and must be measured (§7.7) — never blanket-disabled.
* **Settings** (`src/app/dialogs/SettingsDialog.cpp`): a `QTabWidget` with a
  General tab and a Presets tab; preset bindings live under
  `player/presetShortcuts` (`PresetShortcuts.hpp`). Native preferences are a new
  additive group, and native shortcuts a new group with the same shared
  collision check `isReservedPresetShortcut()` — reserved in every modifier
  form, because the shell and dialog key handlers switch on the key alone.
* **Startup sequencing that must not change**: `MainWindow::openSettings()`
  closes the dialog and calls `search_->refreshFeedSettings()`. A new native
  preference notification must **not** trigger an API cache reset or a feed
  refetch.
* **Language**: English is the source language in `tr()` /
  `QCoreApplication::translate()`; `translations/mikai_uk.ts` is the Ukrainian
  catalog, deployed through `qt_add_translations(mikai …)` plus the `qtbase_uk.qm`
  POST_BUILD copy. New native strings follow the same rule and are added to the
  existing catalog — not a second one.

### 2.7 Baseline assertion retirement

M01 removes the old root-category and planned-page shape pins in `TestAnime4K`,
the Back-row/Ukrainian wording pins in `InteractionProbe` and `Anime4KMenuProbe`,
and tooltip/object-name assertions used to identify those controls. The tree
offers panels side by side, not a Back row. The updated checks use catalog IDs
to exercise preset application, inert placeholders and category navigation.
Shader chain identity, missing-stage refusal, frozen-frame noise floor and PiP
behavior remain covered by their existing assertions.

The test runner now reports the gated live oracle as `SKIP`, and its shared
suite selector includes `TestStreams`. The first current-source hermetic run
passes all ten non-live suites; the live oracle reports `SKIP`. Further harness
work isolates every settings-reading probe and removes remaining wording-only
assertions. M01 stays incomplete until the final harness and desktop runs are
observed. Explicit English/Ukrainian catalog-loading coverage remains assigned
to M38.

### 2.8 Test and probe inventory (current)

`tests/cpp/main.cpp` drives one `mikai_tests` executable, one ctest entry, with
suites `TestRuntimeSelection`, `TestNativeTypes`, `TestNativeRecipe`,
`TestNativeProfiles`, `TestNativeRife`, `TestNativeBundle`,
`TestNativePrepPlan`, `TestNativeManifest`, `TestNativeBuilder`,
`TestNativeStatus`, `TestPlaybackDiagnostics`, `TestNativePrefs`,
`TestNativeMenu`, `TestNativeUserRecipes`, `TestNativeAssets`, `TestMpvAsync`,
`TestNativeCatalog` (only with `MIKAI_WITH_MODEL_INSPECTION=ON`), `TestModels`,
`TestReleases`, `TestAnime4K`, `TestI18n`, `TestStreams`, `TestPagers`,
`TestQualityCache`, `TestRateLimiter`, `TestLayering`, `TestClipboard`, and
`TestLiveOracle` (gated on `MIKAI_LIVE=1` + `MIKAI_API_KEY`); current run
status is in [tasks.md §1.6](./tasks.md#status).
`MIKAI_SOURCE_DIR` lets the layering guards read the sources they police.
Probes (CMake targets, `EXCLUDE_FROM_ALL`, not tests): `mpv_surface_smoke`,
`headless_pixel_probe`, `widget_pixel_probe`, `shell_probe`, `dialog_probe`,
`interaction_probe`, `compare_shot_probe`, `compare_probe`,
`anime4k_menu_probe`, `resolver_probe`, `title_page_probe`, `crash_probe`,
`external_media_probe`, `hls_recovery_probe`, `mpv_loader_probe`,
`runtime_selection_probe`, `native_menu_probe`, `native_settings_probe`,
`rife_settings_probe`, `native_status_probe`, `native_assets_probe`,
`spatial_assets_probe`, `native_prep_probe`, `native_verification_probe`,
`native_dml_interop_probe` and `native_abi_probe`, plus the native CLIs
`mikai_native_prep`/`mikai_native_verify`/`native_inspect` and the bundle tools
`mikai_bundle`/`mikai_manifest_check`. There is no linter, no type checker, no
coverage tool
and no CI. CPU-only tests plus real-desktop probes are the available proof
channel; **no offscreen-GPU proof exists**.

### Baseline status

Current build/run status, the isolated suite ledgers and the observed native
probes are tracked in **[tasks.md §1.6](./tasks.md#status)** and in each task's
`Status` line. This plan records only the durable source evidence (§4) and the
stated proof limits (§3.3). Configure commands live in `readme.md`; after the
loader cutover the build uses `MPV_INCLUDE_DIR` (headers only).

---

## 3. Proof environment, evidence and limits

The observations below are the only things actually executed. Everything else
in this plan is unverified.

### 3.1 Proof environment

| Fact | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 5090 |
| Driver | 616.92 |
| OpenGL | 4.6 (NVIDIA) |
| Qt | 6.8.x MinGW (`<qt>/6.8.3/mingw_64`) |
| Host toolchain | MinGW-w64 g++ (`<w64devkit>/bin/g++.exe`) |

### 3.2 Artifact identity

Observed binary hashes for the research package are recorded, as **descriptive
research observations, not release pins**, in
[`native/manifest/sources.json`](./native/manifest/sources.json). They become
pins only after the provenance/rights gate (M03) and the bundle gate (M05)
close. No cryptographic source→binary reproduction was proved; the package was
confirmed by the user to be built from
`C:/Users/myhli/Downloads/janai/animejanai-inference-main`, but a rebuild from
that tree has **not** been reproduced and byte-matched.

### 3.3 Observed research evidence

The load-bearing observations, and what they do and do not prove:

* **The current render bridge works unmodified against a stock core** — a temp
  Qt harness compiled from the current `MpvHandle.cpp` + `MpvRenderContext.cpp`
  rendered a real Qt frame (604×360, mean 114.723, variance 4672.205, PNG
  inspected, drop counts 0). It proves no performance, fidelity or long-run
  stability.
* **CUDA decoder → CUDA native filter → Qt OpenGL framebuffer works with no
  Vulkan and no HWND** — the same bridge plus the Janai package core
  (`v0.41.0-dev-g63d127174`) + a compatible Cyte FP16 cached engine + `aji.dll`
  loaded a 4 MiB TensorRT engine, configured slot 1 (1920×1080 → 3840×2160),
  captured a CUDA graph, and produced decoder CUDA/P010 → vf CUDA/YUV444P16
  3840×2160 → Qt FBO 604×360 (mean 114.821, variance 4685.251, PNG inspected).
  This is the load-bearing fact behind "keep OpenGL". It proves no temporal
  interpolation, prepared-cache correctness, PiP/Compare/HLS with native,
  DirectML, deployment or fidelity claim.
* **The native filter is only in the fork core** — a stock core rejects the
  exact native `vf` request with *error setting option*; dropping `aji.dll`
  beside a stock core cannot register anything.
* A fresh build of the current libraries plus the full original QTest sources
  passed the hermetic suite once `shaders/` was deployed beside the runner (the
  earlier `TestAnime4K` failures were a scratch prerequisite, not a defect), and
  a fresh `PlayerDialog` wrapper rendered identically on the stock core and the
  fork core with native **off** — it says nothing about native **on**.

Historical run logs live under `.probe-out/`; the current canonical status is
[tasks.md §1.6](./tasks.md#status).

### 3.4 Stated limits of the proof

* The historical native research run emitted a debug `INVALID_ENUM`. The
  current stock-widget audit also reproduced real attachment errors before the
  Qt FBO restoration fix; the current widget audit records zero ErrorType /
  `glGetError` errors
  after the fix. Performance warnings remain. A native-on current-tree run is
  still unverified (`M43`); neither run is described as warning-free.
* The evidence used a **borrowed read-only fixture and a borrowed read-only
  cached engine**. No cross-project import or call was made, and none is planned.
* The native run used `AddDllDirectory` on an existing TensorRT / Torch-Runtime
  directory. That was **investigation only**. Production must ship a
  self-contained dependency closure inside the native bundle — never a Conda or
  torch/Python directory (§9.2, `M05`).
* Long-run performance, memory stability, load latency and sustained cadence are
  **entirely unmeasured** (`M49`).
* **The interactive desktop session must be attached for any GL acceptance.**
  The probes that drive a real window ran twice in one session: once with the
  session attached (`widget_pixel_probe` exit 0, `nonblack=6420/6420`, PNG
  inspected - the RGB bars and frame counter are real pixels) and once after the
  session reported `Disc` (`query session`). In the disconnected state the
  module loads and the framebuffer is a valid 320x180 image, but no video
  produces a lit pixel in any probe, which is the pixel count a black frame
  gives too.
  The two states are distinguished by `hasVideo`/`playing`, not by the pixel
  count, so the `widget_pixel_probe` now prints the surface format and
  `hasVideo`/`playing`/size beside the count. Every GL-dependent acceptance in
  `M42`-`M49` is blocked while the session is disconnected; the CPU suites,
  loader selection and the pure contract tests are not.
* The native filter has **not** been run inside the full current `PlayerDialog`.
  The compact evidence above proves it through the current core and render
  bridge; the fresh `PlayerDialog` composition is proven only with native
  **off**. There is **no** PiP, Compare, HLS or RIFE-with-native evidence of any
  kind.
* The tall 600×1300 dialog geometry is a property of the probe, which runs no
  main window and therefore no Fusion stylesheet or normal placement. It is
  **identical on both cores** and is not evidence of a native-core layout
  difference.
* The unit baseline is hermetic only: the run set `MIKAI_LIVE=0` and supplied
  no API key, so `TestLiveOracle` is skipped by design and the 175/0/1 result
  says nothing about live API behaviour.
* "The current sources compile clean" means the build **succeeded** — not that it
  is warning-free. No linter or type checker is configured, and no warning audit
  was performed.
* The 600×1300 dialog height is **[INFERENCE]** attributable to
  the probe running no main window and therefore no Fusion stylesheet and no
  normal placement: the omission is directly observed and the geometry is
  identical on both cores, but the specific source of the height was not
  isolated. It is not asserted as a native-core defect.
* Every probe's temporary source and executable lived outside this repository
  and has now been **removed** (`rm` exit 0); no `tests/` or `src/probes/` file
  was added. This research changed **no application source**, and the read-only
  reference trees (the Janai inference source, the 3.6.3 package) were left
  untouched.

---

## 4. Native compatibility and source evidence

### 4.1 Pinned mpv fork

All citations are pinned to core SHA
`63d127174d8f70336c82a22408929e2bc301edcb` (verified reachable, HTTP 200):

* [`video/filter/vf_animejanai.c`](https://raw.githubusercontent.com/the-database/mpv/63d127174d8f70336c82a22408929e2bc301edcb/video/filter/vf_animejanai.c)
* [`video/out/hwdec/hwdec_cuda.c`](https://raw.githubusercontent.com/the-database/mpv/63d127174d8f70336c82a22408929e2bc301edcb/video/out/hwdec/hwdec_cuda.c)
* [`video/out/hwdec/hwdec_cuda_gl.c`](https://raw.githubusercontent.com/the-database/mpv/63d127174d8f70336c82a22408929e2bc301edcb/video/out/hwdec/hwdec_cuda_gl.c)
* [`filters/filter.h`](https://raw.githubusercontent.com/the-database/mpv/63d127174d8f70336c82a22408929e2bc301edcb/filters/filter.h)
* [`player/command.c`](https://raw.githubusercontent.com/the-database/mpv/63d127174d8f70336c82a22408929e2bc301edcb/player/command.c)
* [`filters/f_lavfi.c`](https://raw.githubusercontent.com/the-database/mpv/63d127174d8f70336c82a22408929e2bc301edcb/filters/f_lavfi.c)
* [`meson.build`](https://raw.githubusercontent.com/the-database/mpv/63d127174d8f70336c82a22408929e2bc301edcb/meson.build)

What the pinned source actually shows:

* `meson.build` compiles `vf_animejanai.c` and `hwdec_cuda.c` only when
  `features['cuda-hwaccel']` is allowed (which `require()`s `ffnvcodec`), and
  `hwdec_cuda_gl.c` only when `cuda-interop` **and** `gl` are on. `cuda-interop`
  `require()`s `cuda-hwaccel` and (gl **or** vulkan). So the GL/CUDA interop this
  integration depends on is a first-class, explicitly gated build option.
* The filter registers as `name = "animejanai"` with options
  `conf, model-dir, trtexec, trtexec-libdir, slot, stats, engine, lib,
  rife-model-dir, passthrough, skip-seek-pre-target, output-444, queue-depth`,
  and priv defaults `passthrough=true, slot=1, skip_seek-pre-target=true,
  output-444=true, queue-depth=3`. `queue-depth` is `M_RANGE(1, MAX_DEPTH)`.
* RIFE-first output PTS are computed by the filter as
  `pts = p->rife_prev->pts + (src->pts - p->rife_prev->pts) * t` with
  `nominal_fps = in_fps * rife_num / rife_den`, the source grid point keeps
  `src->pts`, and a failed interpolation falls back to emitting the upscaled
  source frame and resyncing the accumulator. `queue depth` is forced to 1
  whenever `rife_on`.
* CUDA↔GL interop **does** exist in the fork's decode/render path, but the
  GPU-side data movement is a real copy chain (device↔CUDA-array staging plus a
  synchronize), not "zero copies". The plan states this plainly in the HUD and
  in `M51`; no zero-copy claim is made anywhere.

### 4.2 The metadata route is already implemented upstream — not an open question

`filters/filter.h` defines `MP_FILTER_COMMAND_GET_META` with
`cmd.res` required to point at a `struct mp_tags*` that the filter sets to a new
instance. `player/command.c`'s `mp_property_filter_metadata` walks the VO chain
(`vf`) or AO chain, issues that command with the label, and exposes the resulting
tags as the `vf-metadata/<label>` property; it `talloc_free`s the node on the way
out. `filters/f_lavfi.c` answers the same command with `mp_tags_dup`.

Therefore: **no new `player/command.c` or `m_property` extension is needed.** The
owned fork only adds a `GET_META` handler to the filter it already owns, under
the label `mikai_native`, and the app reads `vf-metadata/mikai_native` as
`MPV_FORMAT_NODE` through the loader-resolved API. This is settled design, and it
is stated here so no task re-opens it as a question.

### 4.3 Native inference ABI (read-only reference tree)

`C:/Users/myhli/Downloads/janai/animejanai-inference-main` (read-only;
**not** a dependency of this repository). `include/aji.h` declares a C ABI,
`AJI_API_VERSION 8`, with only C types and opaque handles crossing it.

* **Formats**: `AJI_FMT_NV12`, `AJI_FMT_P010`, `AJI_FMT_YUV444P16`
  (TensorRT backend only), `AJI_FMT_RGB10A2` (DirectML input only).
* **Two modes**: *conf mode* (`conf_path` + `model_dir` + `trtexec` + `slot`,
  plus `rife_model_dir`, `async_build`, `d3d11_device`) and *direct mode*
  (single fixed engine via `engine_path`, `max_width/max_height`).
* **Frame**: `aji_frame{width,height,format,matrix,range,siting,plane[3],stride[3]}`,
  planes as `CUdeviceptr` on TensorRT and `ID3D11Texture2D*` + subresource on
  DirectML; caller's CUDA context and stream.
* **Lifecycle**: `aji_create` → `aji_set_slot` → `aji_configure(w,h,fps,&ow,&oh)`
  → `aji_infer` → `aji_flush`/`aji_done`/`aji_wait` (completion tickets,
  monotonic, submission-ordered) → `aji_destroy`.
* **Temporal**: `aji_rife_factor(&num,&den)`, `aji_rife_before_upscale()`,
  `aji_pre_resize(&w,&h)`, `aji_resize`, `aji_infer_rife(a,b,t,out,stream)`,
  which returns `AJI_SCENE` when the pair straddles a scene change (output
  untouched; the reference pipeline emits a duplicate of `a`), and
  `aji_poll()` for async builds.
* **Introspection today**: `aji_current_log()` — a *human-readable* log in the
  shape of `currentanimejanai.log` — plus `aji_scale_factor()`,
  `aji_last_error()`. **There is no structured snapshot.** Parsing that log is
  explicitly rejected (§5, D4).

Build requirements from that tree's `CMakeLists.txt`: `LANGUAGES C CXX CUDA`,
C++17, `CMAKE_CUDA_ARCHITECTURES 75-real 80-real 86-real 89-real 90-real
100-real 120-real 120-virtual`, `find_package(CUDAToolkit REQUIRED)`, and
`AJI_NVINFER` defaulting to `nvinfer_10` (a tested TensorRT 11.3.0.99 setup needs
`nvinfer_11` explicitly). **A CUDA minor version is never inferred from the
TensorRT version.** `aji_trt` links `AJI_NVINFER` + `CUDA::cuda_driver` +
`CUDA::cudart`; `aji` is a thin dispatcher with no CUDA/TRT/ORT dependency of its
own that `LoadLibraryExA`s the chosen backend from its own directory; `aji_dml`
is Windows-only, links `d3d12 d3d11 dxgi d3dcompiler`, and loads
`DirectML.dll` then `onnxruntime.dll` from its own directory in that order
(ORT delay-loads DirectML), requiring the caller's `ID3D11Device`.

### 4.4 Conf semantics that define recipe identity

`src/aji_conf.cpp` in the reference tree:

* Built-in slots are **`1001` Quality, `1002` Balanced, `1003` Performance**,
  plus reference templates `1010`/`1011` (single unrestricted chain) and
  `1012`/`1013` (identical Balanced model, 2× RIFE, differing only in
  `rife_before_upscale`). These are **reference templates, not the identity of a
  model**; this repository names models by content, not by slot.
* Per model: `model_<i>_name`, `resize_factor_before_upscale` (default 100),
  `resize_height_before_upscale` (default 0). A chain with an empty model name
  is skipped; a chain whose model file is missing is reference "missing model
  skip" behaviour, **not** a silent fallback to another model.
* Per chain: `min_resolution`/`max_resolution` (pixels × fps windows),
  `min_fps`/`max_fps`, and RIFE keys `rife`, `rife_factor_numerator` (default 1),
  `rife_factor_denominator` (default 1), `rife_model` (default `414`),
  `rife_ensemble`, `rife_scene_detect_threshold` (default `0.150`),
  `rife_before_upscale` (default `yes`).
* `rife_model_name`: `414 → rife_v4.14`, `4141 → rife_v4.14_lite`, ensemble
  appends `_ensemble`.
* `setup_rife` pads to a multiple of 64 (`pw = (w+63)/64*64`), centres the
  padding and **rounds it down to even** so 4:2:0 chroma stays aligned, and
  builds `--stronglyTyped --optShapes=input:1x11xPHxPW
  --inputIOFormats=fp16:chw --outputIOFormats=fp16:chw
  --tacticSources=-CUDNN,-CUBLAS,-CUBLAS_LT --skipInference`.

### 4.5 The cache problem this integration must fix

`src/aji_trt.cpp` in the reference tree:

```cpp
std::string engine_path_for(model_dir, onnx_name, settings) {
  return (model_dir / (onnx_name + "." + crc32(settings) + engine_suffix())).string();
}
std::string engine_suffix() { return ".trt-" + trt_version + ".gpu-" + gpu_token + ".engine"; }
std::string gpu_token() { /* cudaGetDeviceProperties(&prop, 0) */ name + "-sm" + major; }
```

Identity is **basename + CRC32 of the build-settings string + TensorRT version +
GPU name/SM**. There is **no content hash of the ONNX, no GPU UUID**. Two
different ONNX files with the same basename collide. Changing an `.onnx` in place
does not invalidate the engine. `clean_stale_engines()` then deletes every
foreign `.engine` in `model_dir` — i.e. it treats a user or model directory as a
writable cache and can delete a file it did not create.

**This is exactly the behaviour the owned fork replaces** (`M18`, `M14`). The
rules are absolute: content-addressed identity, an app-owned manifest per
prepared artifact, a fork that resolves a *named prepared artifact* rather than
autocreating from a weak key, and an app-owned cache directory that is the only
writable location. External model roots may be read, never written, never
cleaned, and never used as a cache.

### 4.6 Scene metric honesty

`aji_infer_rife` computes a scene-difference scalar on the unpadded luma,
`cudaMemcpyAsync`s that single float to host and synchronizes, and returns
`AJI_SCENE` when the normalized sum exceeds the threshold. So the temporal path
performs **a D2H copy of one scalar and a stream synchronize per pair**. The plan
does not claim an entirely device-resident temporal path. A software-decoded
source additionally needs one GPU upload of the frame; the HUD labels that
honestly and never claims NVDEC.

### 4.7 Package facts (read-only reference)

`F:/ZKS/janai/mpv-upscale-2x_animejanai-v3.6.3` — `manifest.json` declares
`package_version 3.6.3`, `mpvfork 2026-09-17-63d127174d-63d127174d`,
`inference_runtime trt-11.3.0.99`, `ort_dml 1.24.4+1.15.4`, `rife
models-rife-fp16-1`, `win-x64`. `animejanai/onnx/` holds five spatial ONNX files
(two HD V3.1 Balanced, two HD V3.1Sharp1, one SD V1beta34 Compact
`1x3xHxW_dyn-HW_strong_fp16_op21_dynamo`); `animejanai/inference/` holds
`aji.dll`, `aji_trt.dll`, `aji_dml.dll`, `onnxruntime.dll`, `DirectML.dll`,
two notice files (`THIRD_PARTY_NOTICES.txt`, `TensorRT_Acknowledgements.txt`)
and the harness binaries. The tree also contains `portable_config/scripts/*.lua`,
`mpv-animejanai.conf`, `input-animejanai.conf` and `AnimeJaNaiManager.exe`,
`mpvnet.exe` — **all of which are explicitly not adopted** (§1). Only the
`animejanai` semantics (the filter and the C ABI) are used.

---

## 5. Decisions, and why the alternatives were rejected

**D1 — Keep the OpenGL render path.** Rejected: Vulkan / `gpu-next` (the package
ships `gpu-api=vulkan,auto`, but `meson.build` also gates `hwdec_cuda_gl.c` on
`cuda-interop` + `gl`, and the research evidence (§3.3) proved the GL path
works end to end);
a native child HWND (the documented reason the old design died — a native child
window composites above non-native siblings, which is why the dock kept
disappearing behind the video); a Qt Quick / `QQuickFramebufferObject` rewrite
(discards the painted chrome and the whole `QPainter` shell). Decision:
`VideoSurface` + `MpvRenderContext` stay exactly as they are.

**D2 — One explicit loader, one module, many handles.** Rejected: per-surface
`LoadLibrary` (Windows will not give you two independent copies of a module;
`GetProcAddress` would resolve to whichever loaded first, producing exactly the
mixed-core state this integration must not have); `PATH` manipulation plus static
imports (what the repository does today — it cannot switch core at runtime and
cannot validate a bundle); `AddDllDirectory` into an **external ambient** DLL
directory as a production mechanism (process-global mutable state pointing at
another project's tree, used in research only). A retained `AddDllDirectory`
cookie that points **inside the validated owned bundle** is permitted — that is
the owned closure, not ambient state. Decision: §8.

**D3 — Own the mpv patch set.** Rejected: vendoring the release binary and
living with `aji_current_log()` scraping (no schema, no stability); upstreaming
each change (this repository must be able to build and audit its own fork;
upstreaming is a distribution decision, not an engineering one). Decision: pinned
fork + patch manifest under `native/` (`M03`, `M04`).

**D4 — Structured additive snapshot export, version-negotiated.** Rejected:
parsing `aji_current_log()` (a human log with no schema — explicitly forbidden);
extending `m_property` (unnecessary: `GET_META` already exists, §4.2); bumping
the C ABI version 8 wholesale (would force every consumer and the bundle to move
in lockstep for an additive feature). Decision: keep ABI-8 layouts, add one
additive export whose payload carries its own protocol version, negotiated at
load time, with **no legacy log fallback**. The payload — not out-parameters — is
the authoritative surface, and it is a full state snapshot even for a 1:1
spatial chain where `scale_factor` alone would say nothing useful.

**D5 — Content-addressed prepared artifacts, explicit preparation.** Rejected:
letting the filter build on first play (a multi-minute `trtexec` inside the frame
path — the reference logs "first play at this resolution"); the reference
CRC32-basename cache (§4.5); adaptive partial chains (a chain silently missing
stage 3 is indistinguishable from a chain that never had one). Decision: every
stage prepared, atomically, before activation; on a cold cache the picture stays
**Original** with a visible `NeedsPreparation` state.

**D6 — QtCore-only modules, no framework.** Rejected: a new plugin/registry
system (the app has one player and one session; a framework is dead weight);
preparation on the GUI thread (engine builds take minutes); using `QProcess` for
playback (the player is in-process by design). Decision: five small modules
(§6.1), preparation on a dedicated worker with a process-wide lease, **no GPU
build while a live inference is running**.

**D7 — Generic ONNX acceptance by declared I/O, not by network whitelist.**
Rejected: a hardcoded architecture list (would reject every valid graph and
require code edits per model); a Python re-export step (introduces Python into a
repository whose rules forbid it). Decision: any strongly-typed graph with one
input and one output of a supported type and a supported spatial relationship is
accepted, including the Cyte-style SD graph already present in the package.

**D8 — Preserve, never repurpose, existing surfaces.** Decision: new preferences
group `player/native` v1, separate from `player/anime4k` and
`player/presetShortcuts`; a requested tick that persists when unavailable, with
requested and effective shown separately; a new Processing section inside the
existing `SettingsDialog`; a native branch under the existing sparkle tree; no new
API, no new CLI, no new service.

**D9 — DirectML is capability-gated, not promised.** CUDA↔Qt interop is proven;
**WGL↔D3D11 is not**, and nothing in the CUDA proof implies it. The DirectML
backend in the reference tree does carry RIFE code paths, but no DirectML RIFE
support is claimed here. Decision: the provider row appears only after a real
provider-identity + GPU-rendering proof on the target machine (`M48`);
otherwise it is visible-disabled with the factual reason.

---

## 6. Modules, files and typed contracts

Everything in §6 marked **PROPOSED** does not exist yet. Existing files are
named for the change that will touch them.

**Every default, sample value, key name, schema string and JSON example in §6 is
a PROPOSED schema for this repository.** None of it is a claim about what any
existing component — the mpv fork, `aji.dll`, a backend, or a model file — will
actually emit. Values shown here define what *this* implementation will produce
and consume; where the native side later differs, the native side is right and
this section is amended, not the other way round.

### 6.1 Proposed module layout

| Proposed path | Responsibility | Qt deps |
|---|---|---|
| `src/player/native/NativeCatalog.{hpp,cpp}` | Model identity, roles, capability matrix, user model inventory | Core |
| `src/player/native/NativeRecipe.{hpp,cpp}` | Typed recipe: ordered spatial stages, RIFE stage, geometry validation | Core |
| `src/player/native/NativePreparation.{hpp,cpp}` | `PreparationPlan`, `EngineArtifact` identity, manifest read/write, ownership guards | Core |
| `src/player/native/NativeBuilder.{hpp,cpp}` | Cancellable preparation jobs, process-wide lease, atomic publish | Core |
| `src/player/native/NativeStatus.{hpp,cpp}` | Snapshot schema, parse/validate, revision handling, HUD projection | Core |
| `src/player/native/NativeController.{hpp,cpp}` | Requested vs effective state machine, epochs, activation transaction, PiP handoff | Core |
| `src/player/MpvApi.{hpp,cpp}` | The single process-wide `MpvApi` loader and bundle descriptor | Core |

`native/` (repository root, **PROPOSED**) holds the build and packaging inputs:
source and patch manifests, the native CMake entry, the bundle manifest
template, the rights/provenance record and notes. None of it is on the Qt build
path; the Qt build reads only the installed bundle.

### 6.2 Model identity and capability contract

```cpp
enum class NativeModelRole { Spatial, Temporal };
enum class NativeModelOrigin { Builtin, UserImported };
enum class NativeSpatialRelation { Identity, Upscale, Downscale, Reframe };

struct NativeModelIdentity {
    QString          id;              // stable: content-addressed for user models
    QString          displayName;
    NativeModelOrigin origin;
    QString          onnxPath;        // read-only; never inside a cache dir
    QString          onnxSha256;      // required; a basename alone is forbidden
    qint64           onnxBytes;
    NativeModelRole  role;
    QString          ioType;          // observed; "fp16" is the only eligible one
    NativeSpatialRelation relation;    // learned from the actual graph
    // Absent (nullopt) while the graph is dynamic. A dynamic graph's geometry is
    // NOT known at import time, so recording a 1x1 placeholder would fabricate a
    // known geometry; it stays unknown until the real stage geometry resolves it.
    std::optional<QSize> sourceGeometry;
    std::optional<QSize> outputGeometry;
    QStringList      availableBackends;   // subset of {"tensorrt","directml"}
    QString          termsNote;       // human-readable; from the catalog record
    QString          contentId;       // graph content identity, see below
};
```

* **Two identities, kept separate.** `contentId` is the **graph content
  identity**: `sha256` over model role, ONNX SHA-256, and the *actual* declared
  I/O — rank, N, C, dtype, and whatever geometry is statically known. Two ONNX
  files with the same basename get different ids; editing an ONNX in place
  changes its id and therefore invalidates every artifact built from it. The
  **artifact build identity** (§6.5, `buildKey`) is a different, larger hash
  that additionally covers the GPU, precision, TF32 flag, provider/runtime
  versions and build options. A graph's content identity must never depend on
  which machine or provider will run it.
* Spatial 1:1 and scaled graphs are classified from the **actual graph geometry**
  reported by inspection, never from a filename prefix.
* Built-in catalog rows carry id, display name, bytes SHA, terms/attribution,
  dtype, role, available backends and the real profile. Curated
  Quality / Balanced / Performance entries are honest reference profiles (which
  built-in models, which geometry targets, which RIFE defaults), **not invented
  quality ratings**.

### 6.3 Graph inspection and boundary rules

Inspection is a C++ step with no Python anywhere. Two acceptable
implementations; the choice must be recorded in `native/`:

* **(a)** reuse the native ORT C API the DirectML backend already depends on, to
  read declared input/output metadata and run a CPU reference; or
* **(b)** use the ONNX SDK inspector directly.

(a) adds no new third-party dependency; (b) does. The boundary rules are the
same either way:

* Exactly one input and exactly one output. Extra inputs (a mask or second
  input) or extra outputs are **rejected**, not consumed — the bridge validates
  the exact I/O count, dtype and rank rather than trusting "the last binding",
  which is the weakness in the reference `load_engine` that picks input/output
  names by iterating `getNbIOTensors()`.
* **Only `float16` I/O is eligible today.** The original TensorRT and ORT
  staging is FP16 NCHW, and the bridge binds FP16 buffers. A `float32` or
  `bfloat16` graph is therefore recorded in the inventory as **unsupported,
  with that explicit reason**, until a real FP32 binding-and-cast bridge is both
  implemented **and** proved. It is **never** accepted with an automatic
  converter or an on-the-fly re-export, and the hint is **not** "re-export as
  FP32" — the FP16 bridge *expects* FP16 I/O, so telling a user to re-export to
  FP32 would push them away from the only supported dtype. The refusal names
  the observed dtype and says FP16 I/O is required.
* The eligibility rule is about **dtype and shape**, never about architecture:
  the generic role stays fully architecture-independent and accepts any
  architecture whose declared boundary satisfies these rules.
* A fixed graph must be **rank 4, N=1, C=3, NCHW on both input and output**.
  A dynamic graph must declare the same rank, N and C with dynamic H and W.
* The spatial relation must be a **uniform positive integer scale, including
  identity (scale 1)**. A graph whose declared output is not an exact uniform
  multiple of its input is refused with the observed geometry quoted in the
  error.
* Dynamic H/W are resolved at the **actual stage geometry** for the source being
  played. There is no special-cased `dyn-HW` filename form and no filename-based
  classification of any kind.
* RIFE graphs are a **separate kind**: named RIFE v4.x models with an explicit
  `num`/`den`, order (before or after the spatial chain), full / lite / ensemble,
  and scene-detection threshold. No RIFE-shaped graph is ever silently treated as
  a spatial stage, and no spatial model is ever used as a stand-in for RIFE.

### 6.4 Recipe contract

```cpp
struct SpatialStage {
    // A recipe names MODELS ONLY. It carries no artifact or engine identity:
    // which build a stage resolves to is decided later, by joining the stage's
    // model against the prepared-artifact set on the build key (§6.5).
    QString        modelId;         // NativeModelIdentity::id
    QSize          preResize;       // per-model work size before upscale
    bool           preResizeEnabled;
};

struct TemporalStage {
    // Models only, for the same reason as SpatialStage.
    QString        modelId;         // rife_v4.14 / _lite / _ensemble
    int            num = 1, den = 1;
    TemporalOrder  order = TemporalOrder::AfterSpatial;
    double         sceneThreshold = 0.150;
};

struct NativeRecipe {
    QString                      id;          // stable preference key
    QString                      label;
    QVector<SpatialStage>        spatial;     // ordered, may be empty
    std::optional<TemporalStage> temporal;     // may be empty
    NativeBackend                backend = NativeBackend::TensorRT;
    QString                      preparedPlanId;   // set on activation
};
```

Validation invariants:

* `spatial.isEmpty() && temporal.has_value()` is **RIFE-only** and is legal.
* `!spatial.isEmpty() && !temporal.has_value()` is **spatial-only** and is legal.
* Both non-empty is a **combined** recipe; `order` selects RIFE-before-spatial or
  RIFE-after-spatial.
* **`spatial.isEmpty() && !temporal.has_value()` is invalid** and must be
  rejected. In particular a spatial-only recipe that resolved to *zero* stages
  must **never** reach the core as a configure with nothing to do: that is the
  "RIFE-only wrongly configured as spatial zero" failure, and the filter's
  `passthrough` default would silently turn the whole feature into a no-op while
  the UI still showed it enabled.
* A selected recipe is **never** silently substituted: no swapped chain, no
  swapped model, no dropped RIFE stage. If it cannot be prepared as specified,
  the effective state stays Original with a visible reason.
* A recipe carries **no artifact identity**. A stage names a model; which
  prepared build that stage resolves to is decided by joining it against the
  prepared-artifact set on the **build key** (§6.5), with `engineSha256` as the
  separate post-serialization integrity value.

### 6.5 Preparation plan

```cpp
struct PreparationPlan {
    QString        id;                 // sha256 over the canonical plan body
    SourceGeometry source;              // w, h, fps rational, csp, range
    // Reported by the PROBE pass (§6.11), never guessed and never defaulted to
    // device 0. Empty until a probe snapshot has supplied them.
    QString        decoderDeviceUuid;   // the ACTUAL decoder device
    QString        computeDeviceUuid;
    QString        adapterLuid;        // D3D adapter LUID when DirectML is in play
    int            smMajor, smMinor;
    QVector<ArtifactSpec> artifacts;    // per stage, in execution order
    QVector<QString>    unknowns;       // cleared after inspection
    QVector<QString>    blockers;       // hard stops
    QVector<PreResize>  resizes;        // per-model pre-resize from the recipe
};

struct ArtifactSpec {
    QString role;                 // "spatial" | "temporal"
    QString modelContentId;
    QString stageInputShape;      // "1x3x1080x1920", padded
    QString stageOutputShape;
    QString ioFormats;            // "fp16:chw"
    QString buildOptions;         // canonical; part of the identity
    QString buildKey;            // pre-build identity (hashed request)
    QString engineSha256;       // post-serialization integrity (product)
};
```

Two distinct identifiers, computed at two distinct times:

* **`buildKey` (pre-build identity).** Hashed from: ONNX content SHA-256, model
  role, padded input/output shapes, input/output types, precision and TF32 flag,
  provider/plugin/runtime versions, the core source SHA, the inference ABI
  version, CUDA/driver versions, GPU UUID + SM, and the canonical build option
  string. This is known **before** any engine exists and is the lookup key that
  decides whether a rebuild is needed.
* **`engineSha256` (post-serialization integrity).** The SHA-256 of the
  serialized engine bytes, computed **after** `trtexec` writes the engine. It is
  stored in the manifest alongside `buildKey`.

**Observed producer boundary (M15):** the Release in-process helper builds a plan
through the TensorRT builder API (no `--trtexec`, no child process) on an
explicitly named CUDA UUID. The actual-builder probe passes **seven** behaviours
— existing-output preservation, corrupt-graph failure, absent-device refusal,
`build-started` before the build, the TF32-off/strongly-typed precision gate,
mid-build cancellation publishing nothing, and unchanged prior
artifacts/source bytes; its preserved pre-change predecessor fails only the
precision gate. The seven checks and the raw serialization log are recorded
under M15 in `tasks.md` and in `.probe-out/prep-flags-contract-probe.log`
(PASS) versus `.probe-out/prep-flags-contract-before.log` (6 ok, 1 FAIL). These
bytes remain unverified scratch: no numerical/binding verification,
artifact-manifest publication, complete preparation or Native activation is
claimed by this result.

There is no circularity: `buildKey` identifies the *request*, `engineSha256`
verifies the *product*. A manifest is written only once both exist, and
**deserialization is never attempted before the manifest has been validated**;
an unverified raw engine found on disk cannot enter automatic use. Building
necessarily serializes an engine before the manifest is published — the guard is
on *deserialization*, not on the build step.

Geometry resolution (`M13`) follows the **actual decoded source** — width, height,
fps rational, colourspace and range as libmpv reports them — not the URL and not
the container's nominal values. Padding matches `setup_rife`: to a multiple of
64, centred, rounded down to even. Orientation, crop and CSP tags follow the
reference behaviour rather than being reinterpreted.

### 6.6 Cache, manifest and ownership

```json
{
  "schema": "mikai.native.manifest/1",
  "buildKey": "sha256:…",
  "engineSha256": "…",
  "modelContentId": "sha256:…",
  "onnxSha256": "…",
  "stage": { "role": "spatial", "input": "1x3x1088x1920", "output": "1x3x2176x3840",
             "io": "fp16:chw", "padL": 4, "padT": 4 },
  "build": { "provider": "tensorrt", "runtime": "11.3.0.99",
             "stronglyTyped": true, "tacticSources": "-CUDNN,-CUBLAS,-CUBLAS_LT",
             "skipInference": true, "options": "--stronglyTyped --optShapes=…" },
  "device": { "uuid": "GPU-…", "sm": "120", "driver": "616.92", "cuda": "…" },
  "engine": { "file": "…engine", "sha256": "…", "bytes": 4194304 },
  "source": { "coreSha": "63d127174d8f70336c82a22408929e2bc301edcb",
              "inferenceAbi": 8 },
  "createdAt": "2026-10-01T00:00:00Z",
  "mikaiFormat": 1
}
```

**One hierarchy, one location.** Everything the app owns lives under
`QStandardPaths::AppLocalDataLocation()/native/`, in exactly this tree:

```
<QStandardPaths::AppLocalDataLocation()>/native/
  runtime/        installed runtime bundles (Standard and Native)
  models/         imported/builtin model files the app manages
  artifacts/      prepared engines + their manifests
  scratch/        per-job build scratch, never published
  logs/           preparation and native diagnostics
```

On Windows `QStandardPaths::AppLocalDataLocation()` resolves under
`%LOCALAPPDATA%` (the same API, and it is deliberately **not**
`AppDataLocation()`, which is the roaming variant). The same path is written in
every document — §6.6, §8, §9.2 — so there is exactly one hierarchy.

Rules:

* **Every one of those five roots is app-owned and writable.** **External** model
  directories and any borrowed reference tree are read-only inputs: never
  written, never cleaned, never used as a cache.
* Ownership guard before any delete: the file must live under an app-owned
  artifacts root, must have a manifest, must not be a reparse point or junction
  escaping the root, and its recorded size and hash must match. Canonicalized
  path comparison is case-insensitive on Windows.
* Scratch is per-job and removed on cancel or crash recovery; a partially written
  engine is never published.
* Bundle installation is atomic: build into a versioned directory, verify the
  manifest, then flip a pointer. A half-installed bundle is never selected.

### 6.7 Status snapshot contract

**Where the export lives.** The structured snapshot is a new **additive C export
on the native provider dispatcher `aji.dll`** (and forwarded to the active
backend `aji_trt`/`aji_dml`), not on `libmpv-2.dll`. The Qt `MpvApi` (§8.1)
resolves only the standard client/render API and never links the native symbol.
The bridge is one hop inside the filter: the owned `vf_animejanai` filter calls
the dispatcher snapshot export during `GET_META` handling and packs the result
into `mp_tags`, which the existing upstream `player/command.c` route already
exposes as the `vf-metadata/<label>` property (§4.2). The Qt side reads that
property and never touches `aji` symbols.

**Transport — one coherent choice.** `mp_tags` is a **flat map of string keys to
string values**. It cannot carry nested objects, and the dispatcher cannot emit
real numbers through it. So the payload is exactly two layers:

* flat string tags `schema`, `protocol`, `revision`, and `snapshot`;
* the `snapshot` tag holds **one JSON document as a string** with the fully
  typed payload below.

Qt parses that string once (`QJsonDocument::fromJson`) into typed
`NativeStatus` fields; validation rejects a schema or protocol it does not know.
Every scalar in the inner document is a real JSON type (numbers, booleans,
strings) — the "everything is a string" constraint applies only to the outer tag
map, not to the document inside `snapshot`.

Payload example (spatial-only recipe, 24 fps source, no interpolation):

```json
{
  "schema": "mikai.native.status/1",
  "protocol": 1,
  "revision": 412,
  "state": "Active",
  "epoch": 7,
  "requested": { "recipe": "janai.spatial.hd2x.v1", "backend": "tensorrt" },
  "effective": {
    "recipe": "janai.spatial.hd2x.v1",
    "provider": "tensorrt",
    "model": "2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16",
    "spatialStages": 1,
    "interpolation": null,
    "order": "spatial-then-temporal",
    "sourceSize": { "w": 1920, "h": 1080 },
    "outputSize": { "w": 3840, "h": 2160 },
    "sourceFps": { "num": 24, "den": 1 },
    "outputFps": { "num": 24, "den": 1 },
    "decoder": "nvdec/cuda",
    "csp": "bt709/limited"
  },
  "counters": { "framesIn": 3100, "framesOut": 3100,
                "dropped": 0, "interpolated": 0, "sceneCuts": 0 },
  "reason": ""
}
```

`state` admits `Off`, `Probe`, `Preparing`, `NeedsPreparation`, `Active`,
`Suspended`, `Failed`, `Unsupported` and `Deactivated`. `interpolation` is
`null` for a spatial-only recipe or an object containing its rational `num`,
`den` and model id. `reason` is empty only for `Active`.

The counters are self-consistent by construction: a **spatial-only** recipe
keeps `outputFps == sourceFps`, `framesOut == framesIn`, and `interpolated == 0`.
A **combined** RIFE 2× recipe reports `outputFps = 2 × sourceFps`,
`framesOut ≈ 2 × framesIn`, and `interpolated ≈ framesIn` (each source pair
yields one extra frame, scene cuts excluded). The plan never mixes the two
shapes in one example.

`revision` increments on every semantic change; the app polls at a bounded
500 ms **only** while native is active, preparing or switching — never from
`paintGL`, never per frame — and drops any snapshot whose `revision` did not
advance. The snapshot reports **requested and effective separately**, so a
requested-but-not-applied choice is visible as such. `reason` is non-empty
exactly when the state is not `Active`, and carries a stable machine key plus a
translatable English/Ukrainian message. **No property-change notification is
assumed**: the read is an explicit async `mpv_get_property_async` of
`vf-metadata/mikai_native` as `MPV_FORMAT_NODE`, completed on
`MPV_EVENT_GET_PROPERTY_REPLY`. The node in that reply is **borrowed** — mpv
owns it until the next `mpv_wait_event` — so the handler deep-copies the strings
and numbers out of it and **does not** call `mpv_free_node_contents` on that
borrowed node. `mpv_free_node_contents` is used only for a node **libmpv
allocated**: the `mpv_node` a synchronous `mpv_get_property` fills, or one
allocated by `mpv_event_to_node`. A node the **app** allocated is freed with the
**app's** allocator — mpv's contract is explicit that a self-built node must not
go through `mpv_free_node_contents`. The same allocator split applies to any
command result.

### 6.8 Preferences contract

Additive group, versioned, never touching an existing key:

```
[player/native]
schema=1
enabled=true                     # the requested tick; persists when unavailable
backend=tensorrt
recipe=janai.spatial.hd2x.v1
customModelsDir=                 # optional, read-only, validated on read
[player/native/shortcuts]
toggleProcessing=Ctrl+Shift+J
```

Load order: parse → schema check → per-key validation → fall back to the default
for that key only, logging the reason. An unreadable or corrupt native block
never blocks startup and never clears `player/anime4k`,
`player/presetShortcuts`, the quality cache, feeds or the language. Persisting a
native preference never triggers an API cache reset or a feed refresh
(`MainWindow::openSettings`).

### 6.9 State lifecycle

```
Requested(Off) ──user toggles on──▶ Requested(recipe)
Requested(recipe) ──prep needed──▶ Preparing        [picture stays Original]
Preparing ──ready──▶ PublishReady
Preparing ──cancel/fail──▶ NeedsPreparation | Failed
PublishReady ──ack from filter──▶ Active
Active ──switch──▶ Switching (drain, apply, re-anchor)
Switching ──ack──▶ Active(new epoch)
Active ──PiP parked──▶ Suspended   (drained; intermediates released; widget kept)
Suspended ──resume──▶ Active
any ──source change / media replace──▶ re-plan (recipe choice preserved)
Off: no vf in the chain; effective plan = Deactivated, never Active
```

* Requested and effective are separate fields everywhere, including the HUD.
* Turning native off removes the filter from the chain (`vf` slot 0 bypass) so
  toggling does not rebuild a chain that would then be inactive; the status
  reports `Deactivated`, never `Active`.
* The controller owns a monotonic **revision** per (request, source, core
  handle). Any worker completion or status snapshot with a stale revision is
  dropped on arrival.

### 6.10 Activation transaction

1. Record play/pause, mute, volume, speed, sub and audio track ids, and the
   current PTS anchor.
2. Pause; wait for the filter's pending tickets to drain (bounded, with a
   timeout that falls back to `Failed` rather than hanging).
3. Set the named filter plan on the core through the loader-resolved API (§7.1).
4. Apply the decoder compatibility override **if required** (§7.3).
5. Re-anchor the clock so the picture resumes at the same PTS; interpolate the
   first frame pair rather than stalling.
6. Restore playback state, audio, subtitles and speed.
7. Only after the filter acknowledges the new **epoch** does the controller
   promote `requested` to `effective` and update the HUD.

If any step fails the transaction rolls back to the previous epoch, the picture
returns to Original, and the reason is shown. **The HUD never shows the requested
state as if it were applied.**

### 6.11 Probe-only passthrough — how the real device identity is obtained

**The problem.** `buildKey` needs the *actual* decoder `CUdeviceptr` device UUID
and, for DirectML, the D3D adapter LUID. Public `video-params` properties do not
supply either: mpv exposes dimensions, fps and colourspace, never the underlying
adapter identity. Preparation therefore cannot guess, and **device 0 is never an
acceptable default** — a machine with two adapters would build engines against
the wrong one.

**Native Off installs no filter at all.** The default state is Native Off with
no `@mikai_native` in the chain — no probe, no engine, nothing. The filter is
attached only after a user **Prepare** or **Enable** action.

**The mechanism.** The owned filter gets an explicit **probe-only passthrough
mode** for exactly that action. In this mode the filter:

* is attached as `@mikai_native` after the user's action;
* performs **no** `aji_engine` configure, **no** deserialization, **no** build,
  **no** NN and **no** RIFE work;
* passes the **original frames through unchanged**;
* reports, from the **filter thread**, the real `AVHWFramesContext` device
  UUID/LUID, the decode geometry, the CSP and the range it actually observed.

**Threading.** No CUDA calls and no hardware-context pointers ever cross the Qt
thread. The filter observes on its own thread and publishes the **string**
identity in its `GET_META` snapshot; the Qt side reads only that string. The
preparation helpers are then built against the GPU that was *reported*, never
against an assumption.

**Acquiring a frame.** If the source is paused and no frame has been seen, the
source coordinator may request **one** frame at the **existing PTS** through the
existing async source mechanism, preserving the paused state. A **non-seekable**
source waits for the next real frame or reports `Unknown` — the identity is never
guessed. Playback and the UI stay **Original** throughout probing and preparation.

**Epoch invalidation.** A change of context, device or cadence invalidates the
plan and forces a re-probe for the new epoch; a stale device UUID never drives a
build.

**State and admission.** The backend `C` status reached through `GET_META`
supports a `Probe` state and is **never** `Active` in this mode. The sequence is:
probe admits the device and geometry → **all** stages are prepared → a
transactional switch to the ready NN chain. Turning native off removes the probe
filter only after its acknowledgement.

**Ownership and honesty.** The ABI capability list and the probe protocol are
defined in `M09`; the filter implementation is `M19`/`M22`; the source-aware
plan in `M13` consumes the validated snapshot and can be built and unit-tested
with fixture inputs **before** the producer exists. **No full runtime proof is
claimed until `M19`/`M22` are done.** Nothing here is already implemented.

---

## 7. Runtime, decoder and rendering semantics

### 7.1 Filter invocation and the `@mikai_native` label

The fork filter is invoked by name with a per-session parameter set. Because
Windows paths contain `:` and mpv splits on `:`, the safest construction is the
**native command-node path** (`mpv_command_node` with an options object), which
avoids quoting entirely. If a `vf=` string is used instead it must be
`@mikai_native:animejanai=<k>=<v>:…` with every value `%`-escaped, where the
escape count is over **UTF-8 bytes** (a multi-byte character is more than one
`%` triplet), and colons, commas and drive letters must never leak through.
**No shell string interpolation, ever.** No option is ever built by `QProcess`,
and no `mpv.conf` or input conf is written.

### 7.2 Process-wide work lease

One lease per process. While a preparation job holds it, no other preparation
runs and — critically — **no engine build is started while a live core is doing
inference**. A build during active playback would contend for the GPU and make
both the build and the frame path unpredictable. Preparation is therefore
allowed while the picture shows **Original**, never while an active chain runs.

### 7.3 Decoder and device compatibility

* The native path consumes GPU frames directly: CUDA for TensorRT, D3D11 for
  DirectML. It therefore **may require NVDEC**; a software-decoded source can
  still work by uploading once, and the status labels that case truthfully
  (`software decode + GPU upload`), never claiming NVDEC.
* The inference device is the **actual** `AVHWFramesContext` device from the
  decoder, not device 0. Where an override is needed, the existing
  `hwdec=auto-safe` default is overridden **for that session only** and reverted
  on teardown.
* HTTP headers, user agent, `keepaspect`, volume and the input-binding options
  from §2.4 are preserved verbatim across every native change.
* The full-frame GPU→CPU→GPU inference hot path is prohibited. A single upload
  for a software-decoded source is permitted and labelled.

### 7.4 Colour, range, chroma siting and CSP

The native ABI carries `matrix` (BT601/BT709/BT2020), `range` (limited/full) and
`siting` (left/center/topleft). These are taken from what libmpv reports for the
stream and passed through unchanged; nothing here reinterprets them, and
`output-444` handling is left to the fork's existing behaviour (enabled for the
CUDA path, per §3.3).

### 7.5 Temporal correctness, PTS and cadence

* The filter owns output PTS. Interpolated frames are placed at
  `prev.pts + (src.pts − prev.pts) · t`, the source grid point keeps `src.pts`,
  and `nominal_fps` becomes `in_fps · num / den` — the reference semantics
  (§4.1), preserved.
* **No new Qt-side frame pacing is introduced.** Pacing is the filter's job.
* Rational output rates are reported exactly (`24000/1001 → 48000/1001`), not
  rounded to floats.
* Scene cuts return `AJI_SCENE`; the reference behaviour of emitting a duplicate
  of `a` is preserved, so the output frame count stays continuous across a cut.
  Each such pair costs one device-to-host scalar copy and a synchronize (§4.6);
  that cost is stated in the docs, not hidden.
* A chain reconfigure (slot change, geometry change, model change) never
  interpolates across the boundary: `rife_prev` is cleared and the accumulator
  reset.
* `estimated-vf-fps` remains an mpv **measured output** indicator. It is not
  evidence that inference ran, and it is not the source work rate.

### 7.6 PiP, suspension and teardown

* Both surfaces run the **same effective recipe and the same Anime4K chain id**.
  On entry the float copies the recorded chain id (never a stale resolved path
  list) and the native plan *by id*, then resolves it against its own core.
* Exactly one core plays and exactly one core runs inference. The parked core's
  filter is **drained and suspended**, and its TensorRT/DirectML intermediates
  are released, while its `QOpenGLWidget` and its last displayed frame are kept
  alive — tearing the widget down while the main one is on screen is the
  documented `DrvPresentBuffers` driver fault.
* One module, two handles is the intended and supported shape. Each handle owns
  its **own** native epoch, CUDA context and output pool. **No execution
  context, stream or device allocation is ever shared across cores or threads.**
* Hand-back restores position, play/pause, mute, volume, speed, fullscreen and
  the actual native plan. PiP is never silently bypassed to "keep it simple".
* Close — including after PiP was left earlier — must stop the parked float's
  stream, cancel its native jobs, and drain and release its tickets and GPU
  resources **while its core and render context are still alive**, following the
  exact teardown order in §7.8 (detach callbacks → free the render context with
  GL current → terminate core). The hidden `pipPool_` path is the
  specific risk: today `leavePip` only hides and pauses the float and only
  `PipFloat::closeEvent` calls `closeStream()`, so a parked float keeps a live
  core and `~PlayerDialog` deletes `pip_` but never `pipPool_`. With the native
  core that pins a CUDA context and engine bindings. Widget-pool destruction is
  ordered against application shutdown, not a hot `delete`.
* Repeated open/close must not leak engine or core resources. **This is a
  requirement to be proved by a new native close probe (`M45`); it is not
  claimed as already fixed.**

### 7.7 Composition order with Anime4K

Fixed pipeline: **decode → native vf (RIFE in recipe order) → mpv `colorscale` →
Anime4K shader hook stages.** When a spatial stage is active, mpv is displaying an
image *larger* than the source, so the `//!WHEN OUTPUT.w MAIN.w / 1.200 > …` gates
are evaluated against the **actual post-native image**, measured against the
canvas. The result is recorded per preset; it is **not** asserted that every
Anime4K upscaler "does nothing" under native. An ungated restore stage still
runs, and no existing shader id, category, label or chain is modified.

### 7.8 Asynchronous native commands and status

**The problem, stated from source.** `VideoSurface::pumpEvents()` runs on a
50 ms `QTimer` and reaches `handleState()` and `updateStreamStats()`, which call
`MpvHandle::getNumber()` — `mpv_get_property(MPV_FORMAT_DOUBLE)` — for
`video-params/w`, `video-params/h`, `cache-speed`, `estimated-vf-fps` and
`container-fps`. Every one of those takes the mpv core lock, and the existing
comment in `handleState()` already says so: *"every mpv_get_property takes the
core lock against mpv's own render thread."* A native first configure, engine
deserialization or GPU drain can hold that lock for a long time **even though
none of that work runs on the Qt thread**. Leaving the new native commands and
status reads synchronous would therefore block the GUI timer behind GPU work and
invalidate every responsiveness claim in the [M49](./tasks.md#m49) gate.

**The rule.** Nothing native is applied or read synchronously from the GUI
thread. Specifically:

* The selected `MpvApi` (§8.1) exposes `mpv_command_async` and
  `mpv_get_property_async` in addition to the existing synchronous entries, and
  `MpvHandle` grows matching typed wrappers. The existing synchronous getters
  stay for the current per-timer source/control reads; they are simply never
  used for anything native.
* **Native apply is async.** Setting the filter plan, changing the epoch,
  draining and suspending all go out as `mpv_command_async` and complete through
  `MPV_EVENT_COMMAND_REPLY` inside `drainEvents()`.
* **Status reads are async.** The bounded 500 ms native poll issues
  `mpv_get_property_async("vf-metadata/mikai_native", MPV_FORMAT_NODE)` and
  completes on `MPV_EVENT_GET_PROPERTY_REPLY`. The node reached through that
  reply is **borrowed** — mpv owns it until the next `mpv_wait_event` — so the
  handler deep-copies the strings and numbers out of it and **never** calls
  `mpv_free_node_contents` on it. `mpv_free_node_contents` is reserved for a
  node **libmpv allocated** (a synchronous `mpv_get_property` result, or one
  from `mpv_event_to_node`); a node the **app** allocated is released with the
  **app's** allocator, never mpv's. The typed `NativeStatus` values are copied
  out **synchronously during event dispatch**, so nothing dangles after the next
  `mpv_wait_event`. **No query is ever issued from `paintGL`,**
  `renderGL`, `update()` or any repaint path.
* **Replies are correlated, not assumed.** Every async request carries a
  correlation key of (core handle, source identity, request epoch). A reply
  whose epoch is stale, or whose handle no longer exists, is dropped on arrival.
  This is the same stale-completion rule as §6.9, applied to the transport.

**A command reply is not activation.** `MPV_EVENT_COMMAND_REPLY` with success
only means the core accepted the command. The effective plan becomes `Active`
only when the `GET_META` snapshot reports the **new epoch** *and* the filter has
published its first post-activation output. The controller waits for that pair;
it never promotes `requested` to `effective` on the command reply alone.

**One state bank, not parallel copies.** The source and control properties the
UI needs (video params, pause, speed, track ids, cache-speed, estimated-vf-fps)
continue to be served by the existing `MpvHandle` observe mechanism, but the
observe primitive is **fixed rather than reused unchanged**: `observe()` today
registers with `MPV_FORMAT_NODE_ARRAY` while the dispatcher in `drainEvents()`
decodes only `MPV_FORMAT_DOUBLE`, `MPV_FORMAT_INT64`, `MPV_FORMAT_FLAG` and
`MPV_FORMAT_STRING`, and no caller registers an observer today, so the mismatch
is latent. Native work registers observers with the **correct format per
property** and coalesces deliveries into one UI update on the existing 50 ms
cadence. The native status snapshot and those observed properties merge into the
**same** controller state bank — there are no parallel state copies that can
disagree, and the HUD reads one bank.

**Shutdown ordering — exact sequence.** The render context is **not** freed
before the GPU work is drained; the correct order is:

1. `pause`.
2. Issue the drain / suspend / cancel **while the core and render context are
   still alive**, and await the acknowledgement (bounded).
3. Detach callbacks (`mpv_render_context_set_update_callback(ctx, nullptr,
   nullptr)` and the filter's event bindings) so nothing can fire into a dying
   object.
4. Free the render context **with the GL context still current**.
5. Terminate the core (`mpv_terminate_destroy`).

There is **no Qt-side GPU wait** — the GUI thread never blocks on a CUDA or D3D
operation — and no `QObject` worker or widget is destroyed from a native
callback thread; teardown is marshalled onto the owning thread in the existing
`QPointer`-guarded style.

**Honest limit.** A fatal driver fault or an in-process crash cannot be promised
to recover. The mitigations are `CrashLog` (already installed first in
`main()`) and drain/suspend-before-teardown ordering that avoids provoking the
documented `DrvPresentBuffers` fault. Full recovery from a hard GPU crash is
**not** claimed and is not provable on this channel.

Gates: the loader entry points and async wrappers are [M07](./tasks.md#m07); the
async status read and reply correlation are [M20](./tasks.md#m20); async apply
and the command-reply-is-not-activation rule are [M26](./tasks.md#m26);
drain/suspend/close acknowledgement ordering is [M30](./tasks.md#m30); and GUI
responsiveness while a recipe is active, while a first configure is in flight
and while a preparation job holds the lease is part of the
**[M49](./tasks.md#m49)** gate.

---

## 8. Controlled cutover of the loader

### 8.1 The loader

**PROPOSED** `src/player/MpvApi.{hpp,cpp}` owns:

```cpp
struct MpvBundle {
    QString      root;        // validated, absolute, canonical
    QString      modulePath;  // <root>/libmpv-2.dll
    QString      descriptorId;// sha256 over the bundle manifest
    int          protocolVersion;
    QStringList  deps;        // explicit dependency closure, all inside root
};

class MpvApi {                // the complete resolved entry points
public:
    static MpvApi& instance();          // process-wide, created once
    bool load(const MpvBundle&, QString* reason);
    bool loaded() const;
    const MpvBundle& bundle() const;
    // every mpv_* and mpv_render_context_* used by this repository,
    // including mpv_command_async / mpv_get_property_async (§7.8)
};
```

Rules:

* **Not** `MPV_CPLUGIN_DYNAMIC_SYM`. That SDK macro replaces the `mpv_*` entry
  points with function pointers that the **mpv host populates** for a C plugin
  it loads itself, for `mpv_open_cplugin`. It is not a general player-side DLL
  loader and it is not reused here; the loader in this section resolves the
  module's own exports directly.
* Absolute **`LoadLibraryExW`** (wide, UTF-16) with the safe-DLL-search flags
  (`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`), so
  Unicode user profiles and non-ASCII install paths work; the dependency
  directories are a **single validated flat bundle** or an explicitly listed
  **owned** closure, with retained `AddDllDirectory` cookies that point only
  inside that owned bundle. There is no global `PATH` pretence, and the
  environment is not mutated for the rest of the process. On the native side the
  same rule applies: `std::filesystem` with `u8path` and the Windows wide
  `...W` open calls, never a narrow open and never an ASCII temporary-path
  workaround (extends the `M40` UTF-8 trust boundary).
* The selected `HMODULE` is pinned past every core handle and render context;
  it is never freed while any handle lives.
* No hot module swaps. A runtime-choice change persists and **requires a
  restart**.
* Many handles are explicitly supported and expected (main player, PiP float,
  probes) — one module, many cores.
* The module path, resolved API, protocol version, supported metadata label and
  capability set are recorded at load and reported in the status surface.
* The loader exposes a free-function `mpv_render_context_create` that can only
  be called after a GL context is current; `MpvRenderContext` therefore keeps
  its `get_proc` trampoline and its current-context teardown ordering unchanged.

### 8.2 Startup order

1. `installCrashLog()`.
2. `QApplication`, application name/version/organization.
3. Locale resolution, translators, `setStyle(Fusion)`, `buildStylesheet()`.
4. **Runtime selection** (new): read `player/native/runtimeChoice`; validate the
   bundle descriptor; `MpvApi::load()`. A missing or incomplete native bundle
   never prevents home, plain player or Anime4K startup — it falls back to
   Standard with a visible `RuntimeUnavailable` + `NativeNotApplied` reason.
5. `MikaiClient`, `MainWindow`.

The existing stock discovery in `src/backend/Runtime.cpp` stays as the read-only
fallback for the Standard path. Its current behaviour — resolve the **latest**
`mpv-dev-x86_64-*.7z` release at runtime, download it, extract with 7-Zip,
stage into `%LOCALAPPDATA%/MPVBackend/runtime` — is **not** native bundle
provisioning and is **not** modified by this work beyond what the loader
prerequisite requires.

### 8.3 What is deleted in the cutover

Once `MpvHandle`, `MpvRenderContext`, the probes and `mikai_app` all go through
`MpvApi`:

* the `MPV_LIBRARY` import and its `PUBLIC` link on `mikai_player`;
* the old `MPV_RUNTIME_DIR` cache variable name, replaced by
  `MPV_INCLUDE_DIR` (SDK headers only) — `MPV_RUNTIME_DIR` is **deleted**, not
  aliased, with build docs and every call site updated;
* `provisionMpvRuntime()`'s `PATH` mutation in `src/app/main.cpp`.

There is **no** decorative loader left beside static imports, and no second code
path. `PlayerDialog::runtimeRequested` currently only re-shows the dialog
(`TitlePage.cpp:1593`); with provisioning in place it means *the runtime could
not be created*, and the visible message says which runtime is selected and what
is missing — provisioning is no longer a `show()` side effect.

### 8.4 Failure and recovery

| Condition | Behaviour |
|---|---|
| No bundle at all | Standard fallback; playback, Anime4K, home all work; native rows visible-disabled with the reason |
| Native selected but incomplete/corrupt | Standard fallback + `RuntimeUnavailable` + `NativeNotApplied`; never `Active`; never a mixed core with shims |
| Selected bundle loaded, then a partial ABI failure | Ask for a restart; never load a second module |
| Module not yet loaded and a same-descriptor recovery succeeds | Allowed (nothing has been loaded yet) |
| Runtime choice changed by the user | Persisted, applied on next start; the UI says so immediately |
| DLL search cannot resolve a dependency | Loader refuses the bundle and reports the missing module by name |

---

## 9. Independent native build, packaging, SDK and rights

### 9.1 Two toolchains, one repository

The Qt application keeps building with MinGW-w64 g++ / Qt 6.8 / CMake. The
native addon is a **separate, optional** build with **MSVC + nvcc** for C, C++17
and CUDA. Nothing in the default `cmake --build --preset app`
depends on CUDA, TensorRT, MSVC or nvcc; a machine without the native SDK
configures and builds the app unchanged.

**PROPOSED** `native/CMakeLists.txt` is an independent entry point
(`cmake -S native -B native/build`) that:

1. checks out (or consumes a vendored copy of) the pinned mpv fork at
   `63d127174d8f70336c82a22408929e2bc301edcb` and applies the patch series this
   repository owns;
2. configures the fork with `-Dcuda-hwaccel=enabled -Dcuda-interop=enabled` and
   GL enabled, per §4.1;
3. builds the inference libraries with
   `-DAJI_TRT_ROOT=<TensorRT root> -DAJI_NVINFER=nvinfer_11` for a TensorRT 11.3
   installation — **never** inferring the CUDA minor from the TensorRT version —
   and the documented `CMAKE_CUDA_ARCHITECTURES` list;
4. emits a bundle directory plus its manifest.

The two external trees (`C:/Users/myhli/Downloads/janai/animejanai-inference-main`
and the 3.6.3 package) are **read-only inputs**. No file in either is modified,
and neither is a runtime dependency of the shipped app.

### 9.2 Bundle layout and dependency closure

```
<bundleRoot>/
  bundle.json            # schema, coreSha, sourceSha, versions, file hashes
  libmpv-2.dll           # the fork core, native filter compiled in
  aji/                   # aji.dll + aji_trt.dll (+ aji_dml.dll when offered)
  trt/                   # nvinfer_*.dll, cudart, cudnn, cublas*, the CUDA redist
  ort/                   # onnxruntime.dll + DirectML.dll, DirectML track only
```

Rules:

* **Self-contained.** Every DLL the bundle needs is inside it. The research used
  `AddDllDirectory` into an existing TensorRT/torch-runtime tree; production does
  not. No Conda, no Torch, no Python, no system-wide CUDA requirement beyond the
  driver.
* **Atomic install.** Staged into `…/native/runtime/.staging-<id>/`, verified,
  then renamed; a partial install is never selected.
* **Standard and Native are separate bundles.** The native addon is never written
  into the existing `%LOCALAPPDATA%/MPVBackend/runtime` tree, and the stock
  discovery/download logic there is untouched.
* `bundle.json` lists every file with size and SHA-256; the loader refuses a
  bundle whose recorded hash does not match.

### 9.3 Rights and provenance — a blocking gate, not paperwork

`M03` blocks `M05` and every distribution step. Required before any binary is
vendored or shipped:

* licence and redistribution status for the mpv fork (GPL/LGPL obligations for
  mpv itself, and the status of the AnimeJaNai filter code in it);
* licence, terms and attribution for each shipped ONNX model, recorded per model
  in the catalog (`termsNote`);
* TensorRT redistribution terms and the NVIDIA acknowledgement file contents;
* ONNX Runtime and DirectML redistribution terms;
* a provenance record: core SHA, inference source SHA, patch series identity,
  build toolchain versions, and the file hashes.

Until that record exists, the §3.2 hashes stay descriptive research
observations and are **not** release pins.

The records live beside the code, not in this plan:

* [`native/manifest/sources.json`](./native/manifest/sources.json),
  [`native/manifest/components.json`](./native/manifest/components.json) and
  [`native/manifest/THIRD_PARTY_NOTICES.md`](./native/manifest/THIRD_PARTY_NOTICES.md)
  — pins, per-component licence/redistribution decisions and notices.
* [`native/sdk-acquisition.md`](./native/sdk-acquisition.md) — the isolated SDK
  compile/link/kernel proof (MSVC 19.44, CUDA 13.4.92, TensorRT 11.3.0.99).
* [`native/rife-acquisition.md`](./native/rife-acquisition.md),
  [`native/spatial-acquisition.md`](./native/spatial-acquisition.md) and
  [`native/videojanai-acquisition.md`](./native/videojanai-acquisition.md) —
  asset identity, the 128/128 weight-lineage match and the observed package
  digests.
* [`native/permission-request.md`](./native/permission-request.md) — the
  unsent third-party request; no permission is granted.
* [`native/README.md`](./native/README.md) — the component decisions and the
  build/packaging entry points.

---

## 10. Verification strategy

Every no-regression row (R1–R22) and every native acceptance row (N1–N10,
including N3b) is canonical in **[tasks.md §4](./tasks.md#catalog)** — the R rows
at [§4.3](./tasks.md#catalog-r) and the N rows at
[§4.4](./tasks.md#catalog-n). This plan keeps only the *strategy*: correctness,
GPU, lifecycle and release gates are **measurements on real frames with saved
artefacts**, never exit codes or mock echoes; the CPU matrix is the hermetic
regression channel; desktop/GPU acceptance requires an attached session; and no
offscreen-GPU proof exists on this machine.

---

## 11. Failure, recovery, threat and rollback matrix

| Failure | Detected by | Behaviour | Recovery | Rollback |
|---|---|---|---|---|
| No selected runtime | loader at startup | Standard fallback; `RuntimeUnavailable` visible | user installs/selects a bundle; restart | none needed |
| Native bundle incomplete | bundle hash verification | refuse; `NativeNotApplied`; playback unaffected | reinstall bundle | none needed |
| Module loaded, ABI mismatch | loader protocol negotiation | refuse + restart request; never a second module | install matching bundle | restore Standard choice |
| ONNX import rejected at boundary | graph inspection | refuse with the observed geometry/type and a concrete fix | fix or re-export the model | previous recipe stays |
| Engine build fails | builder exit + log | picture stays **Original**, `Failed` with reason; no partial chain | cancel, fix, retry preparation | previous ready artifacts remain valid |
| Cancel during build | builder | scratch removed; lock released | new attempt | previous artifacts intact |
| Artifact corrupt on load | manifest SHA mismatch | refuse to deserialize; `Failed` | rebuild that artifact | previous artifact restored |
| Cache poisoned by a foreign `.engine` | ownership guard (app root, manifest, canonical path) | never loaded, never deleted if not owned | user re-runs preparation | untouched |
| Filter reports a mid-run error | status snapshot `reason` | drop to Original, show reason; **no** media retry loop | retry processing | previous epoch restored |
| Scene cut / RIFE discontinuity | filter `AJI_SCENE` | duplicate-of-`a`, continuous output timeline | automatic | none |
| Reconfigure drain timeout | controller epoch timeout | rollback to previous epoch, `Failed` | re-request | previous epoch intact |
| PiP parked core resource growth | native close probe / memory counters | drain + suspend + release on park and on close | reopen | pool destruction ordered at shutdown |
| Status poll stalls | 500 ms bounded poll, revision unchanged | HUD shows last known state with a staleness note | next revision | none |
| DirectML provider missing or misidentified | capability probe | provider row visible-disabled with the factual reason | install a working provider | stay on TensorRT |

### Threats and mitigations

| Threat | Mitigation |
|---|---|
| Loading a DLL from an untrusted directory | absolute path, safe search flags, validated flat bundle, per-file hashes |
| Path traversal via a user-supplied model directory | canonicalized path check; read-only; never a cache; ownership guard before delete |
| Reparse point / junction escaping an owned root | reject reparse points in ownership checks |
| Untrusted ONNX parsed by the inspector | inspect in the isolated preparation process; refuse unknown operators rather than guessing |
| Command injection through filter options | command-node construction or `%`-escaped UTF-8-byte values; never a shell string |
| Token or key leakage into logs, manifests or diagnostics | URL tokens and API keys never enter native prefs, manifests, logs or exported diagnostics; native inference receives only media metadata libmpv already provides |
| Resource exhaustion from a runaway build | process-wide lease, cancel, scratch cleanup, bounded timeouts |
| Deleting a user file | only app-owned artifacts with a valid manifest are ever removable |

### Rollback

Native is **additive**. Rolling back means: set the runtime choice back to
Standard, restart. Nothing in `player/anime4k`, `player/presetShortcuts`, the
quality cache, feeds or language is touched, the native addon folder can be
deleted independently, and the stock `%LOCALAPPDATA%/MPVBackend/runtime` path is
untouched throughout.

---

## 12. Workstreams (navigation)

The 52 packages are grouped into eight cohesive workstreams, defined canonically
in **[tasks.md §1.1](./tasks.md#workstreams)**. This is a navigation aid only —
the `Depends` lines and the critical path in [tasks.md §1](./tasks.md#dag) are
authoritative, and no matrix or arrow is restated here.

| WS | Theme | Tasks |
| --- | --- | --- |
| A | Runtime packaging and ABI | [M01](./tasks.md#m01)–[M10](./tasks.md#m10) |
| B | Models, ordered preparation and cache | [M11](./tasks.md#m11)–[M18](./tasks.md#m18) |
| C | Inference, metadata and filters | [M19](./tasks.md#m19)–[M25](./tasks.md#m25) |
| D | Activation, source, PiP and lifetime | [M26](./tasks.md#m26)–[M30](./tasks.md#m30) |
| E | Player UI, shaders and settings | [M31](./tasks.md#m31)–[M38](./tasks.md#m38) |
| F | Media recovery and trust | [M39](./tasks.md#m39)–[M40](./tasks.md#m40) |
| G | End-to-end correctness, fidelity and performance | [M41](./tasks.md#m41)–[M49](./tasks.md#m49) |
| H | Distribution, docs and release | [M50](./tasks.md#m50)–[M52](./tasks.md#m52) |

The next runnable batches are [tasks.md §1.2](./tasks.md#batches).

### Proof work packages, one link each

| Task | What it must prove | Key constraint |
| --- | --- | --- |
| [M42](./tasks.md#m42) | fresh plain-player and native-off desktop regressions | rebuilds `widget_pixel_probe`; re-tests the `INVALID_ENUM` observation |
| [M43](./tasks.md#m43) | CUDA inference reaches the actual Qt framebuffer; paused Prepare reports the real device UUID/LUID for a seekable source or a valid cached frame | pixels from `grabFramebuffer()`; stock-core control; no exit-code-only proof; a non-seekable source without a frame stays Unknown + Original, without unpausing or guessing a device |
| [M44](./tasks.md#m44) | RIFE timing, cuts, padding, seeks, end-of-stream | real video; rational output counts; A/V sync across a cut |
| [M45](./tasks.md#m45) | native PiP, Compare, close and resource lifetimes | two cores, 100 cycles, GPU/engine resource counters, close-after-leave |
| [M46](./tasks.md#m46) | native local/HTTP/HLS and anime session transitions | hermetic `QTcpServer`; session prefs preserved; no live CDN required |
| [M47](./tasks.md#m47) | preparation and artifact failure recovery | cold cache, corrupt artifact, cancel, build failure, missing model |
| [M48](./tasks.md#m48) | DirectML provider identity and GPU rendering where offered | real proof or a visible-disabled row with a factual reason |
| [M49](./tasks.md#m49) | sustained processing, responsiveness and resource stability | matched standalone Janai/mpv throughput/latency, warm-up, cadence, memory, full-frame transfers and synchronization traces |
| [M50](./tasks.md#m50) | independent clean install, upgrade and rollback | no Python, no Conda, offline baseline |

The native acceptance rows N1–N10 (including N3b) are canonical in
[tasks.md §4.4](./tasks.md#catalog-n).

---

## 13. Status and unresolved prerequisites

Status is per task, not a blanket claim. The canonical status summary, the three
current root blockers (initial native snapshot, curated RIFE pin→content
mapping, RIFE full/ensemble numerics) and the current evidence live in
**[tasks.md §1.6](./tasks.md#status)** and in each task's `Status` line. This
plan does not keep a second progress ledger.

---

## 14. Standing constraints

These are durable and checked at every task; the full acceptance catalog is in
[tasks.md §4](./tasks.md#catalog).

- [ ] No dependency on another application's installation or mutable state — not
  on `F:/ZKS/Interp/AnimeInterp`, not on Python, not on the Janai package's
  config, cache or assets; pinned owned third-party SDKs remain permitted.
- [ ] No application code changed outside the files a task names.
- [ ] No claimed-but-unmeasured number; every measurement cites how it was taken.
- [ ] Every failure path lands on Original with a visible, specific reason —
  never a silent substitution and never a media retry loop.
- [ ] The RIFE placeholder becomes a real capability subtree; the RealESRGAN and
  other-models placeholders stay untouched.
- [ ] No test asserts wording, enum ordinals, incidental defaults, mock echoes
  or bare non-emptiness.
- [ ] Nothing native is applied or read synchronously on the GUI thread; no
  `vf-metadata` query from `paintGL`; a command reply alone never means `Active`
  (§7.8).
- [ ] Teardown runs pause → drain/suspend/cancel ack (core and render context
  still alive) → detach callbacks → free the render context with GL current →
  terminate core; the render context is never freed before the GPU drain; no
  Qt-side GPU wait and no widget or worker destroyed from a native callback
  thread.
- [ ] Native status and observed source/control properties live in one state
  bank; async replies are correlated by handle, source and request epoch and
  stale ones are dropped.
- [ ] Device identity for `buildKey` comes only from the probe-only passthrough
  snapshot (§6.11) — never guessed, never defaulted to device 0; probe state is
  never `Active`; no CUDA call or hardware-context pointer crosses the Qt thread.
- [ ] Every native storage path resolves to the single
  `AppLocalDataLocation()/native/{runtime,models,assets,artifacts,recipes,scratch,logs,components}`
  hierarchy (§6.6). The set grew as the work did - `assets` and `recipes` with
  the model stores, `components` with the per-machine GPU component packs - and
  the check is the containment itself, not the count.
- [ ] No feature is deferred, narrowed or renamed; DirectML ships
  disabled-with-reason when unproven, never as a claim.
