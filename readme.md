# Mikai Wrapper

A C++/Qt6 desktop client for the [Mikai public API](https://api.mikai.me/public/docs).

## Build

Needs MinGW-w64 g++, Qt 6.8.x with LinguistTools, and the MinGW toolchain
paths configured by `CMakePresets.json`. During configure, CMake acquires the
pinned Windows x64 mpv developer archive or validates the supplied
`MIKAI_DIST_MPV_ARCHIVE` against its pinned SHA-256, extracts that release's
headers, and compiles against them. The root build copies only
`libmpv-2.dll` beside the app; it does not package `mpv.exe` or use IPC.
The toolchain paths have to be passed explicitly — a bare `-G "MinGW Makefiles"`
fails with `CMAKE_MAKE_PROGRAM is not set` when the toolchain is not on `PATH`.
The API DLL is from the official `the-database/mpv-winbuild` release
`2026-10-07-d6d93599d5` (`mpv-dev-x86_64-20261007-git-d6d93599d5.7z`,
SHA-256 `077cb75fed47b185428f97224e1d798e2d4c2f4063fd8cda2662b74f2892327c`).
Although that release includes the ABI-8 AnimeJaNai filter, it does not include
this project's Native status/admission protocol. The filter requires the separate Aji/backend/model closure; packaging the API
DLL alone does not establish inference. The fork also includes faster ASS and
display-rate-match changes. GPU subtitle blur/compositing/rasterization requires
gpu-next and forklibass and is not claimed on this OpenGL surface;
display-rate-match remains off by default and is limited to local seekable
mkv/lavf media. Keep the prebuilt release package local/private: public
redistribution is not cleared until complete corresponding source, dependency
licences, and relink obligations are satisfied.

```sh
cmake --preset release
cmake --build --preset app --parallel
```

Then run `build\release\mikai.exe`. The app loads the packaged `mpv/libmpv-2.dll`
through libmpv's C and render APIs in its existing Qt `QOpenGLWidget` GUI;
there is no `mpv.exe` child process or IPC. Runtime discovery never downloads
mpv and has no stock-cache fallback: when the packaged module is absent,
playback is unavailable. A probe or test can pin a module with
`MIKAI_MPV_MODULE`. The selected module is pinned for the process — main
playback and the PiP float share it — and changing the runtime takes effect only
after a restart.

## Interface languages

Mikai's interface is available in English and Ukrainian. English is the source language in the C++ `tr()` and `QCoreApplication::translate()` calls; the Ukrainian catalog is `translations/mikai_uk.ts`. In **Settings → General → Interface language**, choose **Use system language**, **English**, or **Ukrainian**. System mode selects Ukrainian only when the operating-system locale is Ukrainian and otherwise uses English. A language change takes effect immediately after Save; a catalog that cannot be loaded is refused and leaves the working language unchanged.

To refresh the catalog after changing source strings, build the `update_translations` target, then translate the new entries in `translations/mikai_uk.ts`:

```sh
cmake --build build/release --target update_translations
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

`windeployqt` does not copy mpv; the `mikai` target places only
`libmpv-2.dll` under `mpv/` beside the executable, and `dist` preserves that
API-DLL-only layout. Runtime discovery does not download a replacement, so
without that packaged module playback is unavailable. The developer archive is
not part of the application runtime package. `--translations uk` deploys Qt's
Ukrainian catalogs, and the `mikai` build copies `qtbase_uk.qm` from the
configured Qt installation into the executable's `translations` directory.

`mikai.exe` is `WIN32_EXECUTABLE`, so it has no console window. Diagnostics go
to `mikai.log` beside the exe, not to stdout: every Qt message is appended
there, it rotates to `mikai.log.1` past 2 MB, and an unhandled exception also
writes `mikai-crash-<timestamp>.txt` carrying the exception code, the faulting
module, and the tail of the session log. A session that starts with an old
crash report still on disk says so in the log.

The `dist` target assembles a self-contained folder into `dist-mikai/` instead:

```sh
cmake --build build/release --target dist
```

That runs `windeployqt` (so a manual `windeployqt` call is not needed), copies
the 49 shaders, translations and only `libmpv-2.dll` under `mpv/`, and writes
`run.bat` and `VERSION`. No `mpv.exe`, developer archive, or IPC runtime is
included, and runtime discovery never provisions a replacement: if the API DLL
is absent, playback is unavailable. `MIKAI_DIST_NATIVE` (OFF by default)
additionally copies the installed `native/runtime`. The dist never ships the
downloadable elements — GPU-specific TensorRT packs and licence-restricted
model graphs remain per-machine assets — and any `components/` or `assets/`
tree a copied native bundle carried is removed.

This target recreates `dist-mikai/`. Do not run it over a user's installed
folder: an executable-only UI update copies the verified `mikai.exe` in place
and preserves downloaded assets and install records. The most recent such update
followed that rule — the gated application was copied in place into
`dist-mikai/mikai.exe` only, with a matching digest and no `dist` run, the three
install records (Rife 3.7, Aji 0.9 trt, TensorRt 3.7) kept their hashes, and the
actually installed executable started, painted a visible Ukrainian feed and
closed normally (`.probe-out/ui-audit-installed-app-startup-current.log`,
`dist-mikai/packaged-app-startup.png`). That is one observed packaged update, not
final acceptance; the fresh-engine, black block and Compare gates are open.

## Tests

```sh
cmake --build --preset tests --parallel
ctest --test-dir build/release -R mikai_tests
```

### Behavioural probes (JSON manifests)

`probe_runner` is a single executable that runs declarative probes, so adding
coverage means adding a JSON file rather than a new binary:

```sh
cmake --build build/release --target probe_runner --parallel
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
`cmake --build build/release --target mikai_http_fixture` and start
`build/release/tests/fixtures/mikai_http_fixture.exe --port 0 --facts facts.json
--require-accept-language`; it serves the real clips under `tests/fixtures/media/`
and the three HLS renditions under `tests/fixtures/hls/`, with `/media.mp4` as the
plain clip and `/retry.mp4` as the same bytes after one deliberate 404, which is
what the retry phase needs.

Refused actions have independent scenarios rather than a combined browser/
clipboard mode:
- `build/release/external_media_probe.exe --player-clipboard-failure <localclip>`
  uses Windows QPA and a separate process holding `OpenClipboard`, exercising a
  real hostless player's link/frame actions, visible accessible refusal and
  preservation of existing clipboard formats.
- `build/release/title_page_probe.exe --host-browser-refusal-only` uses offscreen
  QPA for a real browser-open refusal.
- `build/release/title_page_probe.exe --host-copy-refusal-only` uses Windows QPA
  and the separate clipboard holder for a real copy refusal.

The two title scenarios use the real API/title session and active player's
warning dialog. They must run separately under their respective QPA platforms;
the combined `--host-refusal-only` mode is removed. These new scenarios are
documented invocation contracts, not observed passes; final runs are pending.

`mikai_tests` also carries the runtime-loader and native-contract suites
(`TestRuntimeSelection`, `TestMpvAsync`, `TestNative*`); they run hermetically,
exercising a real core only when `MIKAI_MPV_MODULE` names one and skipping
otherwise. `TestNativeComponents` covers the pinned catalogue, the store record
round-trip, digest re-verification that catches a same-length replacement, and
the toolchain's four source rules.
An earlier reported union build had zero errors or warnings, CTest passed in
13.78 seconds, and its Ukrainian catalogue had 999 finished entries and none
unfinished. These pre-cleanup observations are not current-tree acceptance or
Native inference proof. A later composition binary's hermetic CTest passed in
18.22 seconds, but that binary also predates the native traversal correction, so
it is not the current tree either.

The source-first follow-up build compiles the app, tests and relevant probes with
exit 0, zero warnings and zero errors
(`.probe-out/ui-audit-final-reachable-build.log`). Full CTest passes in 26.53 s
(`.probe-out/ui-audit-final-reachable-hermetic.log`); `lupdate` finds 997 existing
strings and no new strings. Settings, RIFE settings and missing-runtime refusal
checks pass; exact evidence and remaining gates are in `native/README.md`.

Only the verified executable was replaced in `dist-mikai`; the three protected
component records have unchanged hashes. The installed app paints its Ukrainian
home feed and closes normally, exit 0
(`.probe-out/ui-audit-final-installed-app-startup.log`). This is not GPU-playback
acceptance: Windows session 1 is disconnected, fresh Qt contexts receive Mesa
software OpenGL, and mpv refuses that context. Cold-engine pause/completion,
black-block playback and final Compare checks require the NVIDIA-backed desktop.
Tests and probes are hand-run, not CI.

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

That is builder-invocation proof only. In an earlier run, the spatial engine
was **rejected** by `mikai_native_verify` because `maxAbsoluteError` exceeded
the FP16 tolerance. Earlier RIFE runs serialized all four graphs; lite and
lite-ensemble verified numerically while full and ensemble were rejected on
`maxAbsoluteError`. These historical private-verifier observations are separate
from the later successful official Aji menu-driven builds and playback checks
recorded in [the native guide](native/README.md#current-observed-integration-state).
A verify pass covers raw engine bytes,
bindings and numerics only (`manifestVerified: false`, `prepared: false`), so
nothing is `Ready` or `Active`. The pinned mpv fork core builds and its isolated
core loads (exit `0`), and a Lua-enabled candidate now plays a real 320×180 HEVC
24 fps video through the owned runtime with **Native off** — 921600/921600
non-black samples, 290264/921600 motion, 0 decoder and 0 VO drops, 0 live `vf`
chain entries — with an inspected colored 1280×720 motion pattern, and
`widget_pixel_probe` passes 21/21 with 0 skips (colored 640×360 PNG, GL 4.6,
0 `ErrorType`, `glGetError` 0, 30.44% motion); that is the runtime surface only,
not the native filter, CUDA interop or active inference.
An earlier bundle candidate verification stopped on a `GGML` initializer; that
historical result is not a current packaged-application launch result. The
current packaged window launch/shutdown and remaining Native proof limits are
recorded in `native/README.md` and `native/build-msvc.md`.

The window-driving probes need the interactive desktop session attached: with
the session detached there is no GL context and no video output, so every
pixel-counting proof reports a black frame (`hasVideo=0`). With the session
attached, earlier recorded runs passed `interaction_probe` 149/149 and
`compare_probe` 30/30 (frozen-frame proof, 0.000/255 noise floor), with
`hls_recovery_probe` and `external_media_probe` reporting `RESULT: PASS` (real
expired-signature recovery; real picker, HTTP, same-URL retry and file URL).
A later composition binary passed the full PiP interaction probe 171/171, but
that binary also predates the native traversal correction and the runtime-cause
fix, and its reentry check still only compared position, so it is neither
acceptance of the new isolated Compare nor of pooled PiP resize/reentry. The
current post-fix interaction gate passes 172/172 with two non-corner free
placements at different sizes and pooled reentry; it still does not close the
Compare, pixel or fresh-engine gates. The current external-media, HLS, CTest,
refused-action, Settings and operation outcomes are recorded in
[the native guide](native/README.md#current-observed-integration-state).
`native_settings_probe`, `rife_settings_probe` (both languages),
`native_menu_probe`, `native_status_probe`, `runtime_selection_probe`,
`spatial_assets_probe` and the ORT-backed `native_assets_probe`
(`MIKAI_ORT_RUNTIME_DIR` naming the runtime directory) have recorded non-desktop
results; that is not a claim that the current cleanup has been rerun.
`native_component_probe` is the falsifiable check for the TensorRT pack choice:
`--print` reads the real device and the resolved bin directories, `--fetch`
pre-populates the store and prints every installed file's size and digest, and
`--from-sdk` stages a same-release runtime+resource pair and runs the real
builder — with the GPU's own pack it builds (`RESULT: PASS`), and with another
generation's it fails naming the missing resource (exit 4), which is what proves
the selection decides the outcome. `engine_popup_probe` drives the player's preparation window through its public
API: no window before announcement, Cancel reaching the host, named persistent
failure, completion dismissal, and completion-timer isolation between builds.
Its earlier results do not certify current engine-pause behavior. The
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

The pixel clock-0 defect was diagnosed and fixed: the capture defers **one** seek
until `hasVideo` and `duration > 0`, cycles subtitles to none only for the existing
reference fixture, and waits for `QOpenGLWidget::frameSwapped` at the target
timestamp with the window active before capturing. A windowed 1366×975 logical run
at DPR 2 (a 2732×1950 screen) at 6.333 s / frame 152 with NVDEC (`cuda[nv12]`)
then took the screen surface first and matched the FBO on 5 of 5 captures with 0
unavailable, covering 4,196,051 of 4,196,352 reference-lit pixels (99.99%) with a
clean inspected image (`.probe-out/ui-audit-canonical-screen-first-windowed.log`).
The original main-window and Compare black blocks remain open, the user's first
full-screen Alt-Tab occlusion is acknowledged and excluded, and the later
full-screen all-black-screen run with a clean FBO has an unproven cause that must
not be attributed to DWM or GDI as fact. The main/Compare windowed diagnostic run
is owned by the source owner and still in progress. The original 6.333 s /
frame 152 report was reproduced against a known fresh CPU-decoded reference at
that canonical file, so the input file is not corrupt, and hardware decode versus
rendering versus on-screen compositing remains unseparated.

`mikai_tests` is hermetic except the live oracle, which runs only with both set:

```sh
set MIKAI_LIVE=1 & set MIKAI_API_KEY=mk_... & ctest --test-dir build/release -R mikai_tests
```

## What it does

Labels shown in these examples use Ukrainian; the interface can also run in English.

- **Home** — carousels for titles you recently opened, titles airing now, and Mikai's current popular picks, plus the latest episode updates and the weekly airing schedule. Every card shows the poster, the airing status and the aired/planned episode count, and a search result row reads the same way (`Бліч · 2004 · TV · Завершено · 366 серій`) straight from the list response, with no extra request; right-click a card to open it, copy its mikai.me link or raw id, or open the site page in a browser.
- **Title** — description, cover, scores, genres and related titles, then one tab per release (voice or subtitles, grouped by dubbing team). Episode rows sit flush with the bottom edge when they fit; longer lists scroll within the episode pane. Each episode opens the player at the qualities the provider actually has; the right-click menu adds copy-URL-only, re-extract, and hand-off to an external player. The cover opens full size with pan and zoom.
- **Player** — a player built around the same Qt event loop as the app: the `PlayerDialog` composition root drives the `PlayerShell` chrome and the mpv-backed `VideoSurface` in-process. It opens full screen, and the `><` / `<>` buttons beside the PiP icon — or a double click on the picture, or `F` / `F11` — toggle a real decorated native window, so leaving full screen restores the previous windowed geometry and the OS titlebar handles dragging, resizing, snap layouts and moving the window to another monitor. The toggle is scoped to the player; the app's main window is unaffected. The picture fills the window edge to edge, while a two-row dock keeps the large play control and permanent volume slider above a full-width timeline. The chrome fades after a couple of seconds of stillness and returns on the next mouse move. A top-left chip reports available throughput, estimated filter FPS and source-to-output resolution; official Aji status distinguishes filter configuration from observed backend processing and clears stale evidence on Off/source changes. Click the picture to pause, step through episodes from the drawer (S), switch the dub/sub track (T) or the stream quality (Q) without leaving the player. A picture-in-picture float has its own playback core and overlaid controls — click to pause, hold and drag to move it. Drops are freely placed without corner snapping; pooled hide/reentry preserves custom size and position, correcting only placement unreachable from every monitor work area.
- **Anime4K** — the dock's sparkle button opens **Покращення**. Anime4K presets remain live-core shader choices; the official AnimeJaNai ABI-8 filter is a separate enhancement path, not the project's private-prepared Native protocol. The menu offers spatial choices and RIFE choices from the official 47-model archive. The upstream v0.9.0 Aji path is distinct from the private prepared Native path; its 4262 heavy-model extension exists only in the local custom import, not the public downloader's pinned upstream v0.9.0 package. The custom import has an observed ABI-8 install record and supports the heavy selection, but configuration/import does not establish a Ready or Active inference chain. Unsupported choices report their reason rather than substituting another model. Keep `vo=libmpv` on the existing Qt OpenGL surface; no gpu-next-only subtitle capability is claimed.
- **Settings** — the API token with its live quota, a checklist of home blocks, and an **Interface language** selector for system, English, or Ukrainian. A disabled block is never requested. **Presets** assigns Anime4K shortcuts; duplicate assignments and reserved player keys are rejected. **Processing** shows the runtime choice and the official AnimeJaNai entry points on Standard. The separate private-prepared editor, model import, preparation/cancellation and artifact cleanup appear only when Native is deliberately selected or already loaded. The private inventory refreshes lazily when its visible Processing surface needs it, not on ordinary Standard settings opens. There is no Native processing checkbox; Save and Cancel preserve hidden private requests and unrelated settings. Runtime changes require a restart. **Components** owns user-initiated installation and verification of backend packs and the 47-model RIFE archive; startup downloads nothing.
- **Backend components** — Settings → Components describes user-initiated downloads of the pinned AnimeJaNai 3.7.0 TensorRT runtime/resource archives and 47-model `component-rife.7z` archive (462,444,035 bytes; SHA-256 `d41de2b56972cb3c446d9830bd5c5e7257e6e099ef0e7a2d5770ec01a9bf7251`). On the RTX 5090 (SM 12.0), the upstream component query recommends `trt-runtime` and `trt-sm120`; `trt-sm120` is preselected for a new install. That query is discovery only and installs nothing. Optional existing notice files are preserved; the acquired TensorRT component archive itself has no package-wide notice members. The local custom ABI-8 import adds v4.26 heavy mapping not present in public upstream v0.9.0; the public downloader remains pinned to v0.9.0 and does not fetch the custom import by default. Archives/models in the per-machine store do not establish private prepared-engine capability or public redistribution clearance. `dist` remains API-DLL-only: inference binaries and model payloads are not shipped. ESC-1/2/3 remain open; no FPS claim is made.

Official AnimeJaNai activation belongs to the player's **Enhancements → AnimeJaNai**
preset menu, without a separate Native-toggle shortcut. On the Standard runtime,
saved private Native requests neither attach the private filter nor display its
unrelated status chip or duplicate enhancement menu; the requests remain saved
for a later Native session. RIFE selection belongs to
**Enhancements → AnimeJaNai → RIFE models → version range → version → variant**,
using the selected backend and verified model. A refused choice explains the
missing or unsupported requirement; it never substitutes another model.

Earlier GUI/component evidence and selected-model fresh-engine runs are recorded
in [the native guide](native/README.md#current-observed-integration-state).
They do not certify the latest cleanup. The handoff's cached selected-RIFE
selection/render observation also does not prove a fresh build with the new
pause/progress UI. No all-model inference, numerical fidelity, private
Ready/Active, or redistribution clearance is implied.

Engine preparation/building uses an always-on-top operation window with the
actual phase, elapsed time and visible completion or failure. TensorRT's public
backend does not provide a measurable percentage, so its build remains
indeterminate; plan stage counts are not invented engine-build progress. The
actual playback owner stays paused throughout official or private engine work.
Explicit Play/Pause intent is retained until terminal acknowledgement; source
replacement clears the old hold. A cold player build was observed paused but
stalled before completion because its filter poll command omitted a mandatory
empty argument. That argument is fixed, and the traversal correction landed since.
The latest fresh natural-shape 512 attempt aborted at the cursor before any preset
was selected, so no engine was built, and that run is being redone in a separate
Qt visible-widget mode with its own owner. So a fresh 512×288 completed-player
proof of pause, progress, completion and intent restoration is still open, and the
real operation-window API gate (69/69) does not close it.

Refused copy/open actions report the actual failure rather than a success
message. The canonical text/image helper is `src/gui/Clipboard.hpp`,
`mikai::gui`; direct-player refusals are visible and announced to accessibility,
while hosted-title failures warn on the active player. A negative helper's
`OpenClipboard(NULL)` was proven non-exclusive by a live child plus an
independent parent native open, so the clipboard probes now carry a hidden unique
HWND/PID proof; they are the Windows-QPA negative cases, and after the cause fix
the player clipboard gate covers 16 checks and the Search clipboard gate 1. The
Search browser gate passes 1 check under `QT_QPA_PLATFORM=offscreen`, and an
offscreen browser needs `C:/Windows/Fonts` on its font path for a readable
surface. The title browser gate passes actual action, visible warning, parent,
accessible `Alert`, screenshot and no-false-success checks with the session intact
(`.probe-out/ui-audit-title-browser-refusal-after.log`, exit 0), and the title
copy gate under Windows QPA passes the same flags
(`.probe-out/ui-audit-title-clipboard-refusal-after.log`). The probe's own
selector defect is fixed: it had used `findChild<QPushButton*>` while the real
control is a typed `PillButton`, so it now finds a `PillButton` parent and drives
it with `QTest::mouseClick`.

When playback cannot start, the surface's own synchronous `MpvError` is what the
viewer reads. Failed-open callers used to replace that named cause with a generic
"Could not play" line; all eight callers now share one central helper that
preserves it, and no Aji plan is applied from a core that failed to open. A
refusal also raises `QAccessible::Alert`, so a screen reader announces it. On the
baseline that alert did not exist at all — the gate observed 0 of them and failed
— and the real missing-runtime consumer then hit heap `c0000374`:
`VideoSurface::~detach` emitting to an already-destructing `PlayerDialog`, so the
surface's destructor now disconnects early and guards on its closed flag.
Observed after the fix: the runtime-refusal cause gate passed 52 requirements with
0 skips and exit 0, naming the actual missing Root/DLL cause, actually dropping
the request and closing cleanly. The full hermetic CTest suite after the same fix
is 100% in 15.69 s (`.probe-out/ui-audit-failure-cause-hermetic-after.log`).

The upstream AnimeJaNaiManager Chains/Presets handoff is a reference, not a
claim or request to port its complete custom-profile editor or its shortcuts.

### Anime4K handoff

**What it is.** `src/player/Anime4K.cpp` holds one declarative catalogue: for every preset it carries the stable id, the localized menu label and hint, the category it hangs under, and the *ordered* shader chain. `availableModes()` returns that table and `menuRows(parent)` returns the level above it as the rows a panel renders, so nothing else keeps a second copy of a chain - adding a preset is one table entry. `shaders/` contains 49 individual GLSL files; a preset is an ordered chain, not a separate shader. The Compare window also uses this catalogue; its additional spatial choices and rendering contract are described below.

**Menu.** The dock's sparkle button opens **Покращення** with **Anime4K ›** and **AnimeJaNai ›** on Standard. Private prepared-model menus appear only with a loaded Native runtime. The menu is a tree rather than a sequence of screens: resting the pointer on a row opens the level under it as a panel *beside* the panel that row sits in, and every level stays on screen, so there is no **‹ Назад** row anywhere. From the root that is the Anime4K level (**Вимкнено**, then the four categories), one panel per category, then one per experiment family - four panels at the deepest leaf, each top-aligned to the row that opened it and placed on the side of the tree with room. Clicking a category row opens the same panel, so the tree is reachable without a pointer motion, and clicking a preset row applies it and closes the whole tree. A category row only drills down - it never selects a shader - and each top-level category carries its purpose as a tooltip (*Швидкість* low GPU use in real time, *Якість* balanced quality, *Максимум* multi-stage at a higher GPU cost, *Експериментальні* special cases that are not defaults). The family names inside *Експериментальні* are Anime4K's own (*Noise Reduction*, *Thin Lines*, *Texture Enhancement*, *Darken*) with Ukrainian tooltips. The row holding the current preset is highlighted, including the closed category it sits in.

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

**All 49 shader files**, grouped by role; each entry below expands to `Anime4K_<name>.glsl`:

- Upscale — CNN: `Upscale_CNN_x2_{S,M,L,UL,VL}`; denoise CNN: `Upscale_Denoise_CNN_x2_{S,M,L,UL,VL}`; GAN: `Upscale_GAN_x2_{S,M}`, `Upscale_GAN_x3_{L,VL}`, `Upscale_GAN_x4_{UL,UUL}`; other: `Upscale_Original_x2`, `Upscale_DoG_x2`, `Upscale_Deblur_{Original,DoG}_x2`, `Upscale_DTD_x2`.
- Restore — CNN: `Restore_CNN_{S,M,L,UL,VL}`; soft CNN: `Restore_CNN_Soft_{S,M,L,UL,VL}`; GAN: `Restore_GAN_{UL,UUL}`.
- Denoise: `Denoise_Bilateral_{Mean,Median,Mode}`.
- Deblur: `Deblur_{Original,DoG}`.
- Darken: `Darken_{VeryFast,Fast,HQ}`.
- Thin lines: `Thin_{VeryFast,Fast,HQ}`.
- Other: `Clamp_Highlights`, `AutoDownscalePre_{x2,x4}`, `3DGraphics_Upscale_x2_US`, `3DGraphics_AA_Upscale_x2_US`.

**Runtime and persistence.** Every preset and its localized hint comes from the catalogue, and a menu row selects by that preset's id - never by an enum ordinal or a menu index. `VideoSurface` resolves the applied *id* (not the mode) to ordered absolute paths and writes them to mpv's `glsl-shaders`, both when the core is created and on every later change, so a chain picked before the engine exists is still honoured on the first frame. `player/anime4k` stores that id: the ten built-in ids are frozen (`off`, `fast_a`…`hq_ca`) so an existing selection cannot be repointed, and the experiments have permanent readable ids (`exp_thin_hq`, `exp_deblur_strong`, …) rather than positions in a menu. PiP copies both the mode and the applied chain id into the float's own core; the info chip shows a nested experiment qualified by its category (`Тонкі лінії · HQ`). Compare rendering is isolated from that live pipeline. At startup the app searches `<exe directory>/shaders`, then `./shaders`, then `./build/release/shaders`; when none exists, Anime4K controls are hidden. The shader `//!WHEN` conditions can decline to upscale when the output is not larger than the source, so enabling a chain does not guarantee every stage changes every frame.

**Build and language.** Implementation and build configuration are C++20/CMake; shader programs are GLSL. The 49 source assets live in root `shaders/`. `CMakeLists.txt`'s `mikai` POST_BUILD rule copies that directory beside `mikai.exe`; CMake does not precompile GLSL—mpv loads the copied files at runtime. Build from the configured tree with `cmake --build --preset app --parallel`. The Anime4K categories, preset labels and hints are localized UI text. Keep menu labels and tooltips in translation catalogs; keep preset IDs and shader filenames unchanged because settings and chain resolution depend on them. `cmake --build build/release --target anime4k_menu_probe --parallel && build/release/anime4k_menu_probe.exe` runs the full native Anime4K smoke (every menu leaf, pixel rendering, settings reopen, Compare groups, PiP chain and minimum-window fit); `build/release/resolver_probe.exe` verifies the 71-candidate catalog resolves cleanly.

Qualities are extracted once per episode, only when that episode opens, and the resulting stream URLs are cached for half an hour, so reopening a title repaints instead of re-hitting every provider. The links are signed and do expire; if a stream fails to play the player re-extracts it once on its own before reporting the error.

### Compare

Compare exposes five layouts: Split H, Split V, Side-by-side, Flicker and
Difference. Mode-specific controls hide when irrelevant, including on first
opening. Fit accounts for the active layout, including the side-by-side grid;
wheel zoom works on both the canvas and grid panes. Alt+drag pans; ordinary
drag in split modes moves the divider.

Only the two model-name corner badges open choosers, and the chooser closes
before rendering begins. Their options are the player's 24 stable-ID Anime4K
presets (including Off) plus five public AnimeJaNai spatial models. Loose shader
files and temporal RIFE models are excluded: a single frozen image cannot
measure interpolation. Missing/unsupported backend or model closure is refused
with its actual reason, never a fallback.

`CompareRenderer` owns a separate libmpv core, offscreen GL context, surface and
framebuffer. At the held playback timestamp it captures one lossless source PNG
and loads that immutable image once. Both sides use real shader or Aji spatial
processing on that source; Off returns the canonical source image. No comparison
chain is applied to, or restored onto, the live player. Its timestamp, source
pixels and processing remain untouched by side rendering, with a source/pause
hold and private Native plan lock protecting the captured epoch.

Changing one choice retains the unchanged side; only the two current results
are kept, and identical choices share one render. Failures name the affected
choice rather than relabelling stale output or blanking the unaffected side.
Closing, cancellation and source replacement invalidate pending work. Each
opening captures a fresh source rather than falling back to an earlier frame.
Canonical shader and spatial probe runs have passed. A fresh 416×240 run,
however, failed an early live-surface Off restoration check — 0.676/255 against
an unchanged 0.05 tolerance — despite passing 462 of its checks, and a
subsequent canonical startup exited 5 after only its CLI line; that exit-5
startup is a historical, unexplained result, since a later normal canonical
startup ran under GDB and reported `RESULT: PASS`, which is the latest known
startup outcome. Neither startup closes the 416×240 rendering gate. The
residual's cause remains unproven; no dithering explanation or relaxed tolerance
is justified. Exact outcomes and unresolved runtime gates are recorded in
[the native guide](native/README.md#current-observed-integration-state).
These observations do not constitute blanket acceptance of the integrated tree;
earlier live-render Compare results are not proof of this isolated implementation.

## API limits

The **RPM** indicator beside Settings shows the server-reported requests used in the current minute and the limit that applies. Calls wait for the server's reset window instead of failing when the allowance is spent. An optional API key raises the standard limit from 60 to 300 requests per minute and is stored locally in `.env`, which is never committed.

## Layout

`src/backend/` is the domain — `MikaiClient` (rate-limited Qt Network calls), models and HLS extraction (`ashdi`/`moon`/`tortuga`) — and `src/app/` is the Qt Widgets frontend (`MainWindow`, `SearchPage`, `TitlePage`, dialogs, theme tokens), with the player (`src/player/`: `PlayerDialog` → `PlayerShell` → mpv-backed `VideoSurface`, over the once-pinned `MpvApi` dynamic module loader) driven in-process through a `PlayerHost` bridge, and `shaders/` holding the Anime4K `.glsl` presets that CMake copies next to the executable. `docs.md` is the authoritative API contract and `AGENTS.md` the map for contributors: architecture, conventions, build commands and the testing setup. Run the checks with `ctest --test-dir build/release -R mikai_tests`.

`native/README.md` distinguishes official AnimeJaNai ABI-8 filtering from the
private prepared-Native track and records acquisition/build/playback evidence
with its limits. Keep public Aji v0.9.0 downloads separate from explicit local
custom ABI-8 imports. Third-party redistribution issues ESC-1/2/3 remain open;
project-owned integration permission does not clear upstream terms. Exact
digests and model-rights reasoning belong in the acquisition records and
manifests rather than a duplicate acceptance ledger here.

Alongside that track, the GPU-adaptive TensorRT components are fetched at run
time rather than shipped: the pinned AnimeJaNai `3.7.0` packs (`trt-runtime`,
one builder resource per published generation, and `trt-ptx`) carry pinned
asset byte counts and SHA-256; the app reads the compute capability from the
installed driver's NVML, selects the exact `trt-sm{MA}{MI}` pack (or `trt-ptx`
when the release publishes none for that generation), downloads the missing
set on this machine, and proves every file by digest into the app-owned
`%LOCALAPPDATA%` component store. A bundle that already declares this GPU's
resource is used as-is and nothing is downloaded. Bundle validation is served
from a stat-only cache (`verified.json` under the app-owned native root): the
first start after a change re-hashes (about 18 s here), every start after lands
in about half a second. The build enables `-Wall -Wextra -Wno-unused-parameter
-Wno-cast-function-type`; final warning counts belong to the integrated build.

The `mikai_tests` target and shader-using probes deploy `shaders/` beside their
executables during their own builds; building only the test target in a fresh
directory no longer requires building `mikai` first to supply those assets.
