# Repository Guidelines

## Project Overview

PySide6 desktop client for the Mikai public anime API (`https://api.mikai.me/public/v1`). Home feed with carousels (recently opened / ongoing / popular) plus updates and schedule rows; search with a format filter bar; a title page with detail, related titles, per-release tabs, and per-episode quality chips that either copy the stream URL or open the built-in mpv player. `docs.md` is the authoritative API contract; `.env` holds the optional key; `skills/design-ui/` carries OS-native UI guidance (PySide6 / Slint first).

## Architecture & Data Flow

Layering is strict and enforced by tests:

- `mikai_backend/` imports only stdlib + `requests`. It must stay importable with Qt absent (`tests/test_units.py::test_backend_loads_without_qt_runtime`, `::test_backend_source_has_no_gui_imports`).
- `mikai_ui/` is the only Qt layer and depends on the backend through the `AnimeRepository` protocol — never through `MikaiClient` directly.
- UI never blocks the GUI thread: pages submit callables to `Runner.run(fn, on_ok, on_err)` (`mikai_ui/workers.py`). `Runner` owns a `QThreadPool` capped at 4 threads and keeps `Task` refs alive until `TaskSignals.finished/failed` fires, so results always land on the main thread.
- `MainWindow` is a `QStackedWidget` holding one `SearchPage` and at most one `TitlePage`; `_dispose_title()` calls `TitlePage.shutdown()` (closes every `PlayerDialog`, bumps `_extract_seq`) before `deleteLater()`. `closeEvent` disposes the title page.
- `TitlePage` maps `releases[]` (`kind` = `voice`/`sub` + `teams`) through `mikai_backend.releases.sort_releases`, then renders one `QTabWidget` tab per release. Tabs are strictly isolated: each holds only its own embed URLs, chips, and quality state. Episodes paginate in pages of `EPISODES_PER_PAGE = 12` with `←` / `→` buttons, so HLS extraction touches at most 12 episodes for the active page.
- Quality extraction is per-chip and lazy: `streams.get_quality_streams(page_url) -> {height: url}` runs on the `Runner`, and every callback is guarded by a `(extract_seq, seq)` token pair so a stale result can never repaint a chip after a tab or page switch. Results are never cached across requests — signed URLs expire.
- Player layering, one direction each: `PlayerControlsWidget` (transport/timeline/audio/speed/quality/fullscreen intent) → `PlayerController` (commands, immutable `PlaybackState`) → `MPVAdapter` (the only module that imports `mpv`, lazily inside `_initialize`). `PlayerDialog` is the composition root; `PlayerOverlayWidget` owns loading/error cards, toasts, and fullscreen auto-hide; `FullscreenManager` does Qt fullscreen and restores the saved geometry. `tests/test_player_architecture.py` pins this separation with a `FakeMpvAdapter`.
- API envelope: `{ok: true, result, total?, page?, pages?}` or `{ok: false, error: {errorCode, code, message}}`. `MikaiClient._get` raises `MikaiApiError(code, message, status)`; branch on the stable `error.code` string, never on HTTP status alone.
- `MikaiClient` gates every call through a `threading.Condition` rate limiter (`_acquire_api_slot` / `_finish_api_call`) driven by `X-RateLimit-Limit/Remaining/Reset` and `Retry-After`; a 429 parks callers until the advertised window instead of failing. `rate_limit_state()` feeds the RPM label and the settings quota panel.
- Identity: `{ref}` is a bare Mikai id or `mal:21` / `al:21` / `hikka:…` / `slug:…`. Incremental sync via `/updates`, never full-catalog polling.

## Key Directories

- `mikai_backend/` — Qt-free domain, reusable from any frontend:
  - `client.py` — `MikaiClient` (`search`, `browse_ongoing`, `browse_popular`, `get_anime`, `get_player`, `get_updates`, `get_schedule`, `fetch_bytes`, `get_api_key`, `set_api_key`, `validate_api_key`, `rate_limit_state`) plus the rate-limit gate and `.env` persistence.
  - `models.py` — frozen dataclasses (`AnimeSummary`, `AnimeDetail`, `Release`, `Episode`, `Source`, `Team`, `SearchResult`, `UpdatesResult`, `ScheduleResult`, `PlayerResult`, …) with `display_name`, `thumb_url` (small), `poster_url` (big), `cover_url` (UA posters first, then the normal poster), `external_links`.
  - `ports.py` — `AnimeRepository` runtime-checkable Protocol; this is the DI seam and the fakes' contract.
  - `releases.py` — `playable_episode_count`, `sort_releases` (voice → sub, then most playable episodes, then team name).
  - `errors.py` — only `MikaiApiError(code, message, status)`.
  - `streams.py` — HLS extraction per site (`ashdi.vip/vod/`, `moonanime.art/iframe/`, `tortuga.tw/vod/`): `detect_site`, `fetch_text` (20s timeout, 2 retries on 429/5xx honoring `Retry-After`), `ashdi_master`, `moon_master`, `tortuga_decode`, `tortuga_master`, `extract_master`, `parse_variants`, `get_quality_url`, `list_segments`, `get_quality_streams`; `NoSourceError` marks a provider that has no video for an episode.
  - `mpv_runtime.py` — Qt-free libmpv discovery/download/extraction (latest x64 dev `.7z` from shinchiro/mpv-winbuild-cmake via the GitHub Releases API, py7zr with a fallback to a system or auto-downloaded `7zr.exe` because the dev archive uses BCJ2, DLL-dir registration + `ctypes` preload, cache in `%LOCALAPPDATA%/MPVBackend/runtime`).
- `mikai_ui/` — PySide6 frontend: `main.py` (`main()`, `SearchPage`, `MainWindow`), `title_page.py` (`EpisodeGrid`, `TitlePage`), `settings_dialog.py`, `rate_limit_label.py`, `mpv_backend.py` (`MPVAdapter`), `player_controller.py` (`PlaybackStatus`, frozen `PlaybackState`, `PlayerController`), `player_controls.py`, `player_overlay.py`, `fullscreen_manager.py`, `player_dialog.py`, `theme/` (`colors.py`, `metrics.py`, `widgets.py`, `build_stylesheet()` in `__init__.py`), `workers.py` (`Runner`/`Task`).
- `tests/` — `conftest.py` (fixtures + `FakeMpvAdapter`), `test_units.py` (hermetic), `test_player_architecture.py` (hermetic), `test_app.py` (live API).
- `skills/design-ui/` — `SKILL.md` + `references/{refined-ui,typography,surfaces,animations,performance}.md`.

## Development Commands

Package manager is `uv` (PEP 621, hatchling build backend, venv at `.venv`, Python 3.14.3 from `.python-version`):

```sh
uv sync                  # create/refresh .venv, install deps + project (dev group incl. pytest)
uv run mikai-wrapper     # launch the GUI
uv run python -m mikai_ui  # equivalent entry point (mikai_ui/__main__.py)
scripts\run.bat          # Windows launcher: guards on uv, then uv sync + uv run mikai-wrapper
uv run pytest tests/ -v  # hermetic units + player architecture + live-API offscreen UI
curl "https://api.mikai.me/public/v1/meta"   # API sanity check
```

If the Windows `uv.exe` launcher reports `uv trampoline failed to canonicalize script path`, `python -m uv` hits the same failure. Run straight out of the existing venv instead: `.venv\Scripts\python.exe -m pytest tests/ -v`.

No linter, type checker, or coverage tool is configured. Formatting is the one exception: `[tool.ruff]` in `pyproject.toml` pins `line-length = 100` and `target-version = "py314"`.

```sh
uvx ruff format mikai_backend mikai_ui tests   # run before committing
uvx ruff format --check mikai_backend mikai_ui tests
```

The formatter is idempotent (a second run reports no changes) and the full hermetic suite passes afterward. Add no other tooling without a stated reason.

## Code Conventions & Common Patterns

API contract (when adding backend code):

- Error handling: branch on the stable `error.code` string. Known codes and statuses: `invalid_parameter` / `invalid_reference` / `too_many_ids` (422), `invalid_request` (400), `not_found` (404), `invalid_api_key` (401), `api_key_disabled` (403), `rate_limited` (429), `internal_error` (500). An unknown filter value returns 422, never an empty list.
- Rate limits: 60 req/min keyless (per IP), 300 req/min keyed (per key), no daily quota. Re-check live enums/limits via `GET /meta`; never hardcode.
- Naming: the API is camelCase, timestamps are RFC3339, episode numbers are fractional (`1`, `1.5`). Licensed titles return `licensed: true` with empty releases.
- Cache TTLs per `docs.md`: 1 h for dictionaries (`/genres`, `/tags`, `/studios`, `/meta`), 5 min for catalog/player/teams, 1 min for `/schedule`, `/updates`, `/posters/ua`, no-cache for `/me`, `/health`, `/anime/random`.
- Attribution: link mikai.me, preserve `team`/`author` credits, no bulk mirroring.

UI (`skills/design-ui/SKILL.md`, depth in `skills/design-ui/references/`):

- Tokens once, compose everywhere: no raw hex or magic numbers outside `mikai_ui/theme/`; every new value becomes a token in `colors.py` or `metrics.py`. Apply `app.setStyleSheet(build_stylesheet())` exactly once in `main()`.
- Native controls first (`QPushButton` / `QDialog` / `QMenu` / `QTabWidget` / `QTabBar`), styled through the QSS. Variant roles: primary / secondary / destructive. Compatibility aliases in `colors.py` (`BG`, `ELEVATED`, `FG`, …) exist for existing pages — keep new code on the canonical names.
- Rubric: ≤5 colors (1 primary + neutrals + ≤1 accent), ≤2 fonts, body line-height 1.4–1.6, 4/8-based spacing (`SPACING_*`), one primary action per view, override fg whenever bg is overridden, concentric radii (outer = inner + padding).
- Icons: inline SVG through `theme.widgets.make_icon(name, color, size)` — never emoji. HiDPI-aware sizing.
- Motion: 150–250 ms (`QPropertyAnimation`), opacity/position only, respect reduced motion. `ANIMATION_DURATION_MS`, `AUTO_HIDE_MS`, `TOAST_DURATION_MS` are the existing tokens.
- Min-window-first: no clipped controls, targets ≥24 px desktop / ≥44 px touch (`PRIMARY_CONTROL_SIZE = 44`). Ship loading / empty / error states.
- Async discipline: every async callback must be discardable. Use the existing sequence-token pattern, `QTimer.singleShot`, and `shiboken6.isValid(self)` before touching a widget that may have been deleted mid-flight.
- `logging`: `log = logging.getLogger(__name__)` is the established pattern in the UI modules; do not swallow exceptions silently.

## Important Files

- `mikai_ui/main.py` — entrypoint `main()` (Fusion style, one `setStyleSheet`, `MainWindow(MikaiClient())`), `SearchPage` (home discovery, async poster thumbs, format bar, pager), `MainWindow` (navigation + title disposal).
- `mikai_ui/title_page.py` — the largest module: release selector, isolated per-release player tabs, 12-episode pagination, quality chips, clipboard feedback, external-player hand-off.
- `mikai_ui/player_dialog.py` / `player_controller.py` / `player_controls.py` / `player_overlay.py` / `mpv_backend.py` / `fullscreen_manager.py` — separated playback components; `MPVAdapter` is the only python-mpv boundary.
- `mikai_ui/rate_limit_label.py` — RPM indicator; probes `getattr(repository, "rate_limit_state", None)`, so it degrades to `RPM —` on fakes.
- `mikai_ui/settings_dialog.py` — API key entry, token validation, live quota display; saves via `repository.set_api_key`.
- `mikai_ui/theme/` — `colors.py`, `metrics.py`, `widgets.py`, and `build_stylesheet()` in `__init__.py` (there is no `theme.py` module).
- `pyproject.toml` — deps, `mikai-wrapper` script, wheel packages, dev group; `uv.lock` — lockfile, never hand-edit.
- `docs.md` — authoritative API contract (978 lines, Ukrainian); `.env` — single `X-API-Key: mk_…` line, never commit.

## Runtime/Tooling Preferences

- Python 3.14.3 (pinned in `.python-version`; `requires-python = ">=3.14"`), installed via `uv python install 3.14`. Package manager `uv` with `uv.lock` committed; add dependencies with `uv add`, never by hand.
- Runtime deps: `PySide6>=6.10` (6.11.2 installed), `requests>=2.32` (2.34.2), `python-mpv>=1.0.8` (1.0.8), `py7zr` (1.1.3). Dev: `pytest>=9.1.1` (9.1.1) in the `dev` dependency-group.
- The API key is optional and only raises limits. Resolution order in `MikaiClient.__init__`: explicit argument → `MIKAI_API_KEY` env var → repo-root `.env`. `set_api_key` writes the `X-API-Key:` line back to `.env`.
- Windows-first: `mpv_runtime` auto-provisions libmpv, and the `.venv/Scripts/` folder contains `mikai-wrapper.exe` and `py7zr.exe`. Non-Windows hosts skip the auto-download and rely on a system mpv.
- UI stack is native PySide6 per `skills/`; do not introduce a web toolchain unless explicitly requested.
- The repo is a git repository on branch `main`. The root `.gitignore` covers `.env`, `__pycache__/`, `.pytest_cache/`, `.venv/`, and `*.png`; there is no CI pipeline, no Dockerfile, and no linter, type checker, or coverage config.

## Testing & QA

```sh
uv run pytest tests/ -v
```

- `tests/conftest.py` sets `QT_QPA_PLATFORM=offscreen` via `os.environ.setdefault` at import time (before PySide6 is imported), provides a session-scoped `app` fixture and a function-scoped `window` fixture that builds `MainWindow(MikaiClient())` against the real API. Shared helpers: `pump_until(app, cond, timeout=25.0)`, `tab_chips`, `pagination_controls`, `episode_button`, `quality_buttons`, and `FakeMpvAdapter` (injected via `PlayerController.attach_adapter`).
- `tests/test_units.py` — hermetic. 36 tests over a `FakeRepository`, with layer guards (backend imports no Qt, loads without a Qt runtime, fake satisfies the `AnimeRepository` protocol), client/domain parsing, mocked HLS extraction, player UI, search/home feeds, and the title-tab invariants.
- `tests/test_player_architecture.py` — hermetic. 5 tests pinning controller state transitions, seek preview/commit + clamping, volume/mute/speed/quality state restoration, controls reading only `PlaybackState`, and dialog shortcut/fullscreen routing.
- `tests/test_app.py` — live API. An autouse fixture monkeypatches `mikai_ui.title_page.streams.get_quality_streams` to `{}` so no stream extraction happens, except `test_live_stream_extraction_acceptance`, which hits four hardcoded provider URLs and asserts master playlists, 480/720/1080 variants, ≥5 segments, and MPEG-TS 188-byte packets.
- Live oracle: search «Чорний смолоскип» and `BLACK TORCH` both resolve to ref `7428`; the title page shows the description and cover; the release selector has 7 entries, 4 prefixed `Озвучення` and 3 prefixed `Субтитри`, with team substrings `Inari`, `SunDragon`, `Рідний Голос`, `Didko`, `Цукіхана`; releases are sorted with the most-voiced dub at index 0; the `Рідний Голос` voice tab exposes `Ashdi` and `MoonAnime`, each containing episode 12, with the union covering 1–12.
- Cost: expect roughly 75+ API requests and several hundred image fetches per full run against the 60 req/min keyless limit, because each `window` fixture fires the four home-feed calls. The client blocks rather than 429-spams, so a run is slow, not flaky. No coverage threshold exists. For visual checks also launch `uv run mikai-wrapper`: no clipped controls at min window, contrast holds, no anti-slop tells.
