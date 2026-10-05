# Repository Guidelines

## Project Overview

C++/Qt6 desktop client for the Mikai public anime API
(`https://api.mikai.me/public/v1`). One process, one Qt event loop: the app is
`build-cmake/mikai.exe`, built by the root `CMakeLists.txt`. Home feed with
carousels (recently opened / ongoing / popular) plus updates and schedule rows;
search with a format filter bar; a title page with detail, related titles,
per-release tabs, and a compact episode grid where a click fetches that
episode's qualities once and opens the built-in player. `docs.md` is the
authoritative API contract; `.env` holds the optional key; `skills/design-ui/`
carries OS-native UI guidance.

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
- `src/app/` is the only widget layer and talks to the domain through
  `mikai::backend::MikaiClient` (blocking calls; threads via `Runner`).
- UI never blocks the GUI thread: pages submit work to `Runner`
  (`src/app/workers/Runner.hpp`) with three `QThreadPool`s — shared (4),
  bulk (3) for feed fan-out so a title the user just opened is not queued
  behind the whole feed, and image (12) for artwork only. `fetchBytes`
  bypasses the client's rate limiter entirely, so posters cost no API budget
  and are pure latency. `waitForDone(ms)` drains bulk and image pools first,
  then the shared one.
  The owning widget parents `Runner`, which parents its pools. Its dispatcher
  has no QObject parent: only `shared_ptr` owns that allocation, including
  while completions are queued.
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
  top-left `MIKAI` chip shows `cache-speed`, `estimated-vf-fps`, and, while a
  preset is active, source → output resolution.
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

## Key Directories

- `src/backend/` — domain + Qt Network client:
  - `MikaiClient.hpp/cpp` — `MikaiClient` (`search`, `browseOngoing`,
    `browsePopular`, `getAnime`, `getPlayer`, `getUpdates`, `getSchedule`,
    `fetchBytes`, `apiKey`, `setApiKey`, `validateApiKey`,
    `rateLimitState`) plus `RateLimiter` and `.env` persistence.
  - `Models.hpp/cpp` — structs (`AnimeSummary`, `AnimeDetail`, `Release`,
    `Episode`, `Source`, `Team`, `SearchResult`, `UpdatesResult`,
    `ScheduleResult`, `PlayerResult`, …) with `displayName`, `thumbUrl`
    (small), `posterUrl` (big), `coverUrl` (UA posters first), `externalLinks`.
  - `Releases.hpp/cpp` — `playableEpisodeCount`, `sortReleases`.
  - `Errors.hpp` — only `MikaiApiError{code, message, status}`.
  - `Streams.hpp/cpp` — HLS extraction per site; `NoSourceError`.
  - `Runtime.hpp/cpp` — stock mpv discovery at runtime (`findRuntime`) and the
    auto-download provisioning (`ensureRuntime`) the recovery path may run
    (`%LOCALAPPDATA%/MPVBackend/runtime`). It finds a DLL; it never loads one.
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
    `CoverViewer` (chromeless full-screen cover with zoom/pan).
  - `state/` — `Feeds` (home-block toggles), `QualityCache`.
  - `workers/` — `Runner` (pools 4/3/12).
  - `theme/` — `Colors.hpp`, `Metrics.hpp`, `Theme.hpp/cpp`
    (`buildStylesheet()`), `Icons.hpp/cpp` (painted glyphs, never emoji).
  - `Clipboard.hpp`, `Labels.hpp/cpp` (status labels).
- `src/player/` — the player: `MpvApi` (the one dynamic module loader and its
  function table; `MpvHandle` is the only place calling `mpv_*`) →
  `MpvRenderContext` → `VideoSurface`; `ShellTheme`, `ShellWidgets`,
  `ShellSliders`; `PlayerShell` (chrome, display states, input; the picture is
  always fitted, there is no zoom or pan);
  `PlayerPopups` (`MenuPopup`, `QualityPopup`, `TrackPopup`,
  `SeriesDrawer`); `PlayerDialog` (composition root + `PipFloat`).
  Picture-in-picture has three rules that each cost a real bug, and all three
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
  Progress follows the float's own `positionChanged`, and `setTime` shows the
  position whenever it is known - blanking it because the *duration* was not
  threw away the only number a viewer can read.
  `Anime4K` — one declarative catalogue (id, label, hint, category, ordered
  chain) in `Anime4K.cpp` plus the menu tree above it: ten built-in presets
  (`Mode::Off`..`HqCA`, ids frozen), fourteen experiments grouped under
  Anime4K's own family names (Noise Reduction, Thin Lines, Texture
  Enhancement, Darken), and three planned-model submenus (RIFE, RealESRGAN,
  Інші моделі) each holding a disabled В планах leaf. `menuRows(parent)` is
  what the shell paints, one level per panel; a category row only opens the panel
  beside it and a placeholder row is disabled, so neither can change the chain. `shaderStringForCandidate(id)`
  is the only chain resolver, used for a mode and for a loose `.glsl` alike.
  `CompareWindow` — the shader comparison, reached from the picture's
  right-click menu. Its badge chooser mirrors the player menu's Anime4K tree:
  exactly the 24 preset ids and labels from `availableModes()`, under the same
  categories and experiment families. Loose `.glsl` files are not extra choices,
  so every compared model is selectable in the player, and it lets the
  viewer pick two and shows them in
  one of three layouts: side by side, one image with a draggable divide, or
  the amplified per-pixel difference.
  Four things about it are load-bearing, each found the same way the
  Anime4K pixel check was:
  - **A side is a chain applied to the live core, not a re-implementation.**
    `renderWithChain` writes the chain through `VideoSurface::setAnime4KChain`
    and grabs what comes back. Drawing the same image twice would look like a
    comparison and measure nothing.
  - **A comparison leaves the player as it found it.** The surface's own chain
    is read before the two sides render and written back afterwards, so the
    window never leaves the live core - or the stored setting it would be
    compared against - running a preview chain.
  - **The frame is frozen before it is compared.** Re-seeking to one
    timestamp does not reproduce the same pixels, so the window takes one
    frame and both sides render from it; two settled frames of a still picture
    differ by 0.000/255, and that is the noise floor a difference is read
    against. `compare_probe` asserts exactly that number.
  - **A chain is named by id, never by ordinal.** A single `.glsl` has no
    `Mode` at all, so `player/anime4k` stores the candidate id and the enum is
    only the menu's shorthand. A preset added later cannot repoint a choice
    the viewer already made.
  The window is non-blocking on purpose, and the float is barred from
  opening it: the float owns a second mpv core, and the window swaps the
  chain on whichever surface it is given.
- `src/player/native/` — runtime selection and the native vocabulary:
  `selectStartupRuntime()`/`resolveRuntime()`/`commitRuntime()` pick and pin
  one module per process (a later choice is refused in place with a restart
  reason), `NativeBundle`/`NativePaths`/`NativePeImage` validate a bundle
  before any load, and `NativeManifest`/`NativeRecipe`/`NativeStatus`/
  `NativePrefs`/`NativeMenu` carry the profile and recipe model. Compiled into
  the app unconditionally; `MIKAI_WITH_MODEL_INSPECTION` only adds the optional
  CPU inspector.
- `shaders/` — the 49 Anime4K `.glsl` presets, copied next to the executable
  by POST_BUILD rules on `mikai`, `mikai_tests` and shader-using probes.
  `Anime4K::findShadersDir` looks beside the
  exe and in the cwd; when it finds nothing the SHADERS section hides itself
  rather than offering rows that cannot work.
- `src/probes/` — hand-run CMake targets (`EXCLUDE_FROM_ALL`; nothing in
  `tests/` runs them). The GUI ones build a real window and check pixels:
  `mpv_surface_smoke`, `headless_pixel_probe`, `widget_pixel_probe`,
  `shell_probe`, `dialog_probe`, `InteractionProbe`, `anime4k_menu_probe`,
  `compare_probe`, `compare_shot_probe`, `external_media_probe`,
  `hls_recovery_probe`. The loader,
  selection and native-panel ones are `mpv_loader_probe`,
  `runtime_selection_probe`, `resolver_probe`, `native_status_probe`,
  `native_menu_probe`, `native_settings_probe`, `rife_settings_probe`,
  `spatial_assets_probe`, `native_prep_probe` and, only under
  `MIKAI_WITH_MODEL_INSPECTION`,
  `native_assets_probe`. `native_prep_probe` links only `Qt6::Core` and takes
  `<helper.exe> <graph.onnx> <GPU-uuid> <named-input-shapes>`, driving the
  native preparation helper against a real CUDA device; it is Windows-only and
  needs no interactive session. The
  loader/selection probes take `MIKAI_MPV_MODULE` when they need a specific
  DLL. Networked runs:
  `title_page_probe` drives the real `TitlePage` against the real API (its
  cover open/close loop pins the use-after-free) and `external_media_probe`
  needs a 320×180 clip plus its HTTP fixture; `crash_probe` installs the app's
  logging then faults on purpose, to prove a report is actually written.
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

  **A description is not a pass.** These paragraphs say what each probe
  *measures*; describe the behaviour a probe checks, never a specific string,
  theme number or default it happens to pin: those pin the implementation
  instead of the user-visible result. The desktop runs that are green are the
  ones with an observed `RESULT: PASS` line on a current-tree binary —
  `interaction_probe` 149/149, `compare_probe` 30/30, and `hls_recovery_probe`
  and `external_media_probe` — and pixel proofs additionally need the session
  attached (detached: every count reads black with `hasVideo=0`).
- `tests/cpp/` — `mikai_tests` (one exe, one ctest entry; `main.cpp` drives
  each suite and prints a per-suite PASS/FAIL/SKIP line). Domain and UI suites:
  `TestModels`, `TestReleases`, `TestAnime4K`, `TestI18n`, `TestStreams`,
  `TestPagers`, `TestQualityCache`, `TestRateLimiter`, `TestLayering`,
  `TestClipboard`. Runtime and native-contract suites: `TestRuntimeSelection`,
  `TestMpvAsync`, `TestNativeTypes`, `TestNativeRecipe`, `TestNativeProfiles`,
  `TestNativeRife`, `TestNativeBundle`, `TestNativePrepPlan`,
  `TestNativeManifest`, `TestNativeStatus`, `TestNativePrefs`,
  `TestNativeMenu`, `TestNativeUserRecipes`, `TestNativeAssets`, plus
  `TestNativeCatalog` only when `MIKAI_WITH_MODEL_INSPECTION=ON`. The live
  oracle `TestLiveOracle` is gated on `MIKAI_LIVE=1` + `MIKAI_API_KEY`
  (BLACK TORCH → ref `7428`, 7 releases with 4 dubs / 3 subs, one dub carrying
  episodes 1–12, and a 480 / 720 / 1080 ladder) and is a deliberate skip, not a
  pass, when the key is absent.
- `skills/design-ui/` — `SKILL.md` + `references/`.

## Development Commands

Toolchain: MinGW-w64 g++ (`C:/temp/w64dev/w64devkit/bin`), Qt 6.8.x
(`C:/temp/qt6/6.8.3/mingw_64`), and an mpv header directory holding
`mpv/client.h` and `mpv/render_gl.h` (`MPV_INCLUDE_DIR`, cached as `MIKAI_MPV_HEADERS`; no
default, and CMake hard-fails without it). The headers are build-only — the app never links libmpv
— and the optional native SDK is not needed for an ordinary build. This Qt
build needs `QT_PLUGIN_PATH=<qt>/plugins` in the environment or TLS reports 0
and every HTTPS call fails.

```sh
cmake -S . -B build-cmake -G "MinGW Makefiles" \
  -DCMAKE_MAKE_PROGRAM=<w64devkit>/bin/make.exe \
  -DCMAKE_CXX_COMPILER=<w64devkit>/bin/g++.exe \
  -DCMAKE_PREFIX_PATH=<qt>/6.8.3/mingw_64 \
  -DMPV_INCLUDE_DIR=<mpv>/include
cmake --build build-cmake --target mikai --parallel
build-cmake/mikai.exe       # run directly; no launcher script needed
ctest --test-dir build-cmake -R mikai_tests   # hermetic + live oracle when keyed
curl "https://api.mikai.me/public/v1/meta"   # API sanity check
```

`mikai.exe` links Qt dynamically, so the Qt runtime has to sit beside it or be
on `PATH`; a fresh tree needs `windeployqt build-cmake/mikai.exe`. The mpv DLL
is not a load-time import: the app resolves a stock runtime by discovery
(beside the exe or in `%LOCALAPPDATA%/MPVBackend/runtime`), so copy a runtime
tree into one of those places for playback. `mikai.exe` is `WIN32_EXECUTABLE`
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
- Windows-first: `%LOCALAPPDATA%/MPVBackend/runtime` is where a provisioned
  stock mpv runtime lands, and discovery also looks beside the exe; a probe or
  test can name one DLL with `MIKAI_MPV_MODULE`. The app does not link libmpv:
  `selectStartupRuntime()` resolves the module and `MpvApi` loads it
  explicitly, once, and pins it for the process. Many core handles (main plus
  the PiP float) share that one module; swapping to a different runtime is
  refused in place and needs a restart. Non-Windows hosts rely on a system mpv.
- The repo is a git repository. The root `.gitignore` covers `.env`,
  `__pycache__/` (leftover bytecode only, untracked), `.venv/` (removed),
  `*.png`, and `build-cmake/` / `build-dbg/`; there is no CI pipeline, no
  Dockerfile, and no linter, type checker, or coverage config.

## Testing & QA

```sh
ctest --test-dir build-cmake -R mikai_tests
```

- `tests/cpp/main.cpp` drives every suite in turn, prints one
  `[mikai_tests] <Suite>: PASS/FAIL/SKIP (rc=…)` line per suite to stderr (QTest
  itself stays silent on success), and accepts a suite name to run just one:
  `build-cmake/mikai_tests.exe TestStreams`. `MIKAI_SOURCE_DIR` lets the
  layer guards read the sources they police instead of passing vacuously.
- Hermetic suites: `TestModels` (parse fixtures: ref fallback
  `mikai/mal/al/hikka/slug:`, `displayName`, thumb/poster/cover order,
  `externalLinks`, episode int/double/string/null), `TestReleases`
  (playable count, voice→sub sort), `TestStreams` (`detectSite`, variant
  parsing, `qualityUrl` ladders, a real tortuga token), `TestPagers` (chip
  window 7, 55/140-per-page labels, the numbers overload for cours starting
  mid-hundreds), `TestQualityCache` (TTL expiry, 240-entry oldest-first cap,
  page-lifetime `no_source`, `forget()`), `TestRateLimiter` (429 parking on
  `Retry-After`, reset expiry, queued callers), `TestLayering` (no Qt GUI
  headers in the domain, no app-layer names), `TestClipboard`.
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

These are observed results, not a claim that everything is green:

- A fresh isolated inspection-off build (the default,
  `MIKAI_WITH_MODEL_INSPECTION=OFF`) built `mikai` and `mikai_tests` (exit 0),
  and `ctest --test-dir build-cmake -R mikai_tests` passed in **14.82 s**
  (14.83 s wall; one ctest entry, 24 suites PASS + `LiveOracle` SKIP). The
  isolated inspection-on matrix built the app, tests and the listed
  probes/helpers, and its ctest run passed in **15.17 s** (25 suites PASS +
  `LiveOracle` SKIP). Compiler GNU 16.2, Qt 6.8.3; no linter is configured.
- `objdump -p` on the current-tree `mikai.exe` from both the default/OFF and
  review/ON builds lists imports of exactly `Qt6Core`, `Qt6Gui`, `Qt6Network`,
  `Qt6OpenGLWidgets`, `Qt6Widgets`, `KERNEL32`, `msvcrt` and `SHELL32` — **no**
  `libmpv-2.dll`, ONNX Runtime, CUDA, TensorRT or DirectML import. The app
  carries neither the mpv DLL nor the native GPU stack; this supersedes the
  earlier `dumpbin /imports` observation.
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
- With the session attached, the desktop probes pass on current-tree binaries:
  `interaction_probe` 149/149 (`RESULT: PASS`), `hls_recovery_probe`
  (`RESULT: PASS`, real expired-signature recovery), `external_media_probe`
  (`RESULT: PASS`, real picker, HTTP, same-URL retry and file-URL paths), and
  `compare_probe` 30/30 (`RESULT: PASS`, frozen-frame proof, 0.000/255 noise
  floor). `anime4k_menu_probe` was rebuilt on semantic locators (no
  `objectName`/tooltip selectors) and reran as full PASS with 0 failures: all
  24 preset leaves selectable, effect 1.027/255 against a 0.0000 floor, and a
  reopen restore of 0.999/255. Pixel proofs additionally need the session
  attached (detached: every count reads black with `hasVideo=0`).

### Native track: owner grant for project-owned integration (2026-10-04); third-party terms not verified; built locally, not shipped

The project owner granted unrestricted use of this project's own native integration, with no restrictions (2026-10-04, in-session), recorded at `LICENSE` (The Unlicense). The grant covers this project's own integration code, its patch series and its recorded decisions only, and is recorded in `native/manifest/components.json#projectIntegrationGrant`. It does not relicense or verify any third-party component: Qt, mpv, FFmpeg, NVIDIA CUDA/TensorRT/cuDART, ONNX Runtime, DirectML and the upstream AnimeJaNai inference sources/models keep their own upstream terms, recorded per component in `native/manifest/` and the acquisition records. In particular the third-party inference shim's upstream terms are **not verified** — no `LICENSE` located, the GitHub licence field is `null`, and no upstream grant has been obtained — and the draft request in `native/permission-request.md` remains unsent.

The guide is `native/README.md`; acquisition records are
`native/sdk-acquisition.md`, `native/rife-acquisition.md`,
`native/spatial-acquisition.md` and `native/videojanai-acquisition.md`. Digests,
pins and licence reasoning live in those files and in `native/manifest/` — do
not copy hash tables into this file. Components with no verified upstream licence must be recorded as "upstream terms not verified" (not "OpenSource", not "blocked").

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

A grant is not a capability, and local work is not a shipment. **No
redistributable native bundle is shipped, installed, published or distributed**,
and no engine is `Ready` or `Active`. That is a shipping/clearance boundary, not
a claim that nothing is built: the optional MSVC `native/` project is under
active **private, local** development and its own tools build Release from the
pinned, patched source, and it has produced raw TensorRT plans on a real GPU,
while private bundle factory/staging work continues locally. None of that
redistributes the shim or any third-party component. Two RIFE engines (lite and lite ensemble) verify
numerically against the CPU reference; the spatial HD Performance engine and RIFE
full and ensemble are rejected on `maxAbsoluteError`. Nothing is published and no
complete prepared recipe exists. The pinned mpv fork core now builds (273 steps,
Release) and its isolated core loads in the matching stage closure (exit `0`);
the original `Win127` missing symbol is unresolved. Bundle candidate verification
still **aborts on a `GGML` initializer** after the load/unload-all-helpers sweep
(fix in flight, no PASS yet, no M05 completion). A Lua-enabled candidate
(LuaJIT `2.1.1788460057-2`) does pass a real **Native-off GUI surface** after
full candidate hash validation — a 320×180 HEVC video playing through the owned
runtime with 0 decoder/VO drops and an inspected colored motion PNG — but that
is the **runtime surface with Native off**, not the native filter, CUDA interop
or active inference. No native frame is rendered, no validated bundle is
installed, and nothing is `Ready`. The **RIFE / RealESRGAN / Інші моделі**
menu panels remain placeholders and no model in them executes, so the app
performs no native processing. Nothing in the native track is on the Qt/MinGW
build path: the Standard build and the optional CPU-only model inspector do not
depend on any of it.
