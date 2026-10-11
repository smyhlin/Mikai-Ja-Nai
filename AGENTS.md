# Repository Guidelines

## Project Overview

C++/Qt6 desktop client for the Mikai public anime API
(`https://api.mikai.me/public/v1`). One process, one Qt event loop: the app is
`build/release/mikai.exe` (via `CMakePresets.json`), built by the root `CMakeLists.txt`. Home feed with
carousels (recently opened / ongoing / popular) plus updates and schedule rows;
search with a format filter bar; a title page with detail, related titles,
per-release tabs, and a compact episode grid where a click fetches that
episode's qualities once and opens the built-in player. Settings carries four tabs — General, Presets, Processing and **Components** —
and the Components tab lists the pinned GPU-specific TensorRT packs, five
AnimeJaNai HD/SD upscaling graphs and RIFE model archive with install state,
digest verification and explicit user-initiated downloads. The spatial pack
is separate from the backend and RIFE packs; built-in profiles need its SD
graph for low-resolution HLS. Checkboxes select acquisition, not installed
state. Closing Settings retries only a settled unavailable AnimeJaNai request;
component installs persist after Cancel/X, while unsaved preferences do not.
Public Aji component downloads remain pinned to
upstream v0.9.0; a locally imported custom ABI-8 package is separate and is not
the default download source. `docs.md` is the authoritative API
contract; `.env` holds the optional key; `skills/design-ui/` carries OS-native UI
guidance. `cmake/dist.cmake` assembles a self-contained folder behind the `dist`
target; it never ships the downloadable elements.

## Architecture & Data Flow

Layering is strict and enforced by tests:

- `src/backend/` is the domain: `Models.hpp` structs (snake_case members, free
  `parse*` functions, `QString` helpers with empty = none), `Errors.hpp`
  (`MikaiApiError{code, message, status}`), `Releases.hpp`
  (`playableEpisodeCount`, `sortReleases`: voice → sub, most playable, team
  name), `Streams.hpp` (HLS extraction per site: `detectSite`, `fetchText` with
  20 s timeout and 2 retries honoring `Retry-After`, `ashdiMaster`,
  `moonMaster`, `tortugaDecode`, `tortugaMaster`, `parseVariants`,
  `qualityUrl`, `qualityStreams`; `NoSourceError` marks a provider with no
  video). The layering tests (`TestLayering`) assert it includes no Qt GUI
  headers and never names the app layer.
- `src/app/` owns the application widgets and talks to the domain through
  `mikai::backend::MikaiClient` (blocking calls; threads via `Runner`).
  Shared GUI helpers live in `src/gui/`, including `Clipboard.hpp` in
  `mikai::gui`; player widgets remain in `src/player/`.
- UI never blocks the GUI thread: pages submit work to `Runner`
  (`src/app/workers/Runner.hpp`) with four `QThreadPool`s — shared (4),
  bulk (3) for feed fan-out so a title the user just opened is not queued
  behind the whole feed, stream (2) for stream extraction, which is
  latency-critical and holds its thread across two sequential GETs, and image
  (12) for artwork only. `fetchBytes` bypasses the client's rate limiter
  entirely, so posters cost no API budget and are pure latency. `waitForDone(ms)`
  drains the bulk, stream and image pools first, then the shared one.
  Destroying a Runner never waits: it cancels, drops the completions that were
  still queued, and hands the pools to whoever referenced them last, which
  retires them on the GUI event queue. `cancel()` retires not-yet-started work
  and makes `backend::WorkCancellationScope::cancellationRequested()` true for
  the workers that are running, so a blocking backend wait (rate-limit parking,
  chunked download polling) unwinds within a slice; `Runner::shutdownAll(done)`
  drains every live Runner on a detached thread and reports once on the GUI
  thread, which is what `main()` calls from `lastWindowClosed`.
  A worker reaches the GUI through `post()`, a copyable binder that carries only
  shared state: the body it posts resolves its owner on the GUI thread and
  re-reads the cancellation gate there, so invoking it after the Runner died is
  a no-op rather than a dangling dereference. The scope chain consults every
  enclosing scope, so an uncancelled inner unit of work cannot mask a cancelled
  outer one.
- **A `WA_DeleteOnClose` dialog is destroyed the moment it closes, so the page
  must not keep a pointer to it.** `TitlePage::coverViewer_` had exactly that
  bug: closing the cover freed the dialog, and the next open called `close()` on
  freed memory, which is why open-close-open-close crashed. The pointer is
  cleared from `destroyed`, and nothing pairs `close()` with `deleteLater()`
  on these. `shutdown()` takes the same care.
- `MainWindow` is a `QStackedWidget` holding one `SearchPage` and at most one
  `TitlePage`; disposal shuts the title page down (closing every open player
  dialog and flushing the quality cache) before `deleteLater()`.
  The home page's `Відкрити відео` menu opens a local file or media URL in one
  separate, `QPointer`-tracked `PlayerDialog`. Its hostless constructor keeps
  playback, Compare and Anime4K but hides quality, episode and release controls;
  direct-media failures retry the same source without API extraction.
- `TitlePage` maps `releases[]` (`kind` = `voice`/`sub` + `teams`) through
  `sortReleases`, then renders one tab per release. Tabs are strictly isolated:
  each holds only its own episode buttons. Episodes render as a compact grid of
  number pills in pages of `kEpisodesPerPage = 140`, so a typical 24-episode
  season is a single page and the pager stays hidden. The `RangePager` widget
  (`src/app/widgets/RangePager.hpp`) owns first/prev/chips/next/last plus a
  spinbox jump; the 4-arg `setState` takes explicit episode numbers so a cour
  starting at 110 reads "110 – 249", never "1 – 140".
- Quality extraction happens once per episode, only when that episode is
  opened, on the bulk pool; the in-player quality selector owns height choice.
  Results are cached per embed URL (`QualityCache`, QSettings-backed, TTL 30
  min for signed URLs, 240-entry cap with oldest-first eviction, `forget()`
  for the expired-stream recovery path) and `no_source` stays a page-lifetime
  negative cache that disables the episode's button.
- **The player is in-process.** `TitlePage::Host` implements
  `mplayer::shell::PlayerHost` and drives `PlayerDialog` directly: no
  `QProcess`, no stdin/stdout JSON, no `mikai-player` child. The old
  process-boundary files (`src/player/main.cpp`, `src/player/IpcChannel.*`)
  and the Python bridge are deleted. C++ layering, one direction each:
  `PlayerDialog` (composition root, the only feature surface, hosts the
  `PlayerHost` bridge) → `PlayerShell` (all chrome and the whole display state
  machine, painted with `QPainter`) → `VideoSurface` (a `QOpenGLWidget` mpv
  renders into). The mpv options are set in one place,
  `VideoSurface::ensureEngine()` — `vo=libmpv`,
  `http-header-fields=Accept-Language: uk-UA` (MoonAnime's CDN answers 400 to a
  playlist without it), user-agent, `hwdec=auto-safe`, `keepaspect=yes`, input
  bindings off (the keyboard belongs to the shell). `glsl-shaders` is set there
  too, and `VideoSurface::setAnime4KMode`/`setAnime4KChain` swap it on the live
  core: what is *applied* is resolved from the recorded candidate id at
  construction and on every later change, so a chain chosen before the engine
  exists is honoured on the first frame, and a later pick re-uses the
  `change-list`-equivalent property write, which libmpv accepts at runtime
  (confirmed against `mpv_request_log_messages`: `Set property: glsl-shaders`
  succeeds with no shader compile errors). The choice persists under
  `player/anime4k` as the preset *id*, not the enum ordinal, so a preset added
  later cannot silently repoint an existing selection. PiP copies the mode and
  the applied id into the float's own core, and entering the float never
  inherits the previous session's chain.
  **`VideoSurface` owns the GL state it hands mpv.** mpv leaves **framebuffer 0**
  bound after it paints, so Qt then invalidated the wrong FBO; the contract is
  that after every render/`reportSwap` `VideoSurface` restores the known Qt
  widget FBO. That mismatch produced **54 `ErrorType` messages and a sticky
  `0x0500`** before the fix, and the run reads `ErrorType 0` / `glGetError 0`
  after it. Perf-warning messages remain, so a clean run is still not a
  warning-free run.
  The bottom chrome is one opaque two-row dock: permanent volume + a large
  centred play control + episode/track/enhancement/settings actions above the
  full-width timeline. Anime4K lives under the sparkle button's
  **Покращення → Anime4K** submenu, not the speed/quality gear popover. That
  menu is a tree of pooled `MenuPopup` panels standing side by side: a `QEvent::Enter`
  on a row opens the level it names as a panel beside the one it sits in (rows are
  watched by the shell's event filter), the panels above it stay on screen, and the
  levels below it close. So there is no **‹ Назад** row, and a click on a category row
  opens the same panel while a click on a preset applies it and closes the tree. A
  top-left `MIKAI` chip shows `cache-speed`, `estimated-vf-fps`, and available
  source → output resolution. Official Aji status distinguishes a requested or
  retained filter from the backend's observed processing report, clearing stale
  evidence on Off/source changes. Standard never paints unread private Native
  status; a configured selection is not proof of inference.
  The upscalers are gated by `//!WHEN OUTPUT.w MAIN.w / 1.200 > ...`: Anime4K
  declines to upscale a video mpv is already displaying at or above source
  size. A 480p source in a 1080p window is what actually triggers it, which is
  also the case the feature exists for.
- `src/app/CrashLog.cpp` is installed first thing in `main()`, before any other
  work, so a failure during startup still leaves a trace. It writes
  `mikai.log` beside the exe (rotating to `mikai.log.1` past 2 MB), installs a
  `qInstallMessageHandler` so every Qt message lands there too, and installs a
  `SetUnhandledExceptionFilter` that writes `mikai-crash-<timestamp>.txt` with
  the exception code, the faulting address, the module it landed in, whether it
  was a read or a write, and the tail of the session log. The folder is read
  from the loader (`GetModuleFileNameW`), never `QDir::currentPath()`: launched
  from a shortcut, the cwd is somewhere else entirely and a report in the wrong
  directory is a report nobody finds. The crash handler uses Win32 file APIs
  rather than Qt, because allocating again inside a fault is how a crash
  becomes a hang. A session that starts with an old report still on disk says
  so in the log, so a silent death is never invisible.
- API envelope: `{ok: true, result, total?, page?, pages?}` or `{ok: false,
  error: {errorCode, code, message}}`. List payloads (`/anime`, relations,
  `similar`) give each summary a `status` and planned `episodes` count (`0` =
  unknown); the aired fraction exists only in `/anime/{ref}` as
  `episodes.aired`. `/updates` and `/schedule` embed summaries with ids,
  titles and posters only. Branch on the stable `error.code` string, never on
  HTTP status alone.
- `MikaiClient` gates every call through `RateLimiter` (`QMutex` +
  `QWaitCondition`), driven by `X-RateLimit-Limit/Remaining/Reset` and
  `Retry-After` (header lookup is case-insensitive: the CDN lowercases them);
  a 429 parks callers until the advertised window instead of failing.
  `rateLimitState()` feeds the RPM label and the settings quota panel. Every
  request carries a browser `User-Agent`: the bot filter 403s requests without
  one, and Qt sends none by default.
- Identity: `{ref}` is a bare Mikai id or `mal:21` / `al:21` / `hikka:…` /
  `slug:…`. Incremental sync via `/updates`, never full-catalog polling.
- **Bundle validation is cached, because re-hashing it on every start cost half a
  minute.** `validateBundleCached` and `describeStandardRuntimeCached` compute a
  stat-only fingerprint first — every file's relative path, size and modification
  time — and answer from a previously validated verdict while nothing moved. Only
  a *valid* verdict is remembered: a failure is re-probed on every call, so a
  repaired bundle or a reconnected disk is never remembered as broken, and
  `invalidateBundleValidation` drops a verdict outright — `commitRuntime` calls it
  when the loader refuses the bundle, and the stock descriptor goes with it. The
  cache is `verified.json` under the app-owned native root (schema
  `mikai.native.cache/2`), a derived artefact that is ignored rather than
  half-read when its schema does not match. Measured from the app's own log: 23.8 s
  before, 18.1 s on a cold cache, ~0.6 s on every start after.
- **Native assets are per-machine, not part of the app or dist.** Official
  AnimeJaNai 3.7.0 TensorRT packs and the 47-model RIFE archive are explicit
  user-requested components. Public Aji downloads stay pinned to v0.9.0; a
  separate local custom ABI-8 import can carry v4.26 heavy mapping and is never
  silently substituted or auto-downloaded. Acquisition and plan-only checks
  establish neither private Ready/Active inference nor redistribution rights.
  Acquisition records and exact evidence belong in `native/README.md` and
  `native/manifest/`, not a second ledger here.
- **Engine building is a paused, visible operation.** `ProcessingOverlay` is a
  native always-on-top tool window, so modal Settings cannot obscure it. Show
  the actual preparation/build phase and elapsed time; use an indeterminate bar
  when the backend supplies no measurable stage count. A plan's stage count is
  not TensorRT build progress, and no percentage is invented. Completion is
  visible before automatic dismissal; failure stays until dismissed. Hold video
  paused on the actual playback owner throughout official or private engine
  work. Retain explicit Play/Pause intent until terminal acknowledgement. Clear
  the old source's hold on replacement; if the operation remains active, re-arm
  it on the current playback owner with the saved intent. Source replacement
  does not manufacture a new operation generation, and delayed load completions
  cannot replay intent older than a viewer's Play/Pause action. PiP transfers
  the hold and latest intent; only the actual owner resumes on completion.
  The filter poll command's mandatory fourth empty argument is fixed. Current
  attached-console natural-player runs complete fresh RIFE and spatial engines,
  keep playback paused until visible completion, and resume afterward; the
  spatial source is 512×288 and the RIFE proof uses an uncached 576×320 shape.
  Real-core operation checks pass 93/93 on H.264 and player interactions pass
  172/172. Parking main must not overwrite the intent copied into PiP; capture
  it before requesting main's physical pause.
  `main()` requires desktop OpenGL before constructing `QApplication`: Qt's
  automatic software selection produced a refused Mesa context while the
  desktop was disconnected, whereas forced desktop OpenGL renders normally.
  With the console attached, the native-DPR frame-152 screen-first reference
  check passes 5/5. That mode excludes subtitles and does not close the original
  captioned-player/Compare corruption gate. Exact current and failed runtime
  evidence is in `native/README.md`, not inferred from this contract.
- **Startup resolves the runtime off the GUI thread and says so while it runs.**
  `main()` shows the window first, submits `resolveRuntime(storedRuntimeChoice())`
  as Runner work, and commits on the GUI thread; nothing hashes or loads a module
  on the GUI thread. One `ResolutionLease` owns the single process resolution
  (`beginResolution`/`endResolution`, ids never reused, so a stale lease cannot
  release the resolution that replaced it) and `whenRuntimeResolutionFinishes`
  delivers once, after the terminal state, to a still-live context. A playback
  request made while that is in flight joins it instead of failing: the dialog
  shows `runtimePendingPlaybackText()` — never a missing-runtime error — and
  starts its own still-open source once the module is pinned, or names the
  recorded reason. Before `LoadLibraryExW`, `MpvApi::load` reads the module's own
  PE import table and refuses by name when a required Windows system driver
  (today the Vulkan loader that `libmpv-2.dll` imports) is absent from
  `%SystemRoot%\System32`; nothing is copied and no substitute module is tried.

## Key Directories

- `src/backend/` — domain + Qt Network client:
  - `MikaiClient.hpp/cpp` — `MikaiClient` (`search`, `browseOngoing`,
    `browsePopular`, `getAnime`, `getPlayer`, `getUpdates`, `getSchedule`,
    `fetchBytes`, `apiKey`, `setApiKey`, `validateApiKey`,
    `rateLimitState`) plus `RateLimiter` and `.env` persistence.
    `fetchBytes(url, timeoutMs = -1)` is the one transport callers use for large
    non-artwork payloads: a negative value keeps the artwork default, and the
    model downloader asks for its own budget, because 20 s is an artwork figure
    that aborts a graph download on a slow link.
  - `Models.hpp/cpp` — structs (`AnimeSummary`, `AnimeDetail`, `Release`,
    `Episode`, `Source`, `Team`, `SearchResult`, `UpdatesResult`,
    `ScheduleResult`, `PlayerResult`, …) with `displayName`, `thumbUrl`
    (small), `posterUrl` (big), `coverUrl` (UA posters first), `externalLinks`.
  - `Releases.hpp/cpp` — `playableEpisodeCount`, `sortReleases`.
  - `Errors.hpp` — only `MikaiApiError{code, message, status}`.
  - `Streams.hpp/cpp` — HLS extraction per site; `NoSourceError`.
  - `Runtime.hpp/cpp` — packaged libmpv module discovery and runtime selection.
    It never downloads mpv or falls back to a stock cache; a missing packaged
    module is unavailable.
  - `Downloads.hpp/cpp` — the one transport every downloader shares:
    `fetchUrlBytes` (one GET, whole body in memory), `downloadToFile` (creates the
    destination exclusively, hashes as it writes, refuses a size or digest
    mismatch, and removes only its own partial file on a failure or cancellation
    path — a destination that already existed is refused untouched, never
    deleted). Every chunk's write result, the final flush and the close error are
    checked before any verified verdict, and the connection is dropped as soon as
    a write fails. It bounds silence with a 60 s stall watchdog restarted per
    chunk, polls cancellation every 200 ms and honours the shared work-cancellation
    state as well as the caller's own callback, and every URL-bearing string in a
    reason is run through `redactDiagnostic` so a signed asset URL never reaches a
    log or a fault report), `ensureSevenZip`, `extractArchive`. Progress is
    throttled to one report per 100 ms plus a guaranteed terminal one: a per-chunk
    report posts a GUI update for every few kilobytes of a 400 MB asset.
    The component download helpers are for explicit native/model assets only;
    there is no app-launch mpv download path.
- `src/app/` — the Qt Widgets frontend, namespace `mikai::app`:
  - `main.cpp` — entrypoint (application/organization names, locale translators,
    Fusion style and one `setStyleSheet`, then `selectPlayerRuntime()` and
    `MainWindow` over a `MikaiClient`). `selectPlayerRuntime()` calls
    `mplayer::native::selectStartupRuntime()`, which resolves the stored
    Standard/Native choice and loads the chosen module through `MpvApi`; a
    missing or refused runtime is one log line, and playback reports the same
    named reason. There is no load-time libmpv import and no `PATH` edit to
    provision one.
  - `pages/` — `MainWindow`, `SearchPage` (home discovery, async poster
    thumbs, format bar, 55-per-page search pager, per-card detail fan-out),
    `TitlePage` (per-release tabs, 140-episode grid, fetch-on-open qualities,
    in-process `PlayerDialog` via `Host`, episode context menu,
    external-player hand-off via `QProcess::startDetached(mpv|vlc)`).
  - `widgets/` — `TitleCard` (360×140 home card), `RangePager`,
    `RateLimitLabel` (RPM indicator).
  - `dialogs/` — `SettingsDialog` (API key + quota + feed toggles, preset
    shortcuts, and the Standard/Native **Runtime module** choice),
    `ComponentStoreTab` (the Components tab: AnimeJaNai 3.7.0 TensorRT packs and the
    47-model RIFE archive, with install state, digest verification and
    user-initiated download), `NativeAssetsPanel` (the Processing tab's
    lazily refreshed owned-model inventory, artifact cache and source-asset store),
    `CoverViewer` (chromeless full-screen cover with zoom/pan).
  - `state/` — `Feeds` (home-block toggles), `QualityCache`,
    `NativeComponentProvisioner` (the app-side component driver: `ensure()` is the
    bundle-aware path, `ensureStore()` the tab's explicit autofix,
    `needsDownload()` a side-effect-free question, and one single-flight worker
    with a cancellation gate).
  - `workers/` — `Runner` (pools 4/3/12).
  - `theme/` — `Colors.hpp`, `Metrics.hpp`, `Theme.hpp/cpp`
    (`buildStylesheet()`), `Icons.hpp/cpp` (painted glyphs, never emoji).
  - `Labels.hpp/cpp` — status labels.
- `src/gui/Clipboard.hpp` — canonical text/image clipboard helper, namespace
  `mikai::gui`. Success requires the actual write/readback, not just an attempted
  action. Player refusals use visible status plus an accessibility alert; hosted
  title actions report failures in a warning parented to the active player.
- `src/player/` — the player: `MpvApi` (the one dynamic module loader and its
  function table; `MpvHandle` is the only place calling `mpv_*`) →
  `MpvRenderContext` → `VideoSurface`; `ShellTheme`, `ShellWidgets`,
  `ShellSliders`; `PlayerShell` (chrome, display states, input; the picture is
  always fitted, there is no zoom or pan);
  `PlayerPopups` (`MenuPopup`, `QualityPopup`, `TrackPopup`,
  `SeriesDrawer`); `PlayerDialog` (composition root + `PipFloat`).
  Picture-in-picture has four rules that each cost a real bug, and all four
  are exercised in `InteractionProbe` (a description, not a green run — see the
  probe note below):
  - **The float owns its playback.** Every control - the scrubber, the play
    button, click-to-pause, the arrow keys - acts on the float's own
    `VideoSurface`. They used to emit signals the host forwarded to the main
    surface, so they moved a video hidden behind the float and appeared to do
    nothing. The three `PipFloat` play/seek signals are gone for that reason.
  - **Only one core plays.** Entering PiP pauses the main core; leaving
    pauses the float's. Either one playing decodes the stream twice.
  - **The float is pooled, not destroyed.** `PlayerDialog::pipPool_` keeps it
    alive and merely hides it. Tearing down a second `QOpenGLWidget` while the
    player's is on screen faults the NVIDIA driver in `DrvPresentBuffers` - a
    crash in the driver, but ours to avoid - and neither a `deleteLater` nor a
    400 ms deferral avoids it. Re-entry reuses the surface, so it is also
    instant. `setPaused` writes unconditionally: a version that short-circuited
    on the cached `playing_` flag silently dropped the call when the cache was
    stale, leaving a surface the caller believed it had resumed frozen.
  - **The float is placed freely.** A drag drops it exactly where the pointer
    leaves it — no snap-to-corner on release, no re-pin on resize, show, or
    re-entry. The single placement rule is *reachability*: some part of the
    float must share pixels with a work area, or it cannot be clicked or dragged
    back (`overlapsAnyWorkArea()` / `pullIntoWorkArea()`). A pooled float
    re-enters at the exact position and size the viewer last left it; only an
    unreachable position (a monitor that went away) is corrected, once, in
    `showEvent`. `moveToCorner()` is the *first* placement of a brand-new float
    only. An earlier snap-to-corner-on-release was removed: it defeated the
    whole point of a draggable float by forcing every drop to a fixed corner.
  Progress follows the float's own `positionChanged`, and `setTime` shows the
  position whenever it is known - blanking it because the *duration* was not
  threw away the only number a viewer can read.
  `Anime4K` — one declarative catalogue (id, label, hint, category, ordered
  chain) in `Anime4K.cpp` plus the menu tree above it: ten built-in presets
  (`Mode::Off`..`HqCA`, ids frozen), fourteen experiments grouped under
  Anime4K's own family names (Noise Reduction, Thin Lines, Texture
  Enhancement, Darken). `menuRows(parent)` is what the shell paints, one level
  per panel; a category row only opens the panel beside it and cannot change
  the chain. Standard exposes Anime4K and official AnimeJaNai; the latter owns
  the working spatial and RIFE model selectors. Private prepared-model menus
  require a loaded Native runtime. Processing's private editor/import/preparation
  controls appear only when Native is deliberately selected or already loaded;
  Standard keeps the official Aji/RIFE and Anime4K flows. Hidden private requests
  are preserved. No redundant Native checkbox, toggle shortcut, or planned
  nonfunctional menu leaves are offered. `shaderStringForCandidate(id)`
  is the only chain resolver, used for a mode and for a loose `.glsl` alike.
  `CompareWindow` — source-isolated image comparison from the picture's
  right-click menu. Its two corner badges are the only model choosers; their
  menus close before expensive rendering. Choices are the player's 24 stable-ID
  Anime4K presets (including Off) plus five public AnimeJaNai spatial models.
  Loose `.glsl` files and temporal RIFE models are excluded. Unsupported or
  missing backend/model closure is named, never silently substituted.
  `CompareRenderer` owns a separate libmpv core, offscreen GL context, surface
  and framebuffer. It captures one lossless source PNG at the held playback
  timestamp and loads that immutable input once; actual shader/filter processing
  renders both sides there. It does not swap or restore the live player's shader
  chain, Aji plan, or pixels. The main source/pause hold and private plan lock
  protect the captured epoch; closing releases them.
  Only the two current outputs are retained, with the unchanged side reused on
  a model switch; identical choices share one output and Off is the canonical
  source image. A failure names the affected choice without mislabelling stale
  output or clearing the other side. Source replacement, close, or cancelled
  work invalidates pending results.
  Five layouts are exposed: Split H, Split V, Side-by-side, Flicker, Difference.
  Controls appear only for the relevant mode, including on initial display.
  Fit uses the active canvas or side-by-side grid geometry; wheel zoom works in
  both, with pan/divider gestures handled by the respective image view.
  Compare remains non-blocking and is not opened from the PiP float.
  Canonical shader/spatial checks have passed, but the fresh 416×240 run still
  reported an early live-surface Off restoration residual (0.676/255 against the
  unchanged 0.05 tolerance) and its cause is unproven; do not call it dithering or
  weaken the pixel tolerance. A canonical startup also ended with exit 5 after
  only its CLI line, so that invocation carries no rendering acceptance; it is
  retained here as a historical, unexplained result. A later normal canonical
  startup ran under GDB and reported `RESULT: PASS`, so the exit-5 line is not
  the latest known startup outcome — and neither run closes the 416×240 rendering
  gate. See `native/README.md` for exact observed outcomes and remaining gates.
- `src/player/native/` — runtime selection and the native vocabulary:
  `selectStartupRuntime()`/`resolveRuntime()`/`commitRuntime()` pick and pin
  one module per process (a later choice is refused in place with a restart
  reason), `NativeBundle`/`NativePaths`/`NativePeImage` validate a bundle
  before any load, and `NativeManifest`/`NativeRecipe`/`NativeStatus`/
  `NativePrefs`/`NativeMenu` carry the profile and recipe model. Compiled into
  the app unconditionally; `MIKAI_WITH_MODEL_INSPECTION` only adds the optional
  CPU inspector.
  - `components/` — the GPU-adaptive TensorRT components. `NativeComponentCatalog`
    pins the AnimeJaNai `3.7.0` packs by asset byte count and SHA-256 and selects
    the exact `trt-sm{MA}{MI}` pack when the release publishes one, otherwise
    `trt-ptx`, and `nullptr` for a shape the name cannot express rather than an
    approximation onto a neighbour; `trtexec.exe` is deliberately absent.
    `NativeGpuProbe` reads the compute capability from the installed driver's
    NVML, loading `nvml.dll` only from System32 and returning a named refusal
    (`native.gpu.nvmlMissing` / `deviceMissing` / `capabilityUnreported`) rather
    than a default. `NativeComponentStore` installs a verified pack into the
    app-owned `<nativeRoot>/components/tensorrt/3.7.0` and records what it proved
    (`component.json`, schema `mikai.native.components/1`); `storedPack` trusts the
    recorded size while `verifyPack` re-hashes, and `installPack` streams
    source→target hashing as it copies, so a copy that does not reproduce the
    source digest is refused instead of recorded. `NativeComponentToolchain`
    decides which directory the builder's PATH gets: the bundle's own trt/cuda
    dirs when the bundle declares this GPU's resource, else the store's matched
    runtime+resource pair, else a named refusal. `NativeComponentProvisioning`
    downloads, extracts and installs a pack set, single-flight per process.
- `shaders/` — the 49 Anime4K `.glsl` presets, copied next to the executable
  by POST_BUILD rules on `mikai`, `mikai_tests` and shader-using probes.
  `Anime4K::findShadersDir` looks beside the
  exe and in the cwd; when it finds nothing Anime4K controls hide rather than
  offering rows that cannot work. Loose shader files are resolver inputs, not
  additional player/Compare menu leaves.
- `src/probes/` — hand-run CMake targets (`EXCLUDE_FROM_ALL`; nothing in
  `tests/` runs them). A probe that mutates the native store enters
  `mikai::probes::isolation` (`src/probes/shared/ProbeIsolation.hpp`) first: it
  copies itself into an owned `QTemporaryDir`, re-anchors the relative bundle
  paths to the original module directory and relaunches, and the child runs only
  when a marker proves the directory it was loaded from is that verified
  scratch. Every mutating wipe and every cleanup is gated on
  `isOwnedScratch(nativeRoot())`, so launching one of these beside a real store
  either refuses or works on its own copy — changing the application name or the
  CWD never isolates anything. The GUI ones build a real window and check
  pixels:
  `mpv_surface_smoke`, `headless_pixel_probe`, `widget_pixel_probe`,
  `shell_probe`, `dialog_probe`, `InteractionProbe`, `anime4k_menu_probe`,
  `compare_probe`, `compare_shot_probe`, `external_media_probe`,
  `hls_recovery_probe`. The loader,
  selection and native-panel ones are `mpv_loader_probe`,
  `runtime_selection_probe`, `resolver_probe`, `native_status_probe`,
  `native_menu_probe`, `native_settings_probe`, `rife_settings_probe`,
  `spatial_assets_probe`, `native_prep_probe`, `native_component_probe`,
  `engine_popup_probe` and, only under
  `MIKAI_WITH_MODEL_INSPECTION`,
  `native_assets_probe`.
  `native_component_probe` is the falsifiable check for the pack choice: `--print`
  reads the real device and the resolved bin directories, `--fetch` pre-populates
  the store and prints every installed file's size and digest, and `--from-sdk`
  stages a same-release runtime+resource pair and runs the real builder — with the
  GPU's own pack it builds, and with another generation's it fails naming the
  resource that is missing, which is what proves the selection decides the
  outcome. `engine_popup_probe` drives the player's preparation window through its
  public API, including completion/failure visibility and a second build
  reopening the window with working cancellation. `native_prep_probe` links only
  `Qt6::Core` and takes
  `<helper.exe> <graph.onnx> <GPU-uuid> <named-input-shapes>`, driving the
  native preparation helper against a real CUDA device; it is Windows-only and
  needs no interactive session. The
  loader/selection probes take `MIKAI_MPV_MODULE` when they need a specific
  DLL. Networked runs:
  `title_page_probe` drives the real `TitlePage` against the real API (its
  cover open/close loop pins the use-after-free) and `external_media_probe`
  needs a 320×180 clip plus its HTTP fixture; `crash_probe` installs the app's
  logging then faults on purpose, to prove a report is actually written.
  Action-refusal probes are independent: `external_media_probe
  --player-clipboard-failure <localclip>` checks a real hostless player under
  Windows QPA with a separate clipboard-lock holder; `title_page_probe
  --host-copy-refusal-only` checks the actual title host under Windows QPA,
  while `--host-browser-refusal-only` uses offscreen QPA for browser refusal.
  The title scenarios exercise real API sessions and active-player warnings;
  do not combine the platform-specific failure mechanisms. These new scenarios
  still require the parent's final run.
  `widget_pixel_probe` takes a PNG path argument and otherwise runs one fixed
  lavfi source, so it does not exercise alternate-media or dialog flows; its
  only observed failure mode is a refused PNG save (FAIL, exit code 1).
  `shell_probe` and `dialog_probe` are the probes that observed the
  missing-input FAIL (exit code 1), the static/black inconclusive SKIP (exit
  code 77), the moving-portrait and the 1 s-EOF cases. `mpv_surface_smoke`
  also requires inter-frame motion: the static near-black source is SKIP
  (exit code 77), not PASS. `headless_pixel_probe` proves lit pixels and a
  real render without a motion requirement; static near-black can pass there,
  while all-black is inconclusive.
  `InteractionProbe`
  measures Anime4K by freezing the clip on one timestamp, taking a control pair
  of frames with nothing touched, then enabling the preset and measuring the
  picture difference. Three things about that measurement are load-bearing and
  were each found by the check passing when it should not have:
  - The control must be two settled frames *without* re-seeking. Seeking to
    the same timestamp twice does not reproduce the same pixels, and a helper
    that re-seeks makes the "noise floor" a frame-to-frame difference
    (~5/255 here) instead of the real 0.000.
  - The comparison must stay inside the picture band, derived from the source
    aspect. The whole framebuffer includes the letterbox bars, whose edges read
    as a 20x sharpening, and on a portrait framebuffer the middle *half*
    still reaches them.
  - mpv's `glsl-shaders` property echoes the string back without validating
    it, so it cannot tell a working chain from a preset naming a deleted file.
    Verified by deleting one `.glsl`: the property still read back complete.
  The absolute difference is ~0.4/255 because the upscalers carry
  `//!WHEN OUTPUT.w MAIN.w / 1.200 > ...` and decline to upscale a video mpv
  is already displaying at source; on a small surface only the ungated restore
  half runs. The check asserts the chain runs, not that it is worth enabling.

  **A description is not a pass.** These paragraphs describe what probes
  measure, not a green run of the current tree. Older desktop results are
  historical; post-integration checks must cover both Compare families without
  black model switches, exact live-player preservation, engine pause/progress,
  pooled PiP geometry, and real refused actions. Pixel proofs need the
  interactive session attached; detached results read black with `hasVideo=0`.
- `tests/cpp/` — `mikai_tests` (one exe, one ctest entry; `main.cpp` drives
  each suite and prints a per-suite PASS/FAIL/SKIP line). Domain and UI suites:
  `TestModels`, `TestReleases`, `TestAnime4K`, `TestI18n`, `TestStreams`,
  `TestPagers`, `TestQualityCache`, `TestRateLimiter`, `TestLayering`,
  `TestClipboard`. Runtime and native-contract suites: `TestRuntimeSelection`,
  `TestMpvAsync`, `TestNativeTypes`, `TestNativeRecipe`, `TestNativeProfiles`,
  `TestNativeRife`, `TestNativeBundle`, `TestNativePrepPlan`,
  `TestNativeManifest`, `TestNativeStatus`, `TestNativePrefs`,
  `TestNativeMenu`, `TestNativeUserRecipes`, `TestNativeAssets`,
  `TestNativeComponents`, plus
  `TestNativeCatalog` only when `MIKAI_WITH_MODEL_INSPECTION=ON`.
  `main.cpp` also records every suite name it was handed and fails the run when a
  name in the filter was never executed: a suite registered without a `run()` call
  has its name stripped from QTest's arguments and then never runs, which is how
  `TestNativeComponents` was invisible rather than failing. The live
  oracle `TestLiveOracle` is gated on `MIKAI_LIVE=1` + `MIKAI_API_KEY`
  (BLACK TORCH → ref `7428`, 7 releases with 4 dubs / 3 subs, one dub carrying
  episodes 1–12, and a 480 / 720 / 1080 ladder) and is a deliberate skip, not a
  pass, when the key is absent.
- `skills/design-ui/` — `SKILL.md` + `references/`.

## Development Commands

Toolchain: MinGW-w64 g++ (`C:/temp/w64dev/w64devkit/bin`) and Qt 6.8.x
(`C:/temp/qt6/6.8.3/mingw_64`); CMake 3.25 or newer, because `CMakePresets.json`
is schema 6 and that is the version which introduced it. Release tuning is
`-march=x86-64` (the Microsoft x64 ABI floor: x64 plus SSE2), never
`-march=native` and never a newer baseline, so the project's own GNU release
objects do not require an instruction set the platform does not guarantee.
CMake acquires the pinned Windows x64 mpv
developer archive or validates `MIKAI_DIST_MPV_ARCHIVE` against its pinned
SHA-256, extracts matching headers, and compiles against them; no manual
`MPV_INCLUDE_DIR` is used. The app uses the libmpv C/render API in its existing
Qt `QOpenGLWidget` GUI, with no `mpv.exe` or IPC. The build output and
`dist-mikai/mpv/` carry only `libmpv-2.dll`. The optional native SDK is not
needed for an ordinary build. This Qt build needs `QT_PLUGIN_PATH=<qt>/plugins`
in the environment or TLS reports 0 and every HTTPS call fails.

```sh
cmake --preset release
cmake --build --preset app --parallel
build/release/mikai.exe     # run directly; no launcher script needed
ctest --test-dir build/release -R mikai_tests   # hermetic + live oracle when keyed
curl "https://api.mikai.me/public/v1/meta"   # API sanity check
```

Presets (`CMakePresets.json`): `release` (default), `debug`, `native`
(inspection ON). Build presets: `app`, `tests`, `app-native`, `tests-native`.

Warnings are on for GNU/Clang — `-Wall -Wextra -Wno-unused-parameter
-Wno-cast-function-type`, set once in `CMakeLists.txt` — and zero warnings is
the build expectation. The two suppressions are deliberate: Qt's signal/slot signatures
leave unused parameters everywhere, and the `GetProcAddress` cast is the only way
to load a symbol by name. Turning them on is what found a buffer overrun, a test
suite that never ran and four functions nothing called, all of which had been
compiling quietly.

`cmake --build build/release --target dist` assembles a self-contained folder
(`cmake/dist.cmake`, into `dist-mikai/`): the exe, its Qt runtime via
`windeployqt`, the 49 shaders, translations, `libmpv-2.dll` under `mpv/`,
`run.bat`, and `VERSION`. That API DLL is the only mpv runtime artifact; the
app never downloads a replacement, and playback is unavailable without the
packaged module. It never ships the downloadable elements — the AnimeJaNai
3.7.0 TensorRT packs and 47-model RIFE archive remain per-machine assets —
and removes any `components/` or `assets/` tree a copied native bundle carried.
`MIKAI_DIST_NATIVE` (OFF by default) additionally copies `native/runtime`.

`dist` refuses to run into a populated output directory at all — it names what
it found and stops, with no bypass flag — because recreating the output is what
destroyed adjacent installed state before. Its native root is executable-local
(`<exe dir>/native`, with `MIKAI_DIST_NATIVE_ROOT` as the explicit override,
forwarded to the child `cmake -P`), a missing runtime under an explicitly
requested root is a hard failure rather than a silent skip, and the source and
output trees may not contain one another. Downloadable payload directories
(`components`, `assets`, `models`, `records`) are removed by name from the whole
copied tree at any depth, and a post-sweep re-glob fails the assembly if one
survived. For a UI-only update, copy just the verified executable into the
installed folder.

`mikai.exe` links Qt dynamically, so the Qt runtime has to sit beside it or be
on `PATH`; a fresh tree needs `windeployqt build/release/mikai.exe`. The mpv DLL
is not a load-time import: the app loads the packaged `mpv/libmpv-2.dll`
through libmpv's C/render API in its existing `QOpenGLWidget` UI. Runtime
discovery never downloads mpv or falls back to a stock cache; without the
packaged module playback is unavailable. `mikai.exe` is `WIN32_EXECUTABLE`
(no console); diagnostics go to `mikai.log` beside the exe (see the crash-log
note above).

`mikai_tests` runs every suite in one process; `mikai_tests <SuiteName>` runs
one. The `MIKAI_LIVE=1` + `MIKAI_API_KEY=mk_...` oracle hits the real API.

No linter, type checker, or coverage tool is configured.

## Code Conventions & Common Patterns

API contract (when adding backend code):

- Error handling: branch on the stable `error.code` string. Known codes and
  statuses: `invalid_parameter` / `invalid_reference` / `too_many_ids` (422),
  `invalid_request` (400), `not_found` (404), `invalid_api_key` (401),
  `api_key_disabled` (403), `rate_limited` (429), `internal_error` (500). An
  unknown filter value returns 422, never an empty list.
- Rate limits: 60 req/min keyless (per IP), 300 req/min keyed (per key), no
  daily quota. Re-check live enums/limits via `GET /meta`; never hardcode.
- Naming: the API is camelCase, timestamps are RFC3339, episode numbers are
  fractional (`1`, `1.5`). Licensed titles return `licensed: true` with empty
  releases.
- Cache TTLs per `docs.md`: 1 h for dictionaries, 5 min for catalog/player/teams,
  1 min for `/schedule`, `/updates`, `/posters/ua`, no-cache for `/me`,
  `/health`, `/anime/random`.
- Attribution: link mikai.me, preserve `team`/`author` credits, no bulk mirroring.

UI (`skills/design-ui/SKILL.md`, depth in `skills/design-ui/references/`):

- Tokens once, compose everywhere: no raw hex or magic numbers outside
  `src/app/theme/`; every new value becomes a token in `Colors.hpp` or
  `Metrics.hpp`. Apply `app.setStyleSheet(buildStylesheet())` exactly once in
  `main()`.
- Native controls first (`QPushButton` / `QDialog` / `QMenu` / `QTabWidget` /
  `QTabBar`), styled through the QSS. Variant roles: primary / secondary /
  destructive.
- Rubric: ≤5 colors (1 primary + neutrals + ≤1 accent), ≤2 fonts, body
  line-height 1.4–1.6, 4/8-based spacing, one primary action per view, override
  fg whenever bg is overridden, concentric radii (outer = inner + padding).
- Icons: painted glyphs through `theme/Icons` — never emoji. HiDPI-aware sizing.
- Motion: 150–250 ms (`QPropertyAnimation`), opacity/position only, respect
  reduced motion.
- Min-window-first: no clipped controls, targets ≥24 px desktop / ≥44 px touch.
  Ship loading / empty / error states.
- Async discipline: every async callback must be discardable. Use the existing
  sequence-token pattern, `QTimer::singleShot`, and `QPointer` guards before
  touching a widget that may have been deleted mid-flight.
- Logging: `qWarning()`/`qInfo()` with context; do not swallow errors silently.
- Translation: a user-visible string must be extractable by `lupdate`. A file
  that funnels its literals through a local helper (a `translate()` wrapper, a
  `QT_TRANSLATE_NOOP`-carrying constant) has to keep the macro inside that
  helper, because a bare `tr()`/`QObject::tr()` in a file whose convention is the
  helper is never extracted and can only ever render English — that is exactly
  how three setup-prompt strings were missing from the catalogue. After adding
  or changing strings, run `lupdate` and fill the new entries, then `lrelease`
  and check the catalogue reports zero unfinished translations.

## Important Files

- `src/app/pages/MainWindow.cpp` — navigation + title disposal.
- `src/app/pages/SearchPage.cpp` — home discovery (per-card detail fan-out,
  ~20 API calls per home load, paced by the limiter), search + 55-per-page
  pager, feed toggles (a disabled block is never requested: zero API cost).
- `src/app/pages/TitlePage.cpp` — the largest module: release selector,
  per-release tabs, 140-per-page episode grid, fetch-on-open qualities,
  in-process player wiring, episode context menu, external-player hand-off.
- `src/app/main.cpp` — entrypoint; `selectPlayerRuntime()` runs after
  `QApplication`, application identity, translators and theme setup, before any
  widget can reach an mpv core. It resolves the stored runtime choice and loads
  the module through `MpvApi` — no load-time libmpv import, no `PATH` edit.
  Component archives are downloaded only after an explicit user request from
  the Components tab; startup performs no component download. The packaged API
  DLL contains ABI-8 `vf_animejanai`, faster ASS changes and display-rate-match.
  The official filter path is separate from private Native admission; selecting
  a preset requires verified Aji/backend/model closure and does not establish
  inference or Active status. Subtitle GPU blur/compositing/rasterization needs
  gpu-next and forklibass and is not claimed on this OpenGL surface.
  Display-rate-match is default-off and local seekable mkv/lavf only. The DML-to-Qt
  OpenGL path lacks direct D3D11 interop; hardware `d3d11va` → Aji filtering →
  mpv downstream download is the intended path, never CPU-ahead fallback, and
  hardware playback through it remains unobserved. Local runtime installation
  requires an explicit directory/backend, is cancellable and is not auto-imported.
- `src/app/dialogs/ComponentStoreTab.cpp` — the Components tab. Availability is
  the toolchain's verdict rather than the store's contents: asking the store alone
  reported a resource the installed bundle already carries as "Not installed" and
  made Download-missing fetch hundreds of megabytes it did not need.
- `src/app/state/NativeComponentProvisioner.cpp` — the component driver, and the
  only place the app starts a component download.
- `src/backend/Downloads.cpp` — the one transport; `downloadToFile` is where the
  exclusive-create guard, the write/flush/close proof, the stall watchdog, the
  cancellation poll, the digest proof, the throttled progress and the
  URL-bearing redaction all live.
- `src/player/native/components/` — the pinned catalogue, the NVML probe, the
  component store, the toolchain decision and the single-flight provisioner.
- `cmake/dist.cmake` — the `dist` assembly, including the rule that no
  downloadable element is ever shipped.
- `src/backend/MikaiClient.hpp` — `RateLimiter` + `MikaiClient`; header
  documents the Cloudflare notes (lowercased headers, mandatory User-Agent).
- `CMakeLists.txt` — targets `mikai_native_support` (STATIC, always),
  `mikai_player` (STATIC), `mikai_backend` (STATIC), `mikai_app` (STATIC),
  `mikai` (WIN32 exe), `mikai_tests` + probes, and — only under
  `MIKAI_WITH_MODEL_INSPECTION=ON` with `ORT_INCLUDE_DIR` — the
  `mikai_model_inspection` library, `native_inspect` and `TestNativeCatalog`.
- `docs.md` — authoritative API contract (Ukrainian); `.env` — single
  `X-API-Key: mk_…` line, never commit.

## Runtime/Tooling Preferences

- C++20, Qt 6.8.x, MinGW-w64. No Python anywhere: no `pyproject.toml`, no
  `uv.lock`, no `.venv`, no `pytest`. The only runtimes beside Qt are the mpv
  runtime dir and the deployed Qt DLLs next to the exe.
- The API key is optional and only raises limits. Resolution order in
  `MikaiClient`: explicit argument → `MIKAI_API_KEY` env var → repo-root
  `.env`. `setApiKey` writes the `X-API-Key:` line back to `.env`.
- Windows-first: the packaged `mpv/libmpv-2.dll` is the app's only mpv runtime;
  a probe or test can name one DLL with `MIKAI_MPV_MODULE`. The app does not
  link libmpv at load time: `selectStartupRuntime()` resolves the module and
  `MpvApi` loads it explicitly, once, and pins it for the process. Many core
  handles (main plus the PiP float) share that module; swapping runtime needs a
  restart. Missing package means unavailable playback; app-launch discovery
  does not download mpv or use an arbitrary stock cache.
- The repo is a git repository. The root `.gitignore` covers `.env`,
  `__pycache__/` (leftover bytecode only, untracked), `.venv/` (removed),
  `*.png`, and `build/`; there is no CI pipeline, no
  Dockerfile, and no linter, type checker, or coverage config.

## Testing & QA

```sh
ctest --test-dir build/release -R mikai_tests
```

CTest runs four entries: `mikai_tests` (the whole suite, through the launcher),
`NativeStageContracts` (the staging/patch-policy helper, script-mode CMake),
`LauncherContracts` (the launcher's deadline, process-tree kill and cleanup
against a real hung child) and `DistContracts` (the distribution refusals, the
payload sweep and byte-preservation of a populated output). The launcher copies
the test executable into a unique `build/release/_test-runs/mikai-tests-XXXXXX`
directory and removes it on every return path. It owns an internal deadline
(`MIKAI_TEST_TIMEOUT_MS`, default 600000 ms; `--launcher-timeout-ms` overrides
it) and joins the suspended child to a kill-on-close Win32 job before resuming
it, so a hung suite and every descendant it started are killed and the run exits
3; CTest's own TIMEOUT is deliberately larger. Direct test execution refuses a
nonempty or unsafe adjacent `native/` tree before any suite runs. Never run
mutating test/probe binaries from `dist-mikai`: changing CWD or the application
name does not change the executable-local native root. Supply Qt DLLs/plugins
through the configured Qt paths instead; see `readme.md` for the isolated
commands.

Keep diagnostic outputs under `.probe-out/<run>/`, not the repository root.
The root screenshot batch is archived or deduplicated with exact retained paths
in `.probe-out/root-screenshots/cleanup-manifest.json`; maintained probe sources
and scenario manifests remain in place.

- `tests/cpp/main.cpp` drives every suite in turn, prints one
  `[mikai_tests] <Suite>: PASS/FAIL/SKIP (rc=…)` line per suite to stderr (QTest
  itself stays silent on success), and accepts a suite name to run just one.
  Use the isolated launcher's suite/QTest arguments documented in `readme.md`;
  direct execution is safe only beside an absent or empty native tree.
  `MIKAI_SOURCE_DIR` lets the layer guards read the sources they police instead
  of passing vacuously.
- Hermetic suites: `TestModels` (parse fixtures: ref fallback
  `mikai/mal/al/hikka/slug:`, `displayName`, thumb/poster/cover order,
  `externalLinks`, episode int/double/string/null), `TestReleases`
  (playable count, voice→sub sort), `TestStreams` (`detectSite`, variant
  parsing, `qualityUrl` ladders, a real tortuga token), `TestPagers` (chip
  window 7, 55/140-per-page labels, the numbers overload for cours starting
  mid-hundreds), `TestQualityCache` (TTL expiry, 240-entry oldest-first cap,
  page-lifetime `no_source`, `forget()`), `TestRateLimiter` (429 parking on
  `Retry-After`, reset expiry, queued callers, cancellation unwinding a parked
  caller inside the window), `TestLayering` (no Qt GUI headers in the domain, no
  app-layer names), `TestClipboard`, `TestDownloads` (a pre-existing
  destination survives a refusal byte-for-byte, exclusive creation, checked
  writes, digest/size/cancellation cleanup, redacted reasons),
  `TestDiagnostics` (every credential spelling and signed-URL shape is redacted,
  ordinary text passes through byte-for-byte, the log file itself carries no
  token), `TestApiClient` (a loopback server proves the stable error envelope
  survives with its HTTP status, a parked caller resumes with the new key and
  the new quota generation, `setApiKey` is atomic and a failed save keeps the
  old key) and `TestRunner` (destroying a Runner returns promptly, cancel
  retires queued work and unwinds a running one through the real backend gate,
  `shutdownAll` drains without blocking the GUI, and no delivery reaches a dead
  owner).
- `TestLiveOracle` (gated on `MIKAI_LIVE=1` + `MIKAI_API_KEY`): BLACK TORCH /
  «Чорний смолоскип» → ref `7428`; 7 releases with 4 dubs / 3 subs; one dub
  carrying episodes 1–12 with real embeds; a live 480/720/1080 ladder from
  `qualityStreams()`.
- The suite caught two real port bugs before cutover: `tortugaDecode` masking
  with `7*i` for `i` starting at 1 (every byte decoded with the next key
  value; playback broken for all tortuga episodes) and `finishCall` comparing
  the stored `remaining_` with itself (quota froze at the first value).
- Live oracle cost: a handful of API calls plus one stream extraction; the
  hermetic run needs no network and finishes in ~15 s.

### Observed acceptance state

**Remediation session (2026-10-11, latest observed results; the blocks below are
the earlier player-flow record).** Every finding in `known_bugs.md` was fixed and
the fixes were exercised. `cmake --preset release` configures, and `mikai`,
`mikai_tests` and every affected probe build with zero errors and zero warnings
under `-Wall -Wextra`. `ctest --test-dir build/release` is **4/4 PASS**:
`mikai_tests` (all suites PASS, `TestLiveOracle` SKIP without a key),
`NativeStageContracts` (86/86), `LauncherContracts` (real hung child: exit 3, the
child and its descendant gone by PID+creation-time identity, the owned scratch
removed, neighbouring files byte-identical) and `DistContracts` (83 checks,
including byte-preservation of a populated output). New suites carry the
remediation: `TestDownloads`, `TestDiagnostics`, `TestApiClient`, `TestRunner`
(21/21) and `TestRuntimeSelection`. `crash_probe --crash` wrote a real report
through the unhandled-exception filter with none of its planted tokens and the
redaction markers present; running a mutating native probe beside the populated
`build/release/native` refused and left the store byte-identical;
`widget_pixel_probe` passed 25/25 (0 skipped, the on-screen pixel readback
unavailable); a 25 s app smoke reached its runtime line about 0.7 s after
`mikai started` with no crash report; and `lrelease` reports 1016 finished, 0
unfinished translations.

**Latest composed evidence (2026-10-10):** the captioned chroma corruption was
diagnosed as Qt blending left enabled before libmpv rendering. The current
natural player builds fresh spatial 544×320 and RIFE 640×352 engines while
holding playback paused through visible completion, then resumes cleanly.
Compare passes 464 checks with the original frozen-frame close regression,
plus 9 native-window-close checks. Exact evidence and remaining private Native,
numerical-fidelity and licensing limits are in `native/README.md`.

The isolated test cutover builds without warnings/errors and passes 32 suites
with LiveOracle deliberately skipped in 19.40 s. Actual direct installed
invocation now refuses with exit 2 before any suite or fixture mutation.
The earlier unsafe installed test run required a verified local custom ABI-8
TensorRT closure recovery; its unavailable original install-record metadata
was not restored byte-for-byte. The recovered baseline remains unchanged after
the guard and actual installed-app startup/shutdown. Test-only files and
checksum-proven synthetic DirectML fixtures were removed. These current notes
supersede the earlier player/Compare gate and install-preservation snapshots
below; the native guide records the incident and exact boundaries.

**Earlier player-flow observations (historical baseline):** the
build recorded at `.probe-out/ui-audit-playback-failure-cause-build.log` compiled
`mikai` plus the test and probe tree with exit 0, zero errors and zero warnings.

The root fix behind it: a synchronous `MpvError` thrown by the surface's own open
was being overwritten by failed-open callers' generic "Could not play" text. All
eight callers now go through one central helper that preserves the named cause,
and no Aji plan is applied from a core that failed to open. A player refusal now
raises an accessible alert (`QAccessible::Alert`) in addition to the visible
status. On the baseline that alert did not exist at all — the gate observed 0 of
them and failed — and the real missing-runtime consumer then hit heap
`c0000374`; under GDB the fault came from `VideoSurface::~detach` emitting to an
already-destructing `PlayerDialog`, so the destructor now disconnects early and
guards on `closed_`.

Observed after the fix, at `.probe-out/ui-audit-runtime-refusal-cause-after.log`
(52 requirements, 0 skips, exit 0): the actual missing Root/DLL cause is named,
the request is actually dropped, and the close is clean. Real external-media
file-picker, HTTP, same-URL retry and file-URI playback passed 22/22
(`.probe-out/ui-audit-real-playback-after-failure-fix.log`, exit 0). Real
expired-signature HLS recovery passed 45 requirements
(`.probe-out/ui-audit-hls-recovery-after-failure-fix.log`, exit 0). The full
hermetic CTest suite after the same fix is 100% in 15.69 s
(`.probe-out/ui-audit-failure-cause-hermetic-after.log`).

The real refused-action gates also pass after the cause fix, on the current
binaries. The player clipboard gate covers 16 checks and the Search clipboard
gate 1 under the corrected hidden-HWND Windows-QPA proof; the Search browser gate
passes 1 check under `QT_QPA_PLATFORM=offscreen`. The title browser gate
(`.probe-out/ui-audit-title-browser-refusal-after.log`, exit 0) passes actual
action, visible warning, parent, accessible `Alert`, screenshot and
no-false-success checks with the session intact, and the title copy gate under
Windows QPA passes the same flags
(`.probe-out/ui-audit-title-clipboard-refusal-after.log`). Giving the offscreen
browser a readable font surface needs `C:/Windows/Fonts` on its font path; with
it the readable-surface run passes
(`.probe-out/ui-audit-title-browser-refusal-fonts.log`).

Current parent gates on the same post-fix binaries:

- `InteractionProbe` passes 172/172 with 0 skips and exit 0
  (`.probe-out/ui-audit-free-pip-live-intent-final.log`). M4's PiP change, which
  made an unrequested change to the live float, was reverted; the original
  behaviour is preserved: after entering the float playing, the probe explicitly
  pauses the float, captures its live state before `Restore`, and checks
  positions, controls, two non-corner free placements at different sizes, pooled
  reentry, and pass-through. The earlier 169/2 paused-restore result was a stale
  probe expectation, not a production bug. The chrome-idle wait now drives a real
  `QTest::mouseMove` with a bounded wait instead of a synthetic `QMouseEvent`
  followed by a 2800 ms sleep.
- The real operation-window API passes 69/69
  (`.probe-out/ui-audit-engine-operation-pause-after-cause-fix.log`).
- Settings passes 129 checks
  (`.probe-out/ui-audit-native-settings-catalog-after-cause-fix.log`, which needs
  the translation catalogue in the environment), RifeSettings 56
  (`.probe-out/ui-audit-rife-settings-catalog-after-cause-fix.log`) and
  NativeMenu 33 (`.probe-out/ui-audit-native-menu-after-cause-fix.log`).

Earlier composition binaries, which predate that fix, recorded the
operation-window gate at 66/66 requirements, the full PiP interaction probe at
171/171, and the hermetic CTest suite in 18.22 s. Their SearchPage browser and
clipboard outcomes are superseded by the after-fix gates above; Windows QPA is
used only by the clipboard gates, and the browser gates run offscreen.

The later attached-console runs close the selected-model fresh-build gate:
RIFE v4.14 lite at 576×320 and AnimeJaNai upscale at 512×288 both complete,
hold the real source paused with stable media/FBO until visible completion, and
resume afterward. The H.264 operation API gate passes 93/93. These results do not
close the original captioned-player or Compare corruption gates; exact evidence
is in `native/README.md`. The refused-action probe's own selector defect is fixed
and its gate now passes: it had used `findChild<QPushButton*>`
while the real control is a typed `PillButton`, so the probe now finds a
`PillButton` parent and drives it with `QTest::mouseClick`.

The pixel clock-0 defect is diagnosed and fixed: the capture deferred **one** seek
until `hasVideo` and `duration > 0`, cycled subtitles to none only for the
existing reference fixture, and waited for `QOpenGLWidget::frameSwapped` at the
target timestamp with the window active before capturing. On that basis a
windowed 1366×975 logical run at DPR 2 (a 2732×1950 screen) at 6.333 s /
frame 152 with NVDEC (`cuda[nv12]`) took the screen surface first and matched the
FBO on 5 of 5 captures with 0 unavailable
(`.probe-out/ui-audit-canonical-screen-first-windowed.log` and its PNG).
4,196,051 of 4,196,352 reference-lit pixels were covered — 99.99% — and the
inspected image is clean. The original main-window and Compare black blocks are
**still open**, and the user's first full-screen Alt-Tab occlusion is
acknowledged and excluded. The later full-screen all-black-screen run with a clean
FBO has an **unproven** cause; it must not be attributed to DWM or GDI as fact.
The main/Compare windowed diagnostic run is owned by the source owner and still
in progress.

The older 6.333 s / frame 152 report was reproduced against a known fresh
CPU-decoded reference at that canonical file, so the input file is not corrupt.
That leaves hardware decode versus rendering versus on-screen compositing
unproven, and no stage has been named as the cause.

The translation catalogue is rebuilt from production sources: `lupdate` extracted
997 source strings (7 new, 2 obsolete removed), 7 curated translations followed
`lrelease`, and the catalogues finished 997 / unfinished 0
(`.probe-out/ui-audit-catalog-production-build.log`). With that catalogue,
`mikai`, `mikai_tests` and `widget_pixel_probe` build with exit 0 and no
warnings or errors, and full CTest is 100% in 15.71 s
(`.probe-out/ui-audit-current-catalog-hermetic.log`).

The gated application was copied **in place** into `dist-mikai/mikai.exe` only, as
a frozen/gated source with a matching digest; no `dist` target was run. The three
install records — Rife 3.7, Aji 0.9 trt and TensorRt 3.7 — have unchanged hashes.
The actually installed executable starts, paints a visible Ukrainian feed and
closes normally (`.probe-out/ui-audit-installed-app-startup-current.log`, with the
inspected startup image at `dist-mikai/packaged-app-startup.png`).

Source corrections behind the pending runs: GUI-local `QPointer` completion
guards for player and settings work, a failed-open but truthful processing error
path that names the actual reason, an idempotent engine-pause restore that keeps
explicit Play/Pause intent, `ProcessingOverlay` hidden on an accepted close, and
`TitlePage` session/page lifetime plus named stream-recovery errors. The
runtime-cause and destructor-disconnect fixes above are runtime-proved; the rest
of these are source-complete without a fresh composed-binary proof.

**Earlier integration observations (2026-10-09; not final cleanup acceptance):** earlier
reports record the upstream component query (not an install), local custom Aji
import, 47-model plan-only smoke, and GUI evidence in artifacts 343/351/357/361.
Artifact 351's ZIP/NuGet-capable extractor initially rejected a valid Windows
archive path; artifact 357 completed DirectML download/install/verification.
Artifact 361 passed its then-current GUI/settings gate with zero failures in
13.68 seconds. That earlier union build had zero errors/warnings, CTest passed in
13.78 seconds, the Ukrainian catalog had 999 finished and zero unfinished,
and `dist` was rebuilt at stage 7. Packaged `mikai.exe` startup/shutdown and
unprocessed H.264 multitrack playback were observed; that baseline itself is
not processed-playback evidence. RIFE v4.14 lite artifact 366 and spatial
artifact 367 both passed their described fresh-build/render/control playback
runs for the selected models. RIFE's successful surface updates were 145
processed versus 73 baseline in three seconds at equal resolution; the 1.5x
threshold passed. Spatial output was 73 frames over three seconds (24 fps
unchanged), with a nonblack changed/moving OpenGL frame and controls passing.
The active-engine/model identity link remains inferred because Core has no
engine-ID event; numerical fidelity, all-47 inference, private Ready/Active,
and FPS improvement are not established. Spatial attempt a44 was a failed
probe on CRC filename filtering/early raw-output timing, corrected by the
passing artifact-367 run. ESC-1/2/3 clearance remains open. See `native/README.md` and `tasks.md` §1.6. The detailed
checks include historical results and must not be read as current acceptance.

The UI-cleanup handoff additionally records Settings 129, RifeSettings 57,
NativeMenu 33 and a cached selected-RIFE run with 23 checks, plus an isolated
five-spatial Compare run with 107 checks. Those precede the final every-preset
switching, engine-pause/progress, PiP resize/reentry and refused-action checks.
The operation gate (66/66) and the PiP interaction gate (171/171) were observed
on composition binaries that predate the native traversal correction, so they do
not close the fresh-engine completion/pause gate either; the current post-fix
gates (172/172 and 69/69) still leave that engine gate open. No final cleanup
count or current-tree acceptance is asserted here.

These are observed results, not a claim that everything is green:

- Historical inspection-off result: `MIKAI_WITH_MODEL_INSPECTION=OFF` built
  `mikai` and `mikai_tests` with zero warnings/errors under
  `-Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type`; CTest reported
  31 suites PASS plus `LiveOracle` SKIP in about 15 seconds (GNU 16.2, Qt
  6.8.3). The earlier full union build/CTest report is recorded above and in
  `native/README.md`; no new checks are implied here. No linter is configured.
  Inspection-on requires `ORT_INCLUDE_DIR`.
- The upstream AnimeJaNaiManager Chains/Presets handoff is a reference for its
  chain/default-profile vocabulary, not a request or claim that Mikai ports the
  full custom-profile editor or its shortcuts.
- **Historical 3.6.3 component-path exercise (not current 3.7.0 default-download
  evidence):** an earlier real-device probe observed the following:
  - `native_component_probe --print` reports `NVIDIA GeForce RTX 5090 - SM 12.0`
    and selects `trt-sm120` (229 417 183 B). Pointed at the installed bundle it
    resolves the bundle's own `trt` directory with `fromStore=0`, which is the
    fast path a machine with a fitted bundle must take.
  - `--fetch trt-runtime,trt-sm120` downloads both pinned packs and prints every
    installed file's size and SHA-256. The three the bundle also carries match the
    project SDK exactly — `nvinfer_11.dll` `ab5148bc…`, `nvonnxparser_11.dll`
    `823f7d6e…` and `nvinfer_builder_resource_sm120_11.dll` `7b616872…` — which is
    the strongest available confirmation that the historical `3.6.3` packs were this project's
    SDK.
  - The choice is load-bearing, not incidental: `--from-sdk` with the GPU's own
    `trt-sm120` pack builds a real engine (`RESULT: PASS`, 3 078 580 bytes of raw
    plan, exit 0), and the same command with `trt-sm75` fails, exit 4, naming
    `Unable to load library: nvinfer_builder_resource_sm120_11.dll: 126`.
  - `engine_popup_probe` reports **13/13 requirements PASS**, including that no
    window exists before a build is announced, that the bar is indeterminate
    before a stage count exists, that Cancel reaches the host, that a failure
    stays up naming its reason, and that a finished build dismisses its own
    window.
- **Startup cost, measured from the app's own log** (`mikai started` → the
  runtime line): **23.8 s before the validation cache, 18.1 s on a cold cache,
  and ~0.6 s on every start after.** The first start after a redeploy re-hashes
  once, because `dist.cmake` re-copies the tree and every modification time moves.
  - **The dist folder was assembled and inspected**: `mikai.exe`, its Qt runtime and
  plugin folders, 49 shaders, `translations/`, `run.bat`, `VERSION` (git revision +
  UTC date), and `mpv/libmpv-2.dll` only — and **zero** `components/` or
  `assets/` directories, which is the rule that no downloadable element is shipped.
- Historical `objdump -p` inspection of `mikai.exe` from both the default/OFF and
  review/ON builds lists imports of exactly `Qt6Core`, `Qt6Gui`, `Qt6Network`,
  `Qt6OpenGLWidgets`, `Qt6Widgets`, `KERNEL32`, `msvcrt` and `SHELL32` — **no**
  `libmpv-2.dll`, ONNX Runtime, CUDA, TensorRT or DirectML import. The app
  dynamically loads its packaged `mpv/libmpv-2.dll` at runtime; it does not
  carry the native GPU stack. This supersedes the earlier `dumpbin /imports`
  observation.
- A real **640×360** colored moving frame (`testsrc2` + motion) rendered through
  the explicitly loaded module with the GL debug logger active: **32 messages,
  no `ErrorType` message, `glGetError` 0**. Perf-warning messages remain — this
  is not a warning-free run. The GL-state invariant behind it is the
  `VideoSurface` note above. The widget probe built on this scene passed 11
  requirements on a fresh binary at 640×360 with `dpr=2` and 29.60% motion.
- **The M42 metric gate is not closed — a probe-observability gap, not a
  missing player-chip feature.** The widget probe cannot observe raw
  `estimated-vf-fps` / drop counters through `VideoSurface`, so it prints them
  as unavailable; reported stream FPS can fall back to container FPS, and only
  the headless-owned `MpvHandle` reads the raw properties.
- The optional MSVC native build produced an out-of-band preparation helper that
  builds a plan **in-process** through the TensorRT builder API — no `--trtexec`,
  no child process — and links `nvinfer_11`/`nvonnxparser_11`/`cudart` rather
  than the shim. `native_prep_probe` against the real helper reports
  `RESULT: PASS` on six behaviours: existing output refused and unchanged, a
  corrupt graph exiting `5` with no engine-shaped file, an absent GPU UUID
  refused instead of falling back, `build-started` emitted before the build
  proceeds, cancelling mid-build publishing nothing, and unchanged prior
  artifacts and source bytes. A real 64x64 build on the selected RTX 5090 exited
  `0` and wrote **1 380 548 bytes** of raw plan. `mikai_native_verify` then
  **rejects** that engine: bindings and the real GPU enqueue succeed and RMSE
  `0.00215017827` passes, but `maxAbsoluteError` `0.02734375` exceeds the `0.02`
  FP16 tolerance. The RIFE matrix splits: all four graphs run a real CPU
  reference and all four serialize on GPU; **lite** (`maxAbs` `0.0122070312`)
  and **lite ensemble** (`maxAbs` `0.00341796875`) verify numerically, **full**
  (`maxAbs` `0.0561523438`) and **ensemble** (`maxAbs` `0.0358886719`, exit `9`)
  are rejected. A verify pass covers raw engine bytes, bindings and numerics only
  (`manifestVerified: false`, `prepared: false`), so **nothing is `Ready` or
  `Active`**. The rejection diagnosis is unresolved and the bounds are not to be
  widened. The pinned mpv fork now builds
  (273 steps, Release) and its isolated core loads in the matching stage closure
  (exit `0`, client/header API `131077`, mpv `v0.41.0-UNKNOWN`, all swap /
  transport / correlation cases `RESULT: PASS`); the original `Win127` missing
  symbol is **unresolved**. Bundle candidate verification currently **aborts on
  a `GGML` initializer** after the load/unload-all-helpers sweep (fix in flight,
  no PASS yet, no M05 completion). A **Lua-enabled candidate** (LuaJIT
  `2.1.1788460057-2`, 248-step `-Dlua=luajit` core build) does now **pass** an
  actual Native-off GUI surface after full candidate hash validation: the
  Native-kind candidate is explicitly selected as `0.41.0-native.1`, a real
  320×180 HEVC 24 fps video plays with `contextReady` true, position
  `0.542 → 2.542` over 12 s, **921600/921600** non-black samples,
  **290264/921600** motion, **0** decoder and **0** VO drops, **0** live `vf`
  chain entries, and the inspected `mpv_surface_smoke.png` (1280×720) is a
  visibly colored motion test pattern. `widget_pixel_probe` on the same candidate
  also passes **21/21** with 0 skips and exit `0` (colored 640×360 PNG, GL 4.6,
  32 GL-logger messages, 0 `ErrorType`, `glGetError` 0, 30.44% motion); late
  events after close remain unknown and the 256×144 15 fps reopen refreshes
  metrics and the `vf`-absence check. That is **real Qt/OpenGL through the owned
  native runtime with Native off** — **not** the native filter, **not** CUDA
  interop, **not** active inference and **not** task completion — so it does not
  install a bundle and nothing is `Ready`. With Ninja this build must be configured
  `-DCMAKE_BUILD_TYPE=Release`; `--config Release` alone leaves a Debug cache and
  the helper fails with `0xC0000135`.
- Earlier attached-session desktop probes passed on their recorded binaries:
  `interaction_probe` 149/149 (`RESULT: PASS`), `hls_recovery_probe`
  (`RESULT: PASS`, real expired-signature recovery), `external_media_probe`
  (`RESULT: PASS`, real picker, HTTP, same-URL retry and file-URL paths), and
  `compare_probe` 30/30 (`RESULT: PASS`, frozen-frame proof, 0.000/255 noise
  floor). Later composition binaries passed `InteractionProbe` 171/171 and then
  172/172. `anime4k_menu_probe` was rebuilt on semantic locators (no
  `objectName`/tooltip selectors) and reran as full PASS with 0 failures: all
  24 preset leaves selectable, effect 1.027/255 against a 0.0000 floor, and a
  reopen restore of 0.999/255. Those binaries all predate the native traversal
  correction and the runtime-cause fix, so none of them is the current tree;
  the current external-media, HLS and refused-action outcomes are recorded in
  the player-flow block above. Pixel proofs additionally need the session
  attached (detached: every count reads black with `hasVideo=0`).

### Native track: owner grant for project-owned integration (2026-10-04); third-party terms not verified; built locally, not shipped

The project owner granted unrestricted use of this project's own native integration, with no restrictions (2026-10-04, in-session), recorded at `LICENSE` (The Unlicense). The grant covers this project's own integration code, its patch series and its recorded decisions only, and is recorded in `native/manifest/components.json#projectIntegrationGrant`. It does not relicense or verify any third-party component: Qt, mpv, FFmpeg, NVIDIA CUDA/TensorRT/cuDART, ONNX Runtime, DirectML and the upstream AnimeJaNai inference sources/models keep their own upstream terms, recorded per component in `native/manifest/` and the acquisition records. In particular the third-party inference shim's upstream terms are **not verified** — no `LICENSE` located, the GitHub licence field is `null`, and no upstream grant has been obtained — and the draft request in `native/permission-request.md` remains unsent.

The guide is `native/README.md`; acquisition records are
`native/sdk-acquisition.md`, `native/rife-acquisition.md`,
`native/spatial-acquisition.md` and `native/videojanai-acquisition.md`. Digests,
pins and licence reasoning live in those files and in `native/manifest/` — do not copy hash tables into this file. Components with no verified upstream licence must be recorded as "upstream terms not verified" (not "OpenSource", not "blocked").

The installed bundle carries only the sm120 TensorRT builder resource, so on any
other GPU `createInferBuilder` fails. That gap is closed by **provisioning rather
than by shipping**: the app downloads the pinned AnimeJaNai `3.7.0` packs on the
machine that needs them, proves each by digest, and stores them in the app-owned
`%LOCALAPPDATA%` component store. It redistributes nothing — no dist folder, no
installer and no repository content carries those bytes — and the same is true of
the pinned model graphs. The digests and the corroborating measurements are in
`native/manifest/animejanai-release.json`; the bundle-inventory gap entry in
`native/manifest/components.json` now names this path instead of leaving an open
gap. This resolves the *packaging* question only, and leaves ESC-1/ESC-2/ESC-3
untouched.

The findings live in
`native/manifest/components.json`; read its `escalations` there rather than a
summary. ESC-1, ESC-2 and ESC-3 remain **open/`blocking`** and leave the Native
bundle redistribution `blocked`: the upstream inference repository still
declares no licence and its GitHub licence field is still `null`, the CC
BY-NC-SA spatial graphs are not cleared for redistribution under the recorded
scope, and the RIFE whole-graph artefact rights are unresolved. The owner's
2026-10-04 grant clears this project's own integration code only and must never
be presented as an upstream licence. The RIFE v4.14 graphs *are* acquired and
their stored weights match the author's MIT checkpoints after fp16 rounding,
but that does not settle redistributing the graph files. ESC-4 is resolved by
obtaining the DirectML package. ESC-5 (`trtexec.exe`) is resolved by
architecture: the preparation helper builds a plan **in-process** through the
TensorRT builder API and never invokes `trtexec` at all — it is neither a
build-time input nor a run-time dependency — and that resolution would lapse if
the design ever shelled out to `trtexec` at run time.

A grant is not a capability, and local work is not a shipment. ESC-1/2/3
remain open. Current evidence does not establish private Native `Ready` or
`Active`, numerical model fidelity, all-47 inference, or engine-to-active-model
identity beyond the observed configuration (Core has no engine-ID event).
Artifact 366 passed the selected RIFE v4.14 lite fresh-build/render/interactions
and relative presentation-output-count gate; artifact 367 passed the selected
spatial fresh-build/render/control run. These specific-model results do not
prove all models or private Ready/Active.
The optional native tools and private runtime/bundle work are local research
artifacts; their existence does not clear third-party redistribution. Older
RIFE numerical verification/rejection and Native-off surface runs are
historical, not outcomes for the current engine attempts. The current official
filter UI exposes spatial and RIFE selections; artifact 343 passed language
access, verification preservation, refresh, review and backend persistence.
Artifact 351 initially failed DirectML extraction on an unsafe-path rejection;
artifact 357 passed DirectML download/install/verification; artifact 361
passed its then-current GUI/settings gate. The selected spatial a46 pixel/control
run later passed as artifact 367;
private Ready/Active is not established. Final UI-cleanup verification is pending.
The 47-model planner passed a plan-only smoke including v4.26 heavy; it did not
request an engine build. Public Aji downloads remain pinned to upstream v0.9.0;
the explicit local custom ABI-8 import is not the default download. A genuine
H.264 Wasapi/multitrack playback baseline preceded the first Aji request and
does not itself establish processing; artifacts 366/367 later passed selected-
model Aji Qt playback runs. Keep the standard application build
distinct from the optional Native SDK/build path.
