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

The optional `external_media_probe` target exercises the actual home file/URL dialogs,
video rendering, Compare, Anime4K menu, standalone control visibility, error recovery,
and close/reopen/shutdown. Run it with a 320×180 clip longer than 10 seconds and
its HTTP URL. The HTTP fixture must also serve `/retry.mp4`: return 404 for its
first request, then return the same clip.

`mikai_tests` is hermetic except the live oracle, which runs only with both set:

```sh
set MIKAI_LIVE=1 & set MIKAI_API_KEY=mk_... & ctest --test-dir build-cmake -R mikai_tests
```

## What it does

- **Home** — carousels for titles you recently opened, titles airing now, and Mikai's current popular picks, plus the latest episode updates and the weekly airing schedule. Every card shows the poster, the airing status and the aired/planned episode count, and a search result row reads the same way (`Бліч · 2004 · TV · Завершено · 366 серій`) straight from the list response, with no extra request; right-click a card to open it, copy its mikai.me link or raw id, or open the site page in a browser.
- **Title** — description, cover, scores, genres and related titles, then one tab per release (voice or subtitles, grouped by dubbing team). Each episode opens the player at the qualities the provider actually has; the right-click menu adds copy-URL-only, re-extract, and hand-off to an external player. The cover opens full size with pan and zoom.
- **Player** — a player built around the same Qt event loop as the app: the `PlayerDialog` composition root drives the `PlayerShell` chrome and the mpv-backed `VideoSurface` in-process. It opens full screen, and the `><` / `<>` buttons beside the PiP icon — or a double click on the picture, or `F` / `F11` — toggle a real decorated native window, so leaving full screen restores the previous windowed geometry and the OS titlebar handles dragging, resizing, snap layouts and moving the window to another monitor. The toggle is scoped to the player; the app's main window is unaffected. The picture fills the window edge to edge, while a two-row dock keeps the large play control and permanent volume slider above a full-width timeline. The chrome fades after a couple of seconds of stillness and returns on the next mouse move. A top-left chip reports network throughput, decoded FPS and, while enhancement is active, the source-to-output resolution. Click the picture to pause, step through episodes from the drawer (S), switch the dub/sub track (T) or the stream quality (Q) without leaving the player. A picture-in-picture float drops the video into a small always-on-top window with its own overlaid controls — click to pause, hold and drag to move it.
- **Anime4K** — the dock's sparkle button opens **Покращення**, whose **Anime4K** row opens a tree on hover: resting on a row opens the level under it as a panel beside the one it sits in, so the four categories - *Швидкість* (Fast A/B/C), *Якість* (HQ A/B/C), *Максимум* (the double-restore chains) and *Експериментальні* (3D Graphics, Old Film Restore, Deblur, plus single-file Noise Reduction, Thin Lines, Texture Enhancement and Darken presets). **Вимкнено** turns enhancement off. The choice is applied to the running mpv core, so it lands on the next rendered frame without reopening the episode, and it is remembered across runs. The same tree lists **RIFE**, **RealESRGAN** and **Інші моделі** as panels of their own, each holding a disabled *В планах* leaf: they have no engine yet, and nothing in them can be chosen. The presets live in `shaders/` and are deployed next to the executable; the enhancement control hides itself if that folder did not ship.
- **External video** — the home-page **Відкрити відео** menu opens a local file or a media/stream URL in the built-in mpv player. Standalone playback keeps seeking, volume, speed, PiP, **Enhancements → Anime4K**, and the picture's right-click **Порівняти якість** action, but has no anime quality selector, episode list, or release picker. URLs go directly to mpv; anime-provider page extraction is not used. A failed source displays an error, and **Повторити** retries that same file or URL.
- **Settings** — the API token with its live quota, plus a checklist of the home blocks to load. A block you switch off is never requested, which is the cheapest way to cut API spend on a slow or metered connection. The **Пресети** tab assigns a one-keypress shortcut to every Anime4K preset, including **Вимкнено**; leave a binding empty to clear it. Duplicate assignments are rejected immediately, and built-in player keys remain reserved.

### Anime4K handoff

**What it is.** `src/player/Anime4K.cpp` holds one declarative catalogue: for every preset it carries the stable id, the Ukrainian menu label and hint, the category it hangs under, and the *ordered* shader chain. `availableModes()` returns that table and `menuRows(parent)` returns the level above it as the rows a panel renders, so nothing else keeps a second copy of a chain - adding a preset is one table entry. `shaders/` contains 49 individual GLSL files; a preset is an ordered chain, not a separate shader. The Compare window (`src/player/CompareWindow.cpp`) offers the same 24 preset ids and labels as the player menu, under the same categories and experiment families; individual `.glsl` files are not extra choices, so every option compared is one the player can select.

**Menu.** The dock's sparkle button opens **Покращення**: an **Anime4K ›** row plus **RIFE ›**, **RealESRGAN ›** and **Інші моделі ›**. The menu is a tree rather than a sequence of screens: resting the pointer on a row opens the level under it as a panel *beside* the panel that row sits in, and every level stays on screen, so there is no **‹ Назад** row anywhere. From the root that is the Anime4K level (**Вимкнено**, then the four categories), one panel per category, then one per experiment family - four panels at the deepest leaf, each top-aligned to the row that opened it and placed on the side of the tree with room. Clicking a category row opens the same panel, so the tree is reachable without a pointer motion, and clicking a preset row applies it and closes the whole tree. A category row only drills down - it never selects a shader - and each top-level category carries its purpose as a tooltip (*Швидкість* low GPU use in real time, *Якість* balanced quality, *Максимум* multi-stage at a higher GPU cost, *Експериментальні* special cases that are not defaults). The family names inside *Експериментальні* are Anime4K's own (*Noise Reduction*, *Thin Lines*, *Texture Enhancement*, *Darken*) with Ukrainian tooltips. The row holding the current preset is highlighted, including the closed category it sits in.

**Built-in presets** (menu labels and hints are Ukrainian in the catalogue; stable IDs are English and are what settings persist). Each chain name expands to `Anime4K_<name>.glsl`:

| Menu label / ID | Intended use | Ordered shader chain (files in `shaders/`) |
| --- | --- | --- |
| Вимкнено / `off` | Original picture | — |
| A (Швидкий) / `fast_a` | Restore lines + upscale | `Clamp_Highlights` → `Restore_CNN_M` → `Upscale_CNN_x2_M` → `AutoDownscalePre_x2` → `AutoDownscalePre_x4` → `Upscale_CNN_x2_S` |
| B (Швидкий) / `fast_b` | Soft restore + upscale | `Clamp_Highlights` → `Restore_CNN_Soft_M` → `Upscale_CNN_x2_M` → `AutoDownscalePre_x2` → `AutoDownscalePre_x4` → `Upscale_CNN_x2_S` |
| C (Швидкий) / `fast_c` | Denoise/upscale | `Clamp_Highlights` → `Upscale_Denoise_CNN_x2_M` → `AutoDownscalePre_x2` → `AutoDownscalePre_x4` → `Upscale_CNN_x2_S` |
| A (Якісний) / `hq_a` | Detailed CNN restore | `Clamp_Highlights` → `Restore_CNN_VL` → `Upscale_CNN_x2_VL` → `AutoDownscalePre_x2` → `AutoDownscalePre_x4` → `Upscale_CNN_x2_M` |
| B (Якісний) / `hq_b` | Soft restore + upscale | `Clamp_Highlights` → `Restore_CNN_Soft_VL` → `Upscale_CNN_x2_VL` → `AutoDownscalePre_x2` → `AutoDownscalePre_x4` → `Upscale_CNN_x2_M` |
| C (Якісний) / `hq_c` | Denoise with high PSNR | `Clamp_Highlights` → `Upscale_Denoise_CNN_x2_VL` → `AutoDownscalePre_x2` → `AutoDownscalePre_x4` → `Upscale_CNN_x2_M` |
| A+A (Максимум) / `hq_aa` | Two-stage restore | `Clamp_Highlights` → `Restore_CNN_VL` → `Upscale_CNN_x2_VL` → `Restore_CNN_M` → `AutoDownscalePre_x2` → `AutoDownscalePre_x4` → `Upscale_CNN_x2_M` |
| B+B (Максимум) / `hq_bb` | Two-stage soft restore | `Clamp_Highlights` → `Restore_CNN_Soft_VL` → `Upscale_CNN_x2_VL` → `AutoDownscalePre_x2` → `AutoDownscalePre_x4` → `Restore_CNN_Soft_M` → `Upscale_CNN_x2_M` |
| C+A (Максимум) / `hq_ca` | Denoise + detail restore | `Clamp_Highlights` → `Upscale_Denoise_CNN_x2_VL` → `AutoDownscalePre_x2` → `AutoDownscalePre_x4` → `Restore_CNN_M` → `Upscale_CNN_x2_M` |

**Experimental presets** live under *Експериментальні* and each one is a fixed chain from the same shader set - a complete preset, not a toggle to be composed with another. They are not recommended defaults; the hints say so. Their IDs are permanent and readable, so a stored selection keeps naming the same chain:

| Menu path / ID | Character | Ordered shader chain (files in `shaders/`) |
| --- | --- | --- |
| 3D Graphics / `exp_3d_graphics` | 3D AA upscale | `Clamp_Highlights` → `3DGraphics_AA_Upscale_x2_US` |
| Old Film Restore / `exp_old_film` | Old-film restore | `Clamp_Highlights` → `Deblur_Original` → `Restore_CNN_M` → `Upscale_CNN_x2_M` |
| Deblur / `exp_deblur_strong` | Strong Deblur | `Clamp_Highlights` → `Deblur_DoG` → `Restore_CNN_M` → `Upscale_CNN_x2_M` |
| Noise Reduction → Bilateral Mean / `exp_noise_bilateral_mean` | One file | `Denoise_Bilateral_Mean` |
| Noise Reduction → Bilateral Median / `exp_noise_bilateral_median` | One file | `Denoise_Bilateral_Median` |
| Noise Reduction → Bilateral Mode / `exp_noise_bilateral_mode` | One file | `Denoise_Bilateral_Mode` |
| Thin Lines → VeryFast / `exp_thin_veryfast` | One file | `Thin_VeryFast` |
| Thin Lines → Fast / `exp_thin_fast` | One file | `Thin_Fast` |
| Thin Lines → HQ / `exp_thin_hq` | One file | `Thin_HQ` |
| Texture Enhancement → DTD x2 / `exp_texture_dtd_x2` | One file | `Upscale_DTD_x2` |
| Texture Enhancement → DoG x2 / `exp_texture_dog_x2` | One file | `Upscale_DoG_x2` |
| Darken → VeryFast / `exp_darken_veryfast` | One file | `Darken_VeryFast` |
| Darken → Fast / `exp_darken_fast` | One file | `Darken_Fast` |
| Darken → HQ / `exp_darken_hq` | One file | `Darken_HQ` |

The three planned models are submenus, not presets: **RIFE**, **RealESRGAN** and **Інші моделі** open a panel of their own beside ПОКРАЩЕННЯ (category ids `rife`, `realesrgan`, `other-models`), each holding a single disabled **В планах** leaf (`rife/planned`, `realesrgan/planned`, `other-models/planned`). They have no shader chain and no mode, and nothing in those panels can be selected.

**All 49 shader files**, grouped by role; each entry below expands to `Anime4K_<name>.glsl`:

- Upscale — CNN: `Upscale_CNN_x2_{S,M,L,UL,VL}`; denoise CNN: `Upscale_Denoise_CNN_x2_{S,M,L,UL,VL}`; GAN: `Upscale_GAN_x2_{S,M}`, `Upscale_GAN_x3_{L,VL}`, `Upscale_GAN_x4_{UL,UUL}`; other: `Upscale_Original_x2`, `Upscale_DoG_x2`, `Upscale_Deblur_{Original,DoG}_x2`, `Upscale_DTD_x2`.
- Restore — CNN: `Restore_CNN_{S,M,L,UL,VL}`; soft CNN: `Restore_CNN_Soft_{S,M,L,UL,VL}`; GAN: `Restore_GAN_{UL,UUL}`.
- Denoise: `Denoise_Bilateral_{Mean,Median,Mode}`.
- Deblur: `Deblur_{Original,DoG}`.
- Darken: `Darken_{VeryFast,Fast,HQ}`.
- Thin lines: `Thin_{VeryFast,Fast,HQ}`.
- Other: `Clamp_Highlights`, `AutoDownscalePre_{x2,x4}`, `3DGraphics_Upscale_x2_US`, `3DGraphics_AA_Upscale_x2_US`.

**Runtime and persistence.** Every preset and its Ukrainian hint comes from the catalogue, and a menu row selects by that preset's id - never by an enum ordinal or a menu index. `VideoSurface` resolves the applied *id* (not the mode) to ordered absolute paths and writes them to mpv's `glsl-shaders`, both when the core is created and on every later change, so a chain picked before the engine exists is still honoured on the first frame. `player/anime4k` stores that id: the ten built-in ids are frozen (`off`, `fast_a`…`hq_ca`) so an existing selection cannot be repointed, and the experiments have permanent readable ids (`exp_thin_hq`, `exp_deblur_strong`, …) rather than positions in a menu. PiP copies both the mode and the applied chain id into the float's own core; the info chip shows a nested experiment qualified by its category (`Тонкі лінії · HQ`). The Compare window borrows the live surface to render each side and puts the player's own chain back when it is done, so a comparison never leaves the player running a preview chain. At startup the app searches `<exe directory>/shaders`, then `./shaders`, then `./build-cmake/shaders`; when none exists, Anime4K controls are hidden. The shader `//!WHEN` conditions can decline to upscale when the output is not larger than the source, so enabling a chain does not guarantee every stage changes every frame.

**Build and language.** Implementation and build configuration are C++20/CMake; shader programs are GLSL. The 49 source assets live in root `shaders/`. `CMakeLists.txt`'s `mikai` POST_BUILD rule copies that directory beside `mikai.exe`; CMake does not precompile GLSL—mpv loads the copied files at runtime. Build from the configured tree with `cmake --build build-cmake --target mikai --parallel`. The Anime4K categories, preset labels and hints are localized UI text; the future-model submenus intentionally remain placeholder-only with a disabled **В планах** leaf each. Keep menu labels/tooltips in Ukrainian to match the app UI; keep preset IDs and shader filenames unchanged because settings and chain resolution depend on them. `cmake --build build-cmake --target anime4k_menu_probe --parallel && build-cmake/anime4k_menu_probe.exe` runs the full native Anime4K smoke (every menu leaf, pixel rendering, settings reopen, Compare groups, PiP chain and minimum-window fit); `build-cmake/resolver_probe.exe` verifies the 71-candidate catalog resolves cleanly.

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
right image corners, each opening that side's own chooser, and they are re-anchored
after every scroll so a pan cannot carry a label off the picture it names. That chooser is
the same Anime4K preset tree shown in the player's Enhancements menu: it offers the same
24 stable-ID presets with the same categories and experiment families, and no standalone
shader files. Each side can therefore use only a model the player itself can select. Those badges are
the only chooser: the window has no candidate sidebar beside the picture. The window compares
and never rewrites the chain the player is running: each side is rendered on the borrowed
surface, and the player's own chain is restored before the render returns.

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
