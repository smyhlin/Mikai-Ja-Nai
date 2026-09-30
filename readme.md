# Mikai Wrapper

A C++/Qt6 desktop client for the [Mikai public API](https://api.mikai.me/public/docs).

## Build

Needs MinGW-w64 g++, Qt 6.8.x, and the mpv runtime directory (headers plus
`libmpv-2.dll`). `MPV_RUNTIME_DIR` has no default and CMake hard-fails without
it. The toolchain paths have to be passed explicitly — a bare
`-G "MinGW Makefiles"` fails with `CMAKE_MAKE_PROGRAM is not set` when the
toolchain is not on `PATH`.

```sh
cmake -S . -B build-cmake -G "MinGW Makefiles" \
  -DCMAKE_MAKE_PROGRAM=<w64devkit>/bin/make.exe \
  -DCMAKE_CXX_COMPILER=<w64devkit>/bin/g++.exe \
  -DCMAKE_PREFIX_PATH=<qt>/6.8.3/mingw_64 \
  -DMPV_RUNTIME_DIR="%LOCALAPPDATA%/MPVBackend/runtime"
cmake --build build-cmake --target mikai --parallel
```

Then run `build-cmake\mikai.exe`.

## WebP is required

Posters are served as `.webp` and the app asks Qt to decode them. A base Qt
install has **no WebP decoder** — its `imageformats` folder holds only `qgif`,
`qico`, `qjpeg` and `qsvg`. Without `qwebp.dll` the app still runs, the
network still works, and every image is simply blank: `QPixmap` returns a null
image for a format it has no handler for and reports nothing at all.

So `qwebp.dll` is a hard runtime dependency. It has to be in the Qt install, or
`windeployqt` has nothing to copy:

```
<qt>/6.8.3/mingw_64/plugins/imageformats/qwebp.dll
```

The plugin must match the Qt build's toolchain. A MinGW app needs the
`win64_mingw` plugin; an MSVC one will not load it, and the reverse is also
true. Check with `objdump -p qwebp.dll | grep "DLL Name"` — a MinGW build
imports `libstdc++-6.dll`, an MSVC build imports `VCRUNTIME140.dll`.

A Qt installed with `aqtinstall` has no Maintenance Tool, so the module comes
from the Qt online repository:

```sh
curl -LO https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_680/qt6_680/qt.qt6.680.addons.qtimageformats.win64_mingw/6.8.0-0-202410030750qtimageformats-Windows-Windows_10_22H2-Mingw-Windows-Windows_10_22H2-X86_64.7z
tar -xf 6.8.0-*.7z            # bsdtar reads 7z
cp 6.8.0/mingw_64/plugins/imageformats/qwebp.dll <qt>/6.8.3/mingw_64/plugins/imageformats/
```

`TestModels::webpIsDecodable` fails the build if the plugin is missing, so a
missing one is a red test rather than a screen of blank placeholders.

## Deploy

`mikai.exe` links Qt dynamically, so the runtime has to sit beside it or be on
`PATH`; a fresh exe with no deployed DLLs exits with `0xC0000135`.

```sh
windeployqt build-cmake/mikai.exe
```

`windeployqt` does not copy `libmpv-2.dll` — it lives in `MPV_RUNTIME_DIR` and
has to be copied in by hand alongside the Qt DLLs. The resulting folder is
self-contained and is what you hand to someone else.

`mikai.exe` is `WIN32_EXECUTABLE`, so it has no console window. Diagnostics go
to `mikai.log` beside the exe, not to stdout: every Qt message is appended
there, it rotates to `mikai.log.1` past 2 MB, and an unhandled exception also
writes `mikai-crash-<timestamp>.txt` carrying the exception code, the faulting
module, and the tail of the session log. A session that starts with an old
crash report still on disk says so in the log.

## Tests

```sh
cmake --build build-cmake --target mikai_tests --parallel
ctest --test-dir build-cmake -R mikai_tests
```

Hermetic except the live oracle, which runs only with both set:

```sh
set MIKAI_LIVE=1 & set MIKAI_API_KEY=mk_... & ctest --test-dir build-cmake -R mikai_tests
```

## What it does

- **Home** — carousels for titles you recently opened, titles airing now, and Mikai's current popular picks, plus the latest episode updates and the weekly airing schedule. Every card shows the poster, the airing status and the aired/planned episode count, and a search result row reads the same way (`Бліч · 2004 · TV · Завершено · 366 серій`) straight from the list response, with no extra request; right-click a card to open it, copy its mikai.me link or raw id, or open the site page in a browser.
- **Title** — description, cover, scores, genres and related titles, then one tab per release (voice or subtitles, grouped by dubbing team). Each episode opens the player at the qualities the provider actually has; the right-click menu adds copy-URL-only, re-extract, and hand-off to an external player. The cover opens full size with pan and zoom.
- **Player** — a player built around the same Qt event loop as the app: the `PlayerDialog` composition root drives the `PlayerShell` chrome and the mpv-backed `VideoSurface` in-process. It opens full screen, and the `><` / `<>` buttons beside the PiP icon — or a double click on the picture, or `F` / `F11` — toggle a real decorated native window, so leaving full screen restores the previous windowed geometry and the OS titlebar handles dragging, resizing, snap layouts and moving the window to another monitor. The toggle is scoped to the player; the app's main window is unaffected. The picture fills the window edge to edge, while a two-row dock keeps the large play control and permanent volume slider above a full-width timeline. The chrome fades after a couple of seconds of stillness and returns on the next mouse move. A top-left chip reports network throughput, decoded FPS and, while enhancement is active, the source-to-output resolution. Click the picture to pause, step through episodes from the drawer (S), switch the dub/sub track (T) or the stream quality (Q) without leaving the player. A picture-in-picture float drops the video into a small always-on-top window with its own overlaid controls — click to pause, hold and drag to move it.
- **Anime4K** — the dock's sparkle button opens **Enhancements → Anime4K**, with ten presets from *Off* through the Fast A/B/C chains to the HQ and double-restore ones. The choice is applied to the running mpv core, so it lands on the next rendered frame without reopening the episode, and it is remembered across runs. The presets live in `shaders/` and are deployed next to the executable; the enhancement control hides itself if that folder did not ship.
- **Settings** — the API token with its live quota, plus a checklist of the home blocks to load. A block you switch off is never requested, which is the cheapest way to cut API spend on a slow or metered connection.

Qualities are extracted once per episode, only when that episode opens, and the resulting stream URLs are cached for half an hour, so reopening a title repaints instead of re-hitting every provider. The links are signed and do expire; if a stream fails to play the player re-extracts it once on its own before reporting the error.

The shader comparison is a port of AnimeInterp's comparison window. The five modes
(Split H, Split V, Side-by-side, Flicker, Difference) and their geometry live in
`src/player/CompareLayout.cpp` as one pure `buildLayout` call, so a
mode's whole picture is a value rather than a chain of scene mutations. `CompareCanvas`
turns that value into scene items and owns the gestures: the wheel zooms around the cursor,
and Alt+drag pans; in split modes, ordinary drag moves the divider. The image is never
upscaled past 1:1, because a 480p frame blown up shows interpolation artefacts rather than
detail. A split is drawn by clipping the two cached images, so the divider costs no
recomposite and no shader re-render. Clickable model-name badges stay pinned to the left and
right image corners, each opening that side's own candidate list, and they are re-anchored
after every scroll so a pan cannot carry a label off the picture it names. Those badges are
the only chooser: the window has no candidate sidebar beside the picture. The window compares
and never rewrites the chain the player is running.

Every opening captures a fresh frame at the player's current position. A frame is accepted
only once the surface has stopped reporting a move and the framebuffer has held still across
several rounds, because the reported position arrives before the picture does. If no settled
frame can be read, the window says so and shows no picture rather than falling back on the
previous frame. Both sides always render from that one frozen frame, so the difference between
them is the chain and nothing else.

## API limits

The **RPM** indicator beside Settings shows the server-reported requests used in the current minute and the limit that applies. Calls wait for the server's reset window instead of failing when the allowance is spent. An optional API key raises the standard limit from 60 to 300 requests per minute and is stored locally in `.env`, which is never committed.

## Layout

`src/backend/` is the domain — `MikaiClient` (rate-limited Qt Network calls), models, HLS extraction (`ashdi`/`moon`/`tortuga`), libmpv discovery — and `src/app/` is the Qt Widgets frontend (`MainWindow`, `SearchPage`, `TitlePage`, dialogs, theme tokens), with the player (`src/player/`: `PlayerDialog` → `PlayerShell` → mpv-backed `VideoSurface`) driven in-process through a `PlayerHost` bridge, and `shaders/` holding the Anime4K `.glsl` presets that CMake copies next to the executable. `docs.md` is the authoritative API contract and `AGENTS.md` the map for contributors: architecture, conventions, build commands and the testing setup. Run the checks with `ctest --test-dir build-cmake -R mikai_tests`.
