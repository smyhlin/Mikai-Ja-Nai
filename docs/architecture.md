# Architecture

Mikai Ja Nai is a C++20 desktop application organized around Qt 6.12 Widgets and a desktop OpenGL playback surface. Mikai is the upstream catalogue service; its client, the application UI, player/render path and optional Native processing support are separate subsystems.

This document is an orientation, not a claim that optional components are installed or supported on every machine.

## Main layers

- `src/backend/` contains public API models/client, release/update data, provider stream extraction, downloads, runtime discovery and diagnostic scrubbing. `MikaiClient` speaks to `https://api.mikai.me/public/v1`; it is an unofficial client of the public API, not an owner-controlled service component.
- `src/app/` owns application navigation, title/search pages, settings, local watch history (`state/WatchHistory`: per-episode resume offsets and lengths), UI state, appearance (`state/AppearanceStore`), translations, background jobs and component-store UI. Worker activity is coordinated through `Runner`; widgets publish results back to the UI.
  - Appearance reaches the player as pushed tokens rather than imports: `shell::setAccent` and `shell::setCornerRadius` are seeded at startup and refreshed on a customization change or save, so the player library never includes application state.
  - `MainWindow` owns the shared component provisioner and Components tab across Settings visits. The tab and Settings keep borrowed service references as `QPointer`: QObject sibling destruction can retire the service before the tab, which must not call a freed provisioner during shutdown. Destroying the tab still cancels a live service.
- `src/player/core/` owns dynamic libmpv loading, handles, render-context access, media tracks and playback diagnostics. On Linux, packaged builds select the bundled runtime instead of the host's `libmpv.so.2`.
  - The fork the player and AnimeJaNai are built against is loaded from a prefix-relative directory: `bin/mpv` in portable trees and `/usr/lib/mikai/mpv` in packages.
  - `MpvHandle` names the host's CA bundle as `tls-ca-file` unless the caller chose one, because bundled OpenSSL's compiled-in paths belong to the build distribution.
- `src/player/shell/` provides the player UI, controls (including the `EpisodeNavButton` stepper and footer HDR/Dolby/codec badges), picture-in-picture, media inspector and local-file handling. In-source chrome paints inside the surface pass with antialiasing, `SmoothPixmapTransform` and device-pixel-ratio scaling, and positions glyphs on integer pixels.
- `src/player/shaders/` contains Anime4K shader selection. `src/player/compare/` owns frozen-frame candidate rendering, compare modes and GIF export.
- `src/player/native/` holds prepared Native bundle discovery/selection, component/model metadata, planning and optional-runtime support. The GUI-free support code is separated from widgets. AnimeJaNai Standard processing and the prepared Native bundle workflow are distinct; neither is an alias for the other's configuration or runtime.
- `native/` contains optional Native source/build tooling, manifests and patches. It is not a requirement for ordinary Original playback.
- `tests/` and `src/probes/` contain automated tests and purpose-specific probes; see [building](building.md#tests-and-probes) for safe use.

The executable entry point is `src/app/main.cpp`; CMake composes backend, native support, player and app targets in the root `CMakeLists.txt`.

The public API contract is documented in [docs.md](../docs.md); build, deployment and legal details are in the [build](building.md) and [release](releasing.md) guides.

## API and state boundaries

The API client sends an application/version identity (`Mikai-Wrapper/0.1.0` for this release) rather than impersonating a browser. API keys are optional, remain local configuration and are not compiled into source.

The client keeps a bounded in-memory JSON cache keyed by request URL and credential generation. Fresh cache directives can satisfy a request; stale entries can be revalidated with ETag, and `304` reuses the cached body. `no-store`, errors and designated endpoints are not retained. The API's rate-limit and retry headers remain authoritative; cached requests do not change server-side semantics.

Recent updates are paged and merged before publishing a new watermark. Local watch history and recently opened titles are application settings and are not synchronized to the Mikai service. Provider stream extraction is a separate provider-facing transport path. For API details, endpoint schemas and authentication, use the [Public API reference](../docs.md) rather than inferring them from this client description.

Application settings retain the QSettings identity `Mikai` / `Mikai Wrapper`. Native/model data resolves to `MIKAI_NATIVE_ROOT`, else an executable-adjacent `native/` that accepts a write, else `AppLocalDataLocation/native`; logs and crash reports are written beside the executable when writable, otherwise they use Qt's `AppLocalDataLocation`.

On start the app removes only abandoned work from that store (`scratch/` and `*.previous` older than a day; `aji-stage-*` and `*.partial` older than an hour). It never removes an installed component, model or configuration. Builds, tests and package staging must use owned directories and must not reuse user state as disposable output.

## Playback and rendering

Playback uses Qt Widgets with `QOpenGLWidget` and libmpv's `mpv_render_context`; it does not use Qt Quick's RHI. The render surface loads libmpv dynamically (`LoadLibraryExW` on Windows, `dlopen` on Linux). GUI playback commands are asynchronous, and native update callbacks queue/coalesce work to the GUI thread.

The rendering contract is intentionally specific:

- Every `paintGL()` redraws video into Qt's default framebuffer at physical-pixel dimensions, including paused HUD repaints. `NoPartialUpdate` is required; rendering only on newly decoded frames would leave the surface stale after UI-only repaints.
- Render updates happen with the surface OpenGL context current. The render parameter that blocks for target presentation time is passed as integer zero; the GUI thread must not sleep to wait for presentation.
- The event-pump drain is bounded (128 events or 4 ms) rather than consuming an unbounded backlog on the UI thread.
- Hidden/minimized/covered surfaces consume updates without rendering in `paintGL()`. A temporarily unpaintable surface gets a bounded retry through the existing presentation timer before entering skip mode; it must not depend on another mpv callback to release the held frame. This can reduce unnecessary presentation work but does not guarantee decoding or inference is free of GPU cost.
- Both on-screen rendering and transition-frame capture establish a Qt native-painting boundary before issuing raw OpenGL commands. Transition captures clear their own target and contain video only, not HUD pixels. They are taken at processing transitions, not copied continuously during ordinary playback.
- Framebuffer format and channel-depth caches are invalidated when the render context or widget framebuffer is replaced. A new target must not inherit the previous target's dithering/depth parameters.
- Coded source dimensions remain separate from display geometry. Processing and source-resolution labels use `video-params/w,h`; the windowed canvas uses aspect-corrected `video-out-params/dw,dh` and its remaining rotation.
- Renderer cleanup runs with the correct context on destruction; a recreated context reloads the current source at its observed position/pause state.
- The process asks for a desktop OpenGL 3.3 core context before `QApplication` exists. Qt on Wayland otherwise produced a GLES 3.2 context, and mpv's render API inherited it: every Anime4K chain is desktop GLSL and corrupted colour there. Setting it once at the context level keeps every preset on the profile its shaders target.

On-video controls paint in the OpenGL pass while retaining their existing widget input/focus/accessibility behavior. Frame exports and comparison inputs use the video framebuffer to exclude HUD chrome; ordinary widget framebuffer grabs include the HUD.

`hwdec=auto-safe` requests a safe hardware decode path; it is not a universal zero-copy guarantee. GPU transfers, drivers, codecs, decode format and optional filters determine actual resource use. See Qt's [QOpenGLWidget](https://doc.qt.io/qt-6/qopenglwidget.html) and mpv's [render API](https://github.com/mpv-player/mpv/blob/master/include/mpv/render.h) documentation.

Keyboard and skip-button navigation sends asynchronous `relative+exact` seeks to mpv; the timeline and resume workflow retain absolute targets. Explicit precision preserves the old absolute seek accuracy: mpv's default relative seek is keyframe-oriented, unlike its default absolute seek. The UI may show an optimistic position, but it cannot clamp or derive a backend-relative step from a delayed progress observation. Both seek kinds emit the same argument-free intent edge so Compare invalidates a pending capture before the resulting position update arrives.

## Optional enhancement paths

Original playback is the default and does not require model components. Anime4K runs as OpenGL shader processing. AnimeJaNai uses the external AJI stack and separately acquired runtime/model content; its presets and custom profiles are selected explicitly. Prepared Native processing is a separately verified bundle track, with preparation/inspection tools documented in [`native/README.md`](../native/README.md) and [`native/build-msvc.md`](../native/build-msvc.md).

Settings' component store manages optional downloads into application-owned locations and verifies downloaded content before installation. Imported ONNX model structure, registration metadata and a ready UI state do not prove runtime/operator/backend/GPU compatibility. TensorRT engines are device/runtime-specific. Mixed ONNX/Anime4K stages may negotiate through software NV12 and incur host/GPU transfers; do not treat them as zero-copy or HDR-preserving.

## Compare and measurements

Compare captures a player frame and renders selected candidates for visual inspection; it is not a second playback pipeline. Difference processing occurs in Difference mode. Flicker GIF export uses two settled images, letterboxing and a fixed 256-colour palette, so palette quantization makes it unsuitable as a bit-exact image-quality test.

Settings → Benchmark measures selected workloads through the real player/render path, captures new output frames and records environment, geometry and model IDs to JSON. TensorRT compilation is reported separately from sampling; failures remain failures rather than being converted into zero/low performance. [Benchmark evidence](../bench_results.md) contains historical captures with workload limitations; synthetic black-frame fixtures do not establish real-scene playback speed or visual quality.

## Repository references

- [Quick start & catalogue](user-guide.md)
- [Player & Compare](player-guide.md)
- [Processing & benchmarks](processing-guide.md)
- [Settings & privacy](settings-guide.md)
- [Build guide](building.md)
- [Release and redistribution guide](releasing.md)
- [API reference](../docs.md)
- [Native build docs](../native/README.md) and [MSVC notes](../native/build-msvc.md)
- [Third-party notices](../native/manifest/THIRD_PARTY_NOTICES.md) and [license](../LICENSE)
