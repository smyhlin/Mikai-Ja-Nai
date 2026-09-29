# Mikai Wrapper

A C++/Qt6 desktop client for the [Mikai public API](https://api.mikai.me/public/docs).

## Run

```powershell
cmake -S . -B build-cmake -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM=<w64devkit>/bin/make.exe -DCMAKE_CXX_COMPILER=<w64devkit>/bin/g++.exe -DCMAKE_PREFIX_PATH=<qt>/6.8.3/mingw_64 -DMPV_RUNTIME_DIR="%LOCALAPPDATA%/MPVBackend/runtime"
cmake --build build-cmake --target mikai
scripts\run.bat
```

`MPV_RUNTIME_DIR` is the mpv runtime directory (headers plus `libmpv-2.dll`);
CMake hard-fails without it. `scripts\run.bat` just launches
`build-cmake\mikai.exe`; `scripts/PACKAGING.md` covers deploy and the test run.

## What it does

- **Home** — carousels for titles you recently opened, titles airing now, and Mikai's current popular picks, plus the latest episode updates and the weekly airing schedule. Every card shows the poster, the airing status and the aired/planned episode count, and a search result row reads the same way (`Бліч · 2004 · TV · Завершено · 366 серій`) straight from the list response, with no extra request; right-click a card to open it, copy its mikai.me link or raw id, or open the site page in a browser.
- **Title** — description, cover, scores, genres and related titles, then one tab per release (voice or subtitles, grouped by dubbing team). Each episode opens the player at the qualities the provider actually has; the right-click menu adds copy-URL-only, re-extract, and hand-off to an external player. The cover opens full size with pan and zoom.
- **Player** — a full-screen player built around the same Qt event loop as the app: the `PlayerDialog` composition root drives the `PlayerShell` chrome and the mpv-backed `VideoSurface` in-process. The picture fills the screen edge to edge, the controls float over it and fade away after a couple of seconds of stillness, returning on the next mouse move. Click the picture to pause, step through episodes from the drawer (S), switch the dub/sub track (T) or the stream quality (Q) without leaving the player. A picture-in-picture float drops the video into a small always-on-top window with its own overlaid controls — click to pause, hold and drag to move it.
- **Settings** — the API token with its live quota, plus a checklist of the home blocks to load. A block you switch off is never requested, which is the cheapest way to cut API spend on a slow or metered connection.

Qualities are extracted once per episode, only when that episode opens, and the resulting stream URLs are cached for half an hour, so reopening a title repaints instead of re-hitting every provider. The links are signed and do expire; if a stream fails to play the player re-extracts it once on its own before reporting the error.

## API limits

The **RPM** indicator beside Settings shows the server-reported requests used in the current minute and the limit that applies. Calls wait for the server's reset window instead of failing when the allowance is spent. An optional API key raises the standard limit from 60 to 300 requests per minute and is stored locally in `.env`, which is never committed.

## Layout

`src/backend/` is the domain — `MikaiClient` (rate-limited Qt Network calls), models, HLS extraction (`ashdi`/`moon`/`tortuga`), libmpv discovery — and `src/app/` is the Qt Widgets frontend (`MainWindow`, `SearchPage`, `TitlePage`, dialogs, theme tokens), with the player (`src/player/`: `PlayerDialog` → `PlayerShell` → mpv-backed `VideoSurface`) driven in-process through a `PlayerHost` bridge. `docs.md` is the authoritative API contract and `AGENTS.md` the map for contributors: architecture, conventions, build commands and the testing setup. Run the checks with `ctest --test-dir build-cmake -R mikai_tests`.
