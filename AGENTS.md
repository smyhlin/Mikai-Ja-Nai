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
- `MainWindow` is a `QStackedWidget` holding one `SearchPage` and at most one
  `TitlePage`; disposal shuts the title page down (closing every open player
  dialog and flushing the quality cache) before `deleteLater()`.
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
  bindings off (the keyboard belongs to the shell).
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
  - `Runtime.hpp/cpp` — libmpv discovery (`%LOCALAPPDATA%/MPVBackend/runtime`).
- `src/app/` — the Qt Widgets frontend, namespace `mikai::app`:
  - `main.cpp` — entrypoint (Fusion style, one `setStyleSheet`,
    `provisionMpvRuntime()` puts the mpv dir on `PATH` before the first
    `QApplication`, then `MainWindow` over a `MikaiClient`).
  - `pages/` — `MainWindow`, `SearchPage` (home discovery, async poster
    thumbs, format bar, 55-per-page search pager, per-card detail fan-out),
    `TitlePage` (per-release tabs, 140-episode grid, fetch-on-open qualities,
    in-process `PlayerDialog` via `Host`, episode context menu,
    external-player hand-off via `QProcess::startDetached(mpv|vlc)`).
  - `widgets/` — `TitleCard` (360×140 home card), `RangePager`,
    `RateLimitLabel` (RPM indicator).
  - `dialogs/` — `SettingsDialog` (API key + quota + feed toggles),
    `CoverViewer` (chromeless full-screen cover with zoom/pan).
  - `state/` — `Feeds` (home-block toggles), `QualityCache`.
  - `workers/` — `Runner` (pools 4/3/12).
  - `theme/` — `Colors.hpp`, `Metrics.hpp`, `Theme.hpp/cpp`
    (`buildStylesheet()`), `Icons.hpp/cpp` (painted glyphs, never emoji).
  - `Clipboard.hpp`, `Labels.hpp/cpp` (status labels).
- `src/player/` — the player: `MpvHandle` (only place calling `mpv_*`) →
  `MpvRenderContext` → `VideoSurface`; `ShellTheme`, `ShellWidgets`,
  `ShellSliders`; `PlayerShell` (chrome, display states, zoom/pan, input);
  `PlayerPopups` (`MenuPopup`, `QualityPopup`, `TrackPopup`,
  `SeriesDrawer`); `PlayerDialog` (composition root + `PipFloat`).
- `src/probes/` — hand-run probes (`mpv_surface_smoke`, `shell_probe`,
  `dialog_probe`, `widget_pixel_probe`, `headless_pixel_probe`,
  `InteractionProbe`) that build a real window and check pixels. CMake
  targets, not tests; nothing in `tests/` runs them.
- `tests/cpp/` — `mikai_tests` (one exe, one ctest entry; `main.cpp` drives
  each suite and prints a per-suite PASS/FAIL line): `TestModels`,
  `TestReleases`, `TestStreams`, `TestPagers`, `TestQualityCache`,
  `TestRateLimiter`, `TestLayering`, `TestClipboard`, `TestLiveOracle`
  (gated on `MIKAI_LIVE=1` + `MIKAI_API_KEY`: BLACK TORCH → ref `7428`, 7
  releases with 4 dubs / 3 subs, one dub carrying episodes 1–12, and a 480 /
  720 / 1080 ladder).
- `skills/design-ui/` — `SKILL.md` + `references/`.

## Development Commands

Toolchain: MinGW-w64 g++ (`C:/temp/w64dev/w64devkit/bin`), Qt 6.8.x
(`C:/temp/qt6/6.8.3/mingw_64`), mpv runtime dir (headers + `libmpv-2.dll`;
`MPV_RUNTIME_DIR` has no default and CMake hard-fails without it). This Qt
build needs `QT_PLUGIN_PATH=<qt>/plugins` in the environment or TLS reports 0
and every HTTPS call fails.

```sh
cmake -S . -B build-cmake -G "MinGW Makefiles" \
  -DCMAKE_MAKE_PROGRAM=<w64devkit>/bin/make.exe \
  -DCMAKE_CXX_COMPILER=<w64devkit>/bin/g++.exe \
  -DCMAKE_PREFIX_PATH=<qt>/6.8.3/mingw_64 \
  -DMPV_RUNTIME_DIR="%LOCALAPPDATA%/MPVBackend/runtime"
cmake --build build-cmake --target mikai
scripts\run.bat          # launches build-cmake/mikai.exe
ctest --test-dir build-cmake -R mikai_tests   # hermetic + live oracle when keyed
curl "https://api.mikai.me/public/v1/meta"   # API sanity check
```

`mikai.exe` links Qt dynamically, so the runtime has to sit beside it or be on
`PATH`: `build-cmake/` already carries a `windeployqt` drop plus
`libmpv-2.dll`; for a fresh tree run `windeployqt build-cmake/mikai.exe` and
copy `libmpv-2.dll` in from `MPV_RUNTIME_DIR`. `mikai.exe` is
`WIN32_EXECUTABLE` (no console); diagnostics go to the debugger output.

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
- `src/app/main.cpp` — entrypoint; `provisionMpvRuntime()` runs before the
  first `QApplication` because libmpv links at load time.
- `src/backend/MikaiClient.hpp` — `RateLimiter` + `MikaiClient`; header
  documents the Cloudflare notes (lowercased headers, mandatory User-Agent).
- `CMakeLists.txt` — targets `mikai_player` (STATIC), `mikai_backend`
  (STATIC), `mikai_app` (STATIC), `mikai` (WIN32 exe), `mikai_tests` + probes.
- `docs.md` — authoritative API contract (Ukrainian); `.env` — single
  `X-API-Key: mk_…` line, never commit.

## Runtime/Tooling Preferences

- C++20, Qt 6.8.x, MinGW-w64. No Python anywhere: no `pyproject.toml`, no
  `uv.lock`, no `.venv`, no `pytest`. The only runtimes beside Qt are the mpv
  runtime dir and the deployed Qt DLLs next to the exe.
- The API key is optional and only raises limits. Resolution order in
  `MikaiClient`: explicit argument → `MIKAI_API_KEY` env var → repo-root
  `.env`. `setApiKey` writes the `X-API-Key:` line back to `.env`.
- Windows-first: `%LOCALAPPDATA%/MPVBackend/runtime` is where the mpv runtime
  lands; non-Windows hosts rely on a system mpv. The app links `libmpv-2.dll`
  at load time, so the runtime dir must be on `PATH` before `main()` runs —
  `provisionMpvRuntime()` handles that.
- The repo is a git repository. The root `.gitignore` covers `.env`,
  `__pycache__/` (leftover bytecode only, untracked), `.venv/` (removed),
  `*.png`, and `build-cmake/` / `build-dbg/`; there is no CI pipeline, no
  Dockerfile, and no linter, type checker, or coverage config.

## Testing & QA

```sh
ctest --test-dir build-cmake -R mikai_tests
```

- `tests/cpp/main.cpp` drives every suite in turn, prints one
  `[mikai_tests] <Suite>: PASS/FAIL (rc=…)` line per suite to stderr (QTest
  itself stays silent on success), and accepts a suite name to run just one:
  `build-cmake/mikai_tests.exe TestStreams`. `MIKAI_SOURCE_DIR` lets the
  layer guards read the sources they police instead of passing vacuously.
- Hermetic suites: `TestModels` (53 parse fixtures: ref fallback
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
  hermetic run needs no network and finishes in ~4 s.
