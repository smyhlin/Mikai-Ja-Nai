# Architecture

Mikai Ja Nai is a C++20 desktop application organized around Qt 6.12 Widgets and a desktop OpenGL playback surface. Mikai is the upstream catalogue service; its client, the application UI, player/render path and optional Native processing support are separate subsystems.

This document is an orientation, not a claim that optional components are installed or supported on every machine.

## Main layers

- `src/backend/` contains public API models/client, release/update data, provider stream extraction, downloads, runtime discovery and diagnostic scrubbing, plus the shared HTTP transport, cancellation scope and failure type the API and provider paths both use. `MikaiClient` speaks to `https://api.mikai.me/public/v1`; it is an unofficial client of the public API, not an owner-controlled service component. Failures carry the backend's code and HTTP status, so a surface can say why something failed instead of only that it did.
- `src/app/` owns application navigation, title/search pages, settings, local watch history (`state/WatchHistory`: per-episode resume offsets and lengths), home feed selection (`state/Feeds`), the TTL-bounded cache of extracted stream URLs (`state/QualityCache`), UI state, appearance (`state/AppearanceStore`), translations, background jobs and component-store UI. Widgets publish results back to the UI; nothing here blocks the GUI thread on backend work.
  - `pages/TitlePagePlayerSession.cpp` owns one player session for a title page. It opens the dialog on the chosen provider variant and routes the work that belongs to the page rather than the player: stream recovery, MoonAnime skip segments, dub/sub variant switches, quality probes and runtime resolution. A source change supersedes the session, so late replies from the old provider are discarded instead of retargeting the new one.
  - Appearance reaches the player as pushed tokens rather than imports: `shell::setAccent` and `shell::setCornerRadius` are seeded at startup and refreshed on a customization change or save, so the player library never includes application state.
  - `MainWindow` owns the shared component provisioner and Components tab across Settings visits. The tab and Settings keep borrowed service references as `QPointer`: QObject sibling destruction can retire the service before the tab, which must not call a freed provisioner during shutdown. Destroying the tab still cancels a live service. `state/NativeComponentProvisioner` provisions the GPU-specific builder components a prepared bundle is missing, once per process; `state/NativePlaybackAssembly` turns what a playing core observed into the admission request it submits, and never builds or reports an engine as loaded.
- `src/app/workers/Runner` coordinates off-thread work on five pools that exist because the work has five shapes: the shared pool bounds concurrent API calls, the bulk pool fans out one task per home card so a just-opened title is not queued behind the feed, the stream pool carries latency-critical stream extraction that holds its thread across two GETs, the image pool carries artwork (which bypasses the API rate limiter and costs no quota), and the transfer pool carries component and model downloads. Destroying a Runner never waits: it cancels work, drops queued completions behind a gate the owner's destruction sets, and leaves pool retirement to the last reference on the GUI event queue. `shutdownAll()` is the explicit asynchronous drain for application exit.
- `src/gui/` holds the platform integration shared by widgets and the player: idle inhibition while any surface is playing (`IdleInhibitor`), optional default-player registration (Windows Default Apps, XDG desktop entries for Startup and VideoPlayer), the drag-and-drop video target, device-pixel-aware image decoding and the `MIKAI_UI_PROFILE` presentation profiler. Registration is an explicit user action and never changes an existing default silently.
- `src/player/core/` owns dynamic libmpv loading, handles, render-context access, media tracks and playback diagnostics. On Linux, packaged builds select the bundled runtime instead of the host's `libmpv.so.2`.
  - The fork the player and AnimeJaNai are built against is loaded from a prefix-relative directory: `bin/mpv` in portable trees and `/usr/lib/mikai/mpv` in packages.
  - `MpvHandle` names the host's CA bundle as `tls-ca-file` unless the caller chose one, because bundled OpenSSL's compiled-in paths belong to the build distribution.
- `src/player/shell/` provides the player UI, controls (including the `EpisodeNavButton` stepper and footer HDR/Dolby/codec badges), the floating picture-in-picture window (`PipFloat`), hover previews (`FramePreviewService`), the media inspector and local-file handling. In-source chrome paints inside the surface pass with antialiasing, `SmoothPixmapTransform` and device-pixel-ratio scaling, and positions glyphs on integer pixels.
- `src/player/shaders/` contains Anime4K shader selection. `src/player/compare/` owns frozen-frame candidate rendering, compare modes and flicker export.
- `src/player/native/` holds the AnimeJaNai component/runtime contract (`components`: acquisition, admission, plans, build-log interpretation), prepared Native bundle discovery/selection (`bundle`, `prepare`, `status`), model metadata and optional-runtime support. The GUI-free support code is separated from widgets. AnimeJaNai Standard processing and the prepared Native bundle workflow are distinct; neither is an alias for the other's configuration or runtime.
- `native/` contains optional Native source/build tooling, manifests and patches. It is not a requirement for ordinary Original playback.
- `tests/` and `src/probes/` contain automated tests and purpose-specific probes; see [building](building.md#tests-and-probes) for safe use.

The executable entry point is `src/app/main.cpp`; the root `CMakeLists.txt` builds `mikai_backend`, `mikai_native_support`, `mikai_player` and `mikai_app` as static libraries and links them into the `mikai` executable. Startup establishes identity and settings, applies the interface language, pushes accent/corner-radius tokens into the player library, and seeds the process-wide HDR and display-scaling policies before any surface exists. `--version`, `-V` prints the commit and build stamp before a `QApplication` is constructed, so it answers "did my update apply" without a desktop session.

The public API contract is documented in [Mikai's API reference](https://api.mikai.me/public/docs); build, deployment and legal details are in the [build](building.md) and [release](releasing.md) guides.

## UI flow

`MainWindow` is a stack: one `SearchPage` (home feeds, search results, team browsing, filters and the recently-opened carousel) and at most one `TitlePage`. Opening a title disposes the previous one, which saves the quality cache and closes every player it opened; a related title pushes onto a small history so Back walks the chain instead of dropping to search. Opening an episode hands off to `TitlePagePlayerSession`, which owns the dialog, its episode queue and the page-side work described above. Settings (with its component store), the media inspector and Compare are dialogs over that stack rather than additional pages.

## API and state boundaries

The API client sends an application/version identity (`Mikai-Wrapper/0.1.0` for this release) rather than impersonating a browser. API keys are optional, remain local configuration and are not compiled into source.

The client keeps a bounded in-memory JSON cache keyed by request URL and credential generation. Fresh cache directives can satisfy a request; stale entries can be revalidated with ETag, and `304` reuses the cached body. `no-store`, errors and designated endpoints are not retained. The client's own quota estimate before headers arrive is 299 requests/minute without a key and 599 with one; the API's rate-limit and retry headers remain authoritative, and cached requests do not change server-side semantics. A credential change starts a new generation, so a reply from a superseded key's window cannot describe the new key's quota.

Recent updates are paged and merged before publishing a new watermark. Local watch history and recently opened titles are application settings and are not synchronized to the Mikai service; extracted stream URLs live in a separate TTL-bounded cache because provider URLs are signed and expire, and a provider with no video for an episode is remembered for the page lifetime only. Provider stream extraction is a separate provider-facing transport path. For API details, endpoint schemas and authentication, use the [Public API reference](https://api.mikai.me/public/docs) rather than inferring them from this client description.

Application settings retain the QSettings identity `Mikai` / `Mikai Wrapper`. Native/model data resolves to `MIKAI_NATIVE_ROOT`, else an executable-adjacent `native/` that accepts a write, else `AppLocalDataLocation/native`; logs and crash reports are written beside the executable when writable, otherwise they use Qt's `AppLocalDataLocation`.

On start the app removes only abandoned work from that store (`scratch/` and `*.previous` older than a day; `aji-stage-*` and `*.partial` older than an hour). It never removes an installed component, model or configuration. Builds, tests and package staging must use owned directories and must not reuse user state as disposable output.

Episode range pagers use height-for-width and wrap their caption group instead of imposing its one-line minimum on the page. Title-page grids derive their column cap from the scroll viewport and the tab style's `CT_TabWidget` sizing contract, not the grid's previous allocation or pane paint geometry. Hidden provider pages must not retain a wider grid minimum after the viewport shrinks.

## Playback and rendering

Playback uses Qt Widgets with `QOpenGLWidget` and libmpv's `mpv_render_context`; it does not use Qt Quick's RHI. The render surface loads libmpv dynamically (`LoadLibraryExW` on Windows, `dlopen` on Linux). GUI playback commands are asynchronous, and native update callbacks queue/coalesce work to the GUI thread.

The rendering contract is intentionally specific:

- Every `paintGL()` fills Qt's default framebuffer at physical-pixel dimensions, including paused HUD repaints. A pass that owes a new frame renders it; a chrome-only pass blits the last presented picture. `NoPartialUpdate` is required; skipping the pass would leave the surface stale.
- Render updates happen with the surface OpenGL context current. The render parameter that blocks for target presentation time is passed as integer zero; the GUI thread must not sleep to wait for presentation.
- The event-pump drain is bounded (128 events or 4 ms) rather than consuming an unbounded backlog on the UI thread.
- Hidden/minimized/covered surfaces consume updates without rendering in `paintGL()`. A temporarily unpaintable surface gets a bounded retry through the existing presentation timer before entering skip mode; it must not depend on another mpv callback to release the held frame. This can reduce unnecessary presentation work but does not guarantee decoding or inference is free of GPU cost.
- Both on-screen rendering and transition-frame capture establish a Qt native-painting boundary before issuing raw OpenGL commands. Transition captures clear their own target and contain video only, not HUD pixels. They are taken at processing transitions, not copied continuously during ordinary playback.
- Framebuffer format and channel-depth caches are invalidated when the render context or widget framebuffer is replaced. A new target must not inherit the previous target's dithering/depth parameters.
- Coded source dimensions remain separate from display geometry. Processing and source-resolution labels use `video-params/w,h`; the windowed canvas uses aspect-corrected `video-out-params/dw,dh` and its remaining rotation.
- A chrome-only repaint does not disarm an armed presentation slot or consume the frame that slot is waiting to present. It draws the last presented picture plus the cached chrome. The render-lead sample is closed in the swap callback, so it includes composition and the vsync wait. Whether that cuts paused-hover paint time by 30 percent against the 490 µs baseline is not yet measured.
- Display scaling is a process-wide policy with the same revision pattern as HDR tone mapping: Fit, Fill, Stretch, 16:9, 4:3, original size, zoom, upscaler and downscaler. Each mode change writes all seven mpv options (`keepaspect`, `panscan`, `video-aspect-override`, `video-unscaled`, `video-zoom`, `scale`, `dscale`). The newest asynchronous batch requests a forced redraw after every accepted write has replied, so a paused surface cannot keep its pre-change snapshot. Auto downscale restores `option-info/dscale/default-value` after the core is initialized and never sends the literal `auto`. Compare takes a Fit lock for the capture and releases it before playback resumes; picture-in-picture follows the display policy.
- Picture-in-picture hands playback to a floating window (`PipFloat`), and the dialog hides itself while the float owns the core. The surface that owns playback (the float's while PiP is up, the main window's otherwise) is the one whose position, pause state and presentation are reported, so a PiP hand-off does not fabricate an episode-finished edge.
- Renderer cleanup runs with the correct context on destruction; a recreated context reloads the current source at its observed position/pause state. Closing during a native build bounds the GUI settle at two seconds, shows the finishing notice after 300 ms, and continues the close if the stop has not acknowledged. The native stop is not given a new worker that touches widgets.
- The process asks for a desktop OpenGL 3.3 core context before `QApplication` exists. Qt on Wayland otherwise produced a GLES 3.2 context, and mpv's render API inherited it: every Anime4K chain is desktop GLSL and corrupted colour there. Setting it once at the context level keeps every preset on the profile its shaders target.

HDR sources are tone-mapped to SDR by one process-wide policy. Auto uses `bt.2390` with peak computation and automatic gamut mapping, Stable keeps `bt.2390` but disables peak computation and clips to the target gamut, and Vivid uses a Möbius curve with a fixed parameter. Each policy writes its option set into every core at creation and again on change; an option a runtime refuses is logged while playback continues. This is source-to-SDR tone mapping, not native HDR output, and the footer badges report what the container declares rather than what the display can show.

On-video controls paint in the OpenGL pass while retaining their existing widget input/focus/accessibility behavior. Frame exports and comparison inputs use the video framebuffer to exclude HUD chrome; ordinary widget framebuffer grabs include the HUD.

`hwdec=auto-safe` requests a safe hardware decode path; it is not a universal zero-copy guarantee. GPU transfers, drivers, codecs, decode format and optional filters determine actual resource use. See Qt's [QOpenGLWidget](https://doc.qt.io/qt-6/qopenglwidget.html) and mpv's [render API](https://github.com/mpv-player/mpv/blob/master/include/mpv/render.h) documentation.

Keyboard navigation and the **+85** control send asynchronous `relative+exact` seeks to mpv; the timeline, resume workflow and MoonAnime segment-skip buttons use absolute targets. Explicit precision preserves the old absolute seek accuracy: mpv's default relative seek is keyframe-oriented, unlike its default absolute seek. The UI may show an optimistic position, but it cannot clamp or derive a backend-relative step from a delayed progress observation. Both seek kinds emit the same argument-free intent edge so Compare invalidates a pending capture before the resulting position update arrives.

MoonAnime skip metadata is fetched on the stream worker for the selected episode and provider. The title-page session discards stale responses after a source change; valid ranges drive both timeline bands and individually actionable skip buttons, including overlapping segments.

Timeline hover previews use a separate, lazily created libmpv core on a worker thread, not the active playback core. Requests are coalesced and stale results are discarded when the pointer or source changes. Decoding requires a seekable source; the preview is not a capture of the active player's processing pipeline.

### Comparative player-source review

The local IINA Windows candidate uses `QQuickFramebufferObject`; the local Nami candidate uses mpv's D3D11 composition swapchain inside WinUI, not the libmpv render API. Nami's native-window erase hook and delayed swapchain resizing are specific to that topology and are not Qt Widgets fixes. The IINA candidate also has weaker context teardown and callback/timing handling than the existing player, so neither renderer is adopted wholesale.

Both candidate source trees declare GPLv3 licensing. No candidate source is copied into this Unlicense application; the integrations implement documented Qt/mpv contracts independently. The useful retained contracts are bounded handling of temporarily suppressed paints, explicit GL-state ownership, target-lifetime-aware framebuffer metadata, and distinct coded/display geometry.

These changes do not establish that every intermittent black frame is resolved. Destroying a libmpv render context still disables its video output and requires the existing source-reload path. A GPU framebuffer cannot survive its context's destruction. Automatic HDR passthrough and WinUI-style background snapshots are not enabled: their compositor assumptions do not match this player's cross-platform Qt surface.

The Haruna review also followed its external MpvQt renderer. The supplied Haruna tree does not pin an MpvQt version; the inspected upstream implementation uses Qt Quick framebuffers and mpv's default, non-advanced render API mode. Its lack of advanced-control updates and swap reports is not evidence that those calls should be removed from this player. Haruna's backend-relative exact keyboard seeks informed the asynchronous navigation fix; its blocking pause-on-drag writes and KDE/Qt Quick UI dependencies were not adopted.

The bundled mpv revision was checked when comparing render invalidation: changing `glsl-shaders` updates the GPU renderer's options without discarding libmpv's cached frame, while video-filter and hardware-decoder reconfiguration can discard it. The existing paused shader-refresh path is preserved; a blanket redraw on `vo-configured=false` is not used as a black-frame fix.

## Optional enhancement paths

Original playback is the default and does not require model components. Anime4K runs as OpenGL shader processing. AnimeJaNai runs through the pinned fork's filter on the external AJI stack, with presets and custom profiles selected explicitly. The official v0.22.2 dispatcher has no version-query export, so its pinned dispatcher/backend hashes establish its ABI-9 identity. A custom import must either match the pinned pair or report ABI 9 and provide a compatible backend; a mismatch is refused by name. Prepared Native processing is a separately verified bundle track whose private ABI-8 producer builds and admits only the TensorRT and DirectML backends it carries; a selection or manifest token it cannot honour is refused rather than downgraded. Preparation and inspection tools are documented in [`native/README.md`](../native/README.md) and [`native/build-msvc.md`](../native/build-msvc.md).

An AnimeJaNai plan is refused while the source colour metadata is unknown or HDR, unless the separate native-on-HDR override is enabled: the stages force 8-bit NV12, so the block is reported as "Skipped on HDR" or "Skipped: source transfer metadata is unknown" rather than run with a substitute format. The gate re-evaluates on a source change instead of assuming the previous source's colour profile.

Settings' component store manages optional downloads into application-owned locations and verifies downloaded content before installation. Imported ONNX model structure, registration metadata and a ready UI state do not prove runtime/operator/backend/GPU compatibility. TensorRT engines are device/runtime-specific. Mixed ONNX/Anime4K stages may negotiate through software NV12 and incur host/GPU transfers; do not treat them as zero-copy or HDR-preserving.

Backends are not interchangeable, and the plan code treats them as separate contracts:

- On Windows the store can additionally install the `aji-trtrtx-v0.22.2` TensorRT for RTX overlay. It is a distinct runtime from classic TensorRT: it builds engines in process and keeps its own engine and JIT caches instead of receiving `trtexec` options, and a backend substitution the dispatcher reports is surfaced as a refusal, never as a successful RTX run. Upstream publishes no Linux RTX asset, so Linux keeps classic TensorRT; the TensorRT SDK component is pinned to `11.3.0.99` with CUDA `13.4`.
- TensorRT-family plans decode through `nvdec` and DirectML plans through `d3d11`, because that is the format each backend consumes. A software decoder breaks that supply path.
- On Linux the fork prefixes its `trtexec` invocation with `LD_LIBRARY_PATH=<trtexec-libdir>` and hands that line to `/bin/sh` unquoted, so a store path containing a space splits it before TensorRT starts. The plan therefore passes a space-free mirror of the verified library closure when the store path is splittable. The surface also preloads that closure `RTLD_GLOBAL` before attaching the filter, because the backend resolves `libnvinfer` by bare soname and a plain failure otherwise surfaces as an opaque inference-context error.

## Compare and measurements

Compare captures a player frame and renders selected candidates for visual inspection; it is not a second playback pipeline. Difference processing occurs in Difference mode. A flicker export alternates the two settled images as a GIF, an animated WebP or a silent MP4 clip. The GIF and animated WebP paths letterbox and use a fixed 256-colour palette, so palette quantization makes those formats unsuitable as a bit-exact image-quality test. Exports run on a worker and are cancellable.

Settings → Benchmark measures selected workloads through the real player/render path, captures new output frames and records environment, geometry and model IDs to JSON. TensorRT compilation is reported separately from sampling; failures remain failures rather than being converted into zero/low performance. [Benchmark evidence](../bench_results.md) contains historical captures with workload limitations; synthetic black-frame fixtures do not establish real-scene playback speed or visual quality.

## Runtime, packaging and licensing boundaries

- Runtime resolution happens at load time: a packaged Linux build uses the pinned private fork under `/usr/lib/mikai/mpv`, a portable tree uses `bin/mpv`, and both are preferred over the host's `libmpv.so.2`. Windows uses the pinned DEV archive's `libmpv-2.dll`; the archive's separate CLI/updater sibling is not bundled. Pinned identities live in [`native/manifest`](../native/manifest) and [`packaging/provenance`](../packaging/provenance); a mismatch is a build or release defect, not something the running app repairs.
- `packaging/` holds the platform profiles (`windows`, `linux`, `arch`, `debian`, CI inputs), licence texts, provenance records and `third-party.json`. Both platform profiles keep `publicReleaseReady: false`, so a local package is not evidence of rights clearance. Packaging must stage into owned directories: user state is never disposable build output.
- The application source is [Unlicense](../LICENSE), which does not relicense third-party code, models or runtime packages. The bundled mpv fork is GPL-family by default and LGPL-2.1-or-later only in a `-Dgpl=false` build; AnimeJaNai, TensorRT, DirectML and the model packs carry their own terms, and the AnimeJaNai inference repository itself publishes no licence file. [Third-party notices](../native/manifest/THIRD_PARTY_NOTICES.md) and [redistribution evidence](redistribution.md) record the obligations; the catalogue and provider content remain the service's and the providers'.

## Repository references

- [Quick start & catalogue](user-guide.md)
- [Player & Compare](player-guide.md)
- [Processing & benchmarks](processing-guide.md)
- [Settings & privacy](settings-guide.md)
- [Build guide](building.md)
- [Release and redistribution guide](releasing.md)
- [API reference](https://api.mikai.me/public/docs)
- [Native build docs](../native/README.md) and [MSVC notes](../native/build-msvc.md)
- [Third-party notices](../native/manifest/THIRD_PARTY_NOTICES.md) and [license](../LICENSE)
