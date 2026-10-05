# Mikai Wrapper

A C++/Qt6 desktop client for the [Mikai public API](https://api.mikai.me/public/docs).

## Build

Needs MinGW-w64 g++, Qt 6.8.x with LinguistTools, and an mpv header directory
holding `mpv/client.h` and `mpv/render_gl.h`. Pass it as
`-DMPV_INCLUDE_DIR=<mpv>/include` (CMake caches the resolved path as
`MIKAI_MPV_HEADERS`); it has no default and CMake hard-fails without it. The headers are build-only: the app never links libmpv, so no
import library and no `libmpv-2.dll` are needed to compile, and an ordinary Qt
build needs no native SDK. The toolchain paths have to be passed explicitly — a bare
`-G "MinGW Makefiles"` fails with `CMAKE_MAKE_PROGRAM is not set` when the
toolchain is not on `PATH`.

```sh
cmake --preset release
cmake --build --preset app --parallel
```

Then run `build\release\mikai.exe`. Mikai selects its mpv core at runtime and
loads it explicitly, so the DLL is not a load-time import: discovery looks
beside the executable and in `%LOCALAPPDATA%/MPVBackend/runtime`, and a probe
or test that must pin one DLL names it with `MIKAI_MPV_MODULE`. The selected
module is pinned for the life of the process — main playback and the PiP float
share it — and changing the runtime takes effect only after a restart.

## Interface languages

Mikai's interface is available in English and Ukrainian. English is the source language in the C++ `tr()` and `QCoreApplication::translate()` calls; the Ukrainian catalog is `translations/mikai_uk.ts`. In **Settings → General → Interface language**, choose **Use system language**, **English**, or **Ukrainian**. System mode selects Ukrainian only when the operating-system locale is Ukrainian and otherwise uses English. A language change takes effect after restarting Mikai.

To refresh the catalog after changing source strings, build the `update_translations` target, then translate the new entries in `translations/mikai_uk.ts`:

```sh
cmake --build --preset release --target update_translations
```

Keep stable IDs, API values, file patterns, URLs, and dynamic title/team/provider names untranslated. Preserve Qt placeholders such as `%1` and `%n`, HTML markup, and all numerus forms. The app build compiles and embeds the application catalog; Qt omits unfinished entries, which then fall back to the English source text. Before release, check that the catalog contains no unfinished entries.

A new language needs a TS file added to `qt_add_translations()` in `CMakeLists.txt`, a runtime translator and language option in `src/app/main.cpp` and `SettingsDialog`, and its system-language mapping in `src/app/I18n.cpp`. English remains the source-language fallback and does not need a separate catalog.

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

The suite fails if the plugin is missing, so a missing one is a red test rather
than a screen of blank placeholders.

## Deploy

`mikai.exe` links Qt dynamically, so the runtime has to sit beside it or be on
`PATH`; a fresh exe with no deployed DLLs exits with `0xC0000135`.

```sh
windeployqt --translations uk build/release/mikai.exe
```

`MIKAI_MINGW_RUNTIME_DIR` has to name the compiler runtime the configured Qt
links (Qt's `bin` directory); configure refuses anything else. Qt6Core.dll
imports `libgcc_s_seh-1`/`libstdc++-6`/`libwinpthread-1` by name, the
executable's directory wins that lookup, and a process keeps one copy per module
name, so a native closure from another MinGW generation cannot bring its own in
beside Qt's - it has to be built against the Qt runtime. Deploy with
`windeployqt --no-compiler-runtime …` so Qt does not overwrite the selected CRT.

`windeployqt` does not copy an mpv runtime. Mikai resolves one at runtime
(beside the exe or in `%LOCALAPPDATA%/MPVBackend/runtime`), so copy the
runtime tree into either place when the deployed folder has to play on its own. `--translations uk` deploys
Qt's Ukrainian catalogs, and the `mikai` build copies `qtbase_uk.qm` from the
configured Qt installation into the executable's `translations` directory.
The resulting folder is self-contained and is what you hand to someone else.

`mikai.exe` is `WIN32_EXECUTABLE`, so it has no console window. Diagnostics go
to `mikai.log` beside the exe, not to stdout: every Qt message is appended
there, it rotates to `mikai.log.1` past 2 MB, and an unhandled exception also
writes `mikai-crash-<timestamp>.txt` carrying the exception code, the faulting
module, and the tail of the session log. A session that starts with an old
crash report still on disk says so in the log.

## Tests

```sh
cmake --build --preset tests --parallel
ctest --test-dir build/release -R mikai_tests
```

### Behavioural probes (JSON manifests)

`probe_runner` is a single executable that runs declarative probes, so adding
coverage means adding a JSON file rather than a new binary:

```sh
cmake --build --preset release --target probe_runner --parallel
build/release/probe_runner.exe probes/                  # run everything
build/release/probe_runner.exe probes/ --list           # what exists, without running
build/release/probe_runner.exe probes/ --filter seek    # one scenario by name
```

Manifests live in `probes/<category>/` and address the application by identity
only: a widget by its `objectName`, the core by an mpv property. There are no
screen coordinates, no image or pixel comparison, and no sleep used as a
synchroniser — a scenario waits on a signal or on an observable condition.
Assertions read behaviour: a Qt signal, a Qt property, or what mpv itself holds
(`pause`, `volume`, `duration`, `time-pos`, `filename`).

```json
{
  "name": "settings-gear-opens-flyout",
  "category": "ui-interaction",
  "surface": "player_dialog",
  "media": "tests/fixtures/media/motion_320x180_24fps_8bit_hevc.mp4",
  "steps": [
    { "kind": "wait_for", "name": "hasVideo" },
    { "kind": "click", "target": "playerSettingsButton" }
  ],
  "assertions": [
    { "kind": "qt_property", "target": "playerSettingsFlyout",
      "property": "visible", "expected": true }
  ]
}
```

Four categories are in use: `ui-interaction`, `player-behavior`, `state-sync`
(which checks that the Qt state and the core agree) and `regression`, which pins
a defect that has already been fixed. Adding a probe to an interactive widget
usually means giving that widget an `objectName` first; a control a manifest has
to write also needs its `Q_PROPERTY` exposed, so the manifest drives the real
setter and sees the same signal a user would.

The optional `external_media_probe` target exercises the actual home file/URL dialogs,
video rendering, Compare, Anime4K menu, standalone control visibility, error recovery,
and close/reopen/shutdown. Run it with a 320×180 clip longer than 10 seconds and
its HTTP URL. The fixture lives in the repository: build
`cmake --build --preset release --target mikai_http_fixture` and start
`build/release/tests/fixtures/mikai_http_fixture.exe --port 0 --facts facts.json
--require-accept-language`; it serves the real clips under `tests/fixtures/media/`
and the three HLS renditions under `tests/fixtures/hls/`, with `/media.mp4` as the
plain clip and `/retry.mp4` as the same bytes after one deliberate 404, which is
what the retry phase needs.

`mikai_tests` also carries the runtime-loader and native-contract suites
(`TestRuntimeSelection`, `TestMpvAsync`, `TestNative*`); they run hermetically,
exercising a real core only when `MIKAI_MPV_MODULE` names one and skipping
otherwise. Adding `-DMIKAI_WITH_MODEL_INSPECTION=ON -DORT_INCLUDE_DIR=<onnxruntime>/include`
builds a CPU-only ONNX model inspector and its `TestNativeCatalog` suite; it is
not a GPU/DirectML/TensorRT path, and the default build has no ONNX Runtime
dependency. These probes and the suite are hand-run checks, not a CI pipeline.

`native_prep_probe` is the one hand-run probe that drives the native
preparation helper against a real CUDA device. The helper builds a plan
in-process through the TensorRT builder API and launches no other executable:

```sh
native_prep_probe <helper.exe> <graph.onnx> <GPU-uuid> <named-input-shapes>
```

On the current helper — which builds a plan in-process through the TensorRT
builder API and launches no other executable — it reports `RESULT: PASS` over
seven behaviours: an existing output refuses and keeps its bytes, a corrupt
graph exits `5` and leaves nothing engine-shaped, an absent GPU UUID refuses
rather than falling back to another adapter, `build-started` is emitted before
the build proceeds, the TF32-off/strongly-typed precision gate holds, cancelling
mid-build publishes nothing, and failure or cancellation leaves prior artifacts
and source bytes unchanged.

That is builder-invocation proof only. The spatial engine it produces is
currently **rejected** by `mikai_native_verify` — its bindings and GPU enqueue
succeed, but `maxAbsoluteError` exceeds the FP16 tolerance. The RIFE matrix
splits: all four graphs run a real CPU reference and all four serialize on GPU;
**lite** and **lite ensemble** verify numerically, **full** and **ensemble** are
rejected on `maxAbsoluteError`. A verify pass covers raw engine bytes,
bindings and numerics only (`manifestVerified: false`, `prepared: false`), so
nothing is `Ready` or `Active`. The pinned mpv fork core builds and its isolated
core loads (exit `0`), and a Lua-enabled candidate now plays a real 320×180 HEVC
24 fps video through the owned runtime with **Native off** — 921600/921600
non-black samples, 290264/921600 motion, 0 decoder and 0 VO drops, 0 live `vf`
chain entries — with an inspected colored 1280×720 motion pattern, and
`widget_pixel_probe` passes 21/21 with 0 skips (colored 640×360 PNG, GL 4.6,
0 `ErrorType`, `glGetError` 0, 30.44% motion); that is the runtime surface only,
not the native filter, CUDA interop or active inference.
The bundle candidate itself still aborts verification on a `GGML` initializer
(fix in flight, no PASS yet). See `native/README.md` and `native/build-msvc.md`.

The window-driving probes need the interactive desktop session attached: with
the session detached there is no GL context and no video output, so every
pixel-counting proof reports a black frame (`hasVideo=0`). With the session
attached, `interaction_probe` passes 149/149, `compare_probe` 30/30
(frozen-frame proof, 0.000/255 noise floor), and `hls_recovery_probe` and
`external_media_probe` report `RESULT: PASS` (real expired-signature recovery;
real picker, HTTP, same-URL retry and file URL). `native_settings_probe`,
`rife_settings_probe` (both languages),
`native_menu_probe`, `native_status_probe`, `runtime_selection_probe`,
`spatial_assets_probe` and the ORT-backed `native_assets_probe`
(`MIKAI_ORT_RUNTIME_DIR` naming the runtime directory) pass either way. The
`compare_probe` accepts an optional media file argument. A failed direct-media
stream shows its error with a Retry chip that survives chrome auto-hide;
replacing the player deletes the old dialog synchronously, so no parked core
decodes behind the new stream. `widget_pixel_probe` takes a PNG path argument
(otherwise one fixed lavfi source), so it does not cover alternate-media or
dialog flows; its only observed failure is a refused PNG save. `shell_probe` and
`dialog_probe` are what observed the missing-input FAIL (exit code 1), the
static/black inconclusive SKIP (exit code 77), the moving-portrait and the
1 s-EOF cases. `mpv_surface_smoke` also requires inter-frame motion, so static
near-black is SKIP (exit code 77). `headless_pixel_probe` proves lit pixels and
a real render without a motion requirement: static near-black can pass there,
while all-black is inconclusive.

`mikai_tests` is hermetic except the live oracle, which runs only with both set:

```sh
set MIKAI_LIVE=1 & set MIKAI_API_KEY=mk_... & ctest --test-dir build/release -R mikai_tests
```

## What it does

Labels shown in these examples use Ukrainian; the interface can also run in English.

- **Home** — carousels for titles you recently opened, titles airing now, and Mikai's current popular picks, plus the latest episode updates and the weekly airing schedule. Every card shows the poster, the airing status and the aired/planned episode count, and a search result row reads the same way (`Бліч · 2004 · TV · Завершено · 366 серій`) straight from the list response, with no extra request; right-click a card to open it, copy its mikai.me link or raw id, or open the site page in a browser.
- **Title** — description, cover, scores, genres and related titles, then one tab per release (voice or subtitles, grouped by dubbing team). Each episode opens the player at the qualities the provider actually has; the right-click menu adds copy-URL-only, re-extract, and hand-off to an external player. The cover opens full size with pan and zoom.
- **Player** — a player built around the same Qt event loop as the app: the `PlayerDialog` composition root drives the `PlayerShell` chrome and the mpv-backed `VideoSurface` in-process. It opens full screen, and the `><` / `<>` buttons beside the PiP icon — or a double click on the picture, or `F` / `F11` — toggle a real decorated native window, so leaving full screen restores the previous windowed geometry and the OS titlebar handles dragging, resizing, snap layouts and moving the window to another monitor. The toggle is scoped to the player; the app's main window is unaffected. The picture fills the window edge to edge, while a two-row dock keeps the large play control and permanent volume slider above a full-width timeline. The chrome fades after a couple of seconds of stillness and returns on the next mouse move. A top-left chip reports network throughput, decoded FPS and, while enhancement is active, the source-to-output resolution. Click the picture to pause, step through episodes from the drawer (S), switch the dub/sub track (T) or the stream quality (Q) without leaving the player. A picture-in-picture float drops the video into a small always-on-top window with its own overlaid controls — click to pause, hold and drag to move it.
- **Anime4K** — the dock's sparkle button opens **Покращення**, whose **Anime4K** row opens a tree on hover: resting on a row opens the level under it as a panel beside the one it sits in, so the four categories - *Швидкість* (Fast A/B/C), *Якість* (HQ A/B/C), *Максимум* (the double-restore chains) and *Експериментальні* (3D Graphics, Old Film Restore, Deblur, plus single-file Noise Reduction, Thin Lines, Texture Enhancement and Darken presets). **Вимкнено** turns enhancement off. The choice is applied to the running mpv core, so it lands on the next rendered frame without reopening the episode, and it is remembered across runs. The same tree lists **RIFE**, **RealESRGAN** and **Інші моделі** as panels of their own, each holding a disabled *В планах* leaf: they have no engine yet, no model in them executes, and nothing can be chosen — RIFE because its producer is unavailable (ESC-1), RealESRGAN and Інші моделі simply planned. The presets live in `shaders/` and are deployed next to the executable; the enhancement control hides itself if that folder did not ship.
- **External video** — the home-page **Відкрити відео** menu opens a local file or a media/stream URL in the built-in mpv player. Standalone playback keeps seeking, volume, speed, PiP, **Enhancements → Anime4K**, and the picture's right-click **Порівняти якість** action, but has no anime quality selector, episode list, or release picker. URLs go directly to mpv; anime-provider page extraction is not used. A failed source displays an error, and **Повторити** retries that same file or URL.
- **Settings** — the API token with its live quota, a checklist of home blocks, and an **Interface language** selector for system, English, or Ukrainian. A disabled block is never requested. The **Presets** tab assigns a one-keypress shortcut to every Anime4K preset; duplicate assignments are rejected and built-in player keys remain reserved.

### Anime4K handoff

**What it is.** `src/player/Anime4K.cpp` holds one declarative catalogue: for every preset it carries the stable id, the localized menu label and hint, the category it hangs under, and the *ordered* shader chain. `availableModes()` returns that table and `menuRows(parent)` returns the level above it as the rows a panel renders, so nothing else keeps a second copy of a chain - adding a preset is one table entry. `shaders/` contains 49 individual GLSL files; a preset is an ordered chain, not a separate shader. The Compare window (`src/player/CompareWindow.cpp`) offers the same 24 preset ids and labels as the player menu, under the same categories and experiment families; individual `.glsl` files are not extra choices, so every option compared is one the player can select.

**Menu.** The dock's sparkle button opens **Покращення**: an **Anime4K ›** row plus **RIFE ›**, **RealESRGAN ›** and **Інші моделі ›**. The menu is a tree rather than a sequence of screens: resting the pointer on a row opens the level under it as a panel *beside* the panel that row sits in, and every level stays on screen, so there is no **‹ Назад** row anywhere. From the root that is the Anime4K level (**Вимкнено**, then the four categories), one panel per category, then one per experiment family - four panels at the deepest leaf, each top-aligned to the row that opened it and placed on the side of the tree with room. Clicking a category row opens the same panel, so the tree is reachable without a pointer motion, and clicking a preset row applies it and closes the whole tree. A category row only drills down - it never selects a shader - and each top-level category carries its purpose as a tooltip (*Швидкість* low GPU use in real time, *Якість* balanced quality, *Максимум* multi-stage at a higher GPU cost, *Експериментальні* special cases that are not defaults). The family names inside *Експериментальні* are Anime4K's own (*Noise Reduction*, *Thin Lines*, *Texture Enhancement*, *Darken*) with Ukrainian tooltips. The row holding the current preset is highlighted, including the closed category it sits in.

**Built-in presets** (menu labels and hints are localized; stable IDs are never translated and are what settings persist). Each chain name expands to `Anime4K_<name>.glsl`:

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

**Runtime and persistence.** Every preset and its localized hint comes from the catalogue, and a menu row selects by that preset's id - never by an enum ordinal or a menu index. `VideoSurface` resolves the applied *id* (not the mode) to ordered absolute paths and writes them to mpv's `glsl-shaders`, both when the core is created and on every later change, so a chain picked before the engine exists is still honoured on the first frame. `player/anime4k` stores that id: the ten built-in ids are frozen (`off`, `fast_a`…`hq_ca`) so an existing selection cannot be repointed, and the experiments have permanent readable ids (`exp_thin_hq`, `exp_deblur_strong`, …) rather than positions in a menu. PiP copies both the mode and the applied chain id into the float's own core; the info chip shows a nested experiment qualified by its category (`Тонкі лінії · HQ`). The Compare window borrows the live surface to render each side and puts the player's own chain back when it is done, so a comparison never leaves the player running a preview chain. At startup the app searches `<exe directory>/shaders`, then `./shaders`, then `./build/release/shaders`; when none exists, Anime4K controls are hidden. The shader `//!WHEN` conditions can decline to upscale when the output is not larger than the source, so enabling a chain does not guarantee every stage changes every frame.

**Build and language.** Implementation and build configuration are C++20/CMake; shader programs are GLSL. The 49 source assets live in root `shaders/`. `CMakeLists.txt`'s `mikai` POST_BUILD rule copies that directory beside `mikai.exe`; CMake does not precompile GLSL—mpv loads the copied files at runtime. Build from the configured tree with `cmake --build --preset app --parallel`. The Anime4K categories, preset labels and hints are localized UI text; the future-model submenus intentionally remain placeholder-only with a disabled **В планах** leaf each. Keep menu labels and tooltips in translation catalogs; keep preset IDs and shader filenames unchanged because settings and chain resolution depend on them. `cmake --build --preset release --target anime4k_menu_probe --parallel && build/release/anime4k_menu_probe.exe` runs the full native Anime4K smoke (every menu leaf, pixel rendering, settings reopen, Compare groups, PiP chain and minimum-window fit); `build/release/resolver_probe.exe` verifies the 71-candidate catalog resolves cleanly.

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

`src/backend/` is the domain — `MikaiClient` (rate-limited Qt Network calls), models, HLS extraction (`ashdi`/`moon`/`tortuga`) and stock mpv discovery/provisioning — and `src/app/` is the Qt Widgets frontend (`MainWindow`, `SearchPage`, `TitlePage`, dialogs, theme tokens), with the player (`src/player/`: `PlayerDialog` → `PlayerShell` → mpv-backed `VideoSurface`, over the once-pinned `MpvApi` dynamic module loader) driven in-process through a `PlayerHost` bridge, and `shaders/` holding the Anime4K `.glsl` presets that CMake copies next to the executable. `docs.md` is the authoritative API contract and `AGENTS.md` the map for contributors: architecture, conventions, build commands and the testing setup. Run the checks with `ctest --test-dir build-cmake -R mikai_tests`.

`native/README.md` covers the optional native (AnimeJaNai) track — source pins,
acquisition records and the licence basis; the findings and their resolutions
live in `native/manifest/components.json`. This project's own native integration
is covered by the owner grant of 2026-10-04 (unrestricted OpenSource use of this
project's own integration and its recorded decisions); third-party components
keep their own terms, and the inference shim's upstream terms are not verified.
The four RIFE v4.14 graphs are acquired and their stored weights match the
author's MIT checkpoints after fp16 rounding; the residual whole-graph questions
are recorded by owner decision rather than answered. The RIFE, RealESRGAN and Інші моделі panels
all remain placeholders and no model in them executes, so the app performs no
native processing. The optional MSVC `native/` project builds the shim from the
pinned, patched source and has produced raw TensorRT plans on a real GPU. The
shim also now compiles and links with the additive ABI v2 snapshot patch, and
its `native_abi_probe` reports ABI 8 unchanged with protocol/schema 2/2 and a
real-context probe that stays `Unavailable` (no `Prepared`/`Active`). Two
RIFE engines (lite and lite ensemble) and the spatial HD Performance engine
verify numerically against the CPU reference; RIFE full and ensemble are
rejected on `maxAbsoluteError`. Nothing is `Ready` or `Active`, no complete
prepared recipe exists, and no native frame has been rendered. The pinned mpv
fork core builds and its isolated core loads (exit `0`), and a Lua-enabled
candidate plays real 320×180 HEVC video through the owned runtime with Native
off (0 decoder and 0 VO drops), but the bundle candidate still aborts
verification on a `GGML` initializer (fix in flight, no PASS yet). It is not on
this build path and no native SDK is needed to build the app.

The `mikai_tests` target and shader-using probes deploy `shaders/` beside their
executables during their own builds; building only the test target in a fresh
directory no longer requires building `mikai` first to supply those assets.
