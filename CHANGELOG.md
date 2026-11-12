# Changelog

Published changes are recorded with their downloads on the [GitHub releases page](https://github.com/smyhlin/Mikai-Ja-Nai/releases). Artifact checksums live with each release, not in the documentation.

## 0.2.3 — Unreleased

This release introduces the player-only Qt Quick/QML frontend while retaining the C++ playback engine and Widgets application pages.

- Upgrade Standard AnimeJaNai to v0.24.0 (ABI 9) across Windows and Linux, updating official runtime assets and SHA-256 cryptographic pins.
- Add a Cut moment editor for selecting, previewing and exporting a short MP4:
  - Rebuilt as a token-driven filmstrip sheet bounded to ±2 minutes around the current playback anchor.
  - Preview playhead moves independently of selection boundaries and can be scrubbed before, on top of, and after the clip; preview playback loops within the selection.
  - Preview audio is enabled by default with a dedicated mute button (`M`).
  - Size estimate dynamically derives from the observed stream bitrate, scaled by target quality height and capped at the encode target.
  - Transport discs follow the global corner radius, preset chips share a uniform 52 px width with tabular numerals (`tnum`), and preset chips hide responsively below 700 px width.
- Overhaul Compare quality workstation for native AnimeJaNai models:
  - Replay models from the real source through an isolated core using the player's decoder (`nvdec`/`d3d11`) and filter string verbatim without format normalisers or loop filters.
  - Frame-step to the held timestamp rather than reloading the source, preserving configured stage instances and ring depth.
  - Render offscreen strictly on `MPV_RENDER_UPDATE_FRAME` without advanced control to eliminate timer-driven stale frame presentation.
  - Enforce delivery evidence: `video-out-params` must match the stage's configured output geometry and a frame must be presented post-reconfiguration.
  - Add `PictureMetrics` with scale-normalized reference detail (measured on copies normalized to ≤1024 px) to prevent 4× models from false flat-frame refusal, plus low-contrast and blank model refusals.
  - Write comparison stills off the GUI thread on worker pools, and verify read-backs against presented frames via `screenshot-raw` (`bgr0`) oracle.
- Modernize Settings and Components management:
  - Settings Save button commits preferences immediately without closing the dialog, providing an inline "Saved" acknowledgement.
  - Components tab features a compact top toolbar under the backend dropdown with a tri-state "Select all" checkbox, live selection counter, ghost "Verify", and "Install selected" (demoted from primary save-like styling, disabled when 0 items selected).
  - Obsolete "Refresh" and "Install local runtime..." buttons and manual import paths removed; acquisition "Cancel" appears only while active.
  - Components backend dropdown immediately stores the global engine (`player/animejanai/backend`), which the Benchmark tab toolbar reflects and follows; release inventory scanning latch on executor rebinding.
- Fix player startup by removing an unsupported Qt Quick accessibility property from the speed slider.
- Player and discovery enhancements:
  - Replace the player presentation with a native Qt Quick window and QML scene backed by a C++ `PlayerController` and the existing `VideoSession`; PiP changes the presentation of that same window/session rather than reopening media or creating another decoder. Playback and renderer lifetime remain in the player core, while Compare, Clip, Media Information, Settings and Components remain Widgets interfaces.
  - Keep frame composition tied to the live session and its current render target across window changes; stale media/session work is rejected rather than applied to a replacement source.
  - Replace stock `QMessageBox` resume playback prompt with a custom themed dialog matching app design tokens.
  - Remove vertical scrollbar from discovery feed for clean full-width display; vertical scrolling supported via mouse wheel and explicit Alt + mouse wheel.
  - Group processing controls into Restore, Upscale and Interpolation; improve Anime4K shader selection.
  - Remove the duplicate fullscreen button from the bottom player controls and place the top-right window buttons on a themed surface for visibility over video.
  - Separate the episode-list control from the video information chip and make its list compact, opaque and focused on the current episode.
  - Anchor timeline frame previews above the hovered position, keeping them inside the player and preserving the frame's aspect ratio.
  - Give picture-menu actions consistent, descriptive icons, including local-file actions and video scaling.
  - Keep scaling selection indicators synchronized between the picture menu and player settings, with a distinct checkmark for the active choice.
  - Capture comparisons from the Qt Quick player's visible video frame without requiring an unrelated retained transition image.
  - Distinguish TensorRT engine compilation from cached-engine warm-up, display the elapsed time correctly, and wait for background preparation to finish before reporting a configured stage; never infer processing dimensions from an unobserved plan.
  - Fix keyboard activation in track selectors, picture-in-picture timeline previews and their dismissal, local file drops, icon alignment, and the completed preparation progress indicator.
  - Refresh the player speed, quality and scaling flyout with live appearance colors and corner radii.
  - Restore right-click access to the picture menu and enable the clip control when video becomes available.
  - Refine antialias settings controls, make episode buttons more compact and remove the redundant Episodes button from the title hero.
  - An earlier focused Windows production-player smoke passed 87 checks covering fullscreen auto-hide/wake, nested hover menus, click containment, fixture playback and close/reopen. The later QML-audit probe passed 157 requirements; its black-frame instrumentation was unavailable. Linux and broad workflow verification remain open.

### Safety, lifetime and playback correctness

- Contain archive extraction: both the `tar` and 7-Zip paths list entries before extracting and refuse `..`, absolute and drive-letter entries and symlink entries, instead of trusting the archive. Digest and size verification of the downloaded archive is unchanged.
- Allow only `http`, `https`, `file` and local paths for `--open-video`; other schemes are a usage error. `--frame-timing-log` validates that its parent directory exists and is writable before timing logging is enabled.
- Move the OpenSubtitles key out of plaintext settings into a store in the app-data folder — Windows DPAPI, a user-only-readable file elsewhere — with a one-time migration that deletes the legacy value. A `.env` file in the current working directory is no longer read.
- Redact credential-bearing URLs from every embed exception in the streams layer, and wait interruptibly during retry backoff instead of sleeping, so cancellation unwinds promptly.
- Range-check JSON numbers before conversion in the client, the models and native playback assembly; validate and percent-encode external-link identifiers. Out-of-range values are refused with one warning instead of being truncated by a cast.
- Guard watch history and the quality cache with one mutex per store, keep load+mutate+save a single critical section, warn once on a corrupt cache blob instead of discarding it silently, and trim deterministically oldest-first with millisecond timestamps.
- Own player sessions by value and look them up by stable id; observe dialogs weakly. Subtitle search and download workers now capture their shared slot and cancellation flag, removing a use-after-free and a compile-time capture defect.
- Cancel the Compare still writer through an atomic only, guard the native provisioner slot with a mutex, bound the runner's shutdown drain with a per-pool timeout instead of blocking forever, and drop GUI callbacks once `QCoreApplication` is gone rather than running them off the GUI thread.
- Degrade instead of aborting when an OpenGL context is lost during render-context release; delete the duplicate context resolvers and sixteen dead GL entry points; refuse `advancedControl` without an update callback instead of silently downgrading.
- Windows: inhibit display and system sleep while a surface is playing, and bound DXGI adapter enumeration with per-adapter failure reporting. Guessed shader-model inference is removed — an unmeasured SM stays unknown instead of being reported as a device capability.
- Drive end-of-file auto-advance from the player host.

### Interface

- Keep open player menus above the playback controls and switch enhancement submenus immediately on hover.
- Show the player's centre spinner while a source opens, so the wait for the first frame is a visible state instead of a bare black window.
- Keep the player's chrome on screen while the viewer reads a tooltip or rests the pointer on it, instead of auto-hiding the chip underneath its own explanation.
- Keep backend remarks out of the player's status chip: the "RTX builds the engine in-process, so `--skipInference` is not used" detail now lives in the chip's and the engine row's tooltip instead of widening a line read over the picture.
- Split the Components action bar into a selection row and an action row so the page fits the minimum dialog width in both languages.
- Promote the backend display for an RTX GPU only when nothing is stored; the stored explicit choice is never overwritten by the promotion.
- Fit Compare selector badges against the laid-out slot instead of the pre-layout button width, which had elided `A (Fast)` to `B · A (…` before the canvas was shown.
- Build the settings stylesheet from named `%{…}` markers instead of a positional 52-call `.arg()` chain, so adding or removing a value cannot shift every later one.
- Render the native cache from the snapshot delivered by the worker; enumeration and hashing no longer run on the GUI thread during refresh.

### Internal cleanup

- Delete the dead `MpvApi::module()` accessor, the unused `MpvHandle` synchronous setters, the unwired ClipSheet widget and its probe, and five render-only status reasons.
- Share one implementation of the mpv number helper, frame fitting, `theme::repolish`, `native::hashFile`, JSON reading, the MSVC native tool helpers and the Compare layout helpers; move the stylesheet into `SettingsUi.cpp` and the settings keys into `SettingsKeys.hpp`.
- Make the idle-inhibitor owner list unbounded, and add `TestWin32Support`, `TestCrashLog` and `TestJson` suites covering the registry and secret-store helpers, the crash-log contract, and the range-checked JSON accessors.

### Build and CI

- Fix the Linux lane's TLS gate: it now checks `libavformat`'s real `NEEDED` TLS dependency instead of the warning string every FFmpeg build contains, which had rejected every correct rebuild.
- Stage the toolchain MinGW runtime next to the Windows test child and prepend its bin directory to the child `PATH`, so tests and their grandchildren stop dying with `0xc0000139` against Git's runtime.
- Install the pinned Vulkan Runtime during Windows provisioning; `libmpv-2.dll` imports the loader, and the app refuses a module whose system driver is missing.
- Refuse the hover canary before the first paint when the context cannot be created or is older than mpv requires, instead of crashing in `QOpenGLWidget`'s first paint.
- Fix the base CI regressions: benchmark filter selection loss, settings page overflow, the stale `Save` probe contract, the component progress bar against a real closure record, and `IN_LIST` in `cmake -P` script mode.

The historical verification for these audit changes is recorded in [release status](docs/release-status.md#audit-remediation-consolidated--2026-11-11): `022c98c` compiled on Windows and produced review artifacts, but its individual audit changes were not exercised by the QtTest suites. Later focused checks and current blockers are recorded separately in the [application state](docs/release-status.md#application-state--2026-11-13).

## 0.2.1 — 2026-11-07

- Keep test fixtures in their disposable native store even when `MIKAI_NATIVE_ROOT` is set; direct test runs refuse a different or populated store.
- Reject component records and files redirected through symlinks or reparse points, and refuse RIFE graphs whose size changes during inspection.
- Interrupt stream-retry backoff promptly when work is cancelled; preserve an open player's pending quality request when changing releases.
- Keep AnimeJaNai preset data alive during preparation, propagate native stage-validation failures, validate unknown presets before inspecting the runtime, and restore the previous hardware decoder after an attached filter fails.
- Make the recursive mpv reply type compile with Clang and libstdc++ without changing its owned-copy semantics.
- Preserve media-information selections across normalized line breaks, re-fit Compare selector labels on resize, and translate previously unextracted native diagnostics.

- Make title details interactive: open the cover, copy titles, jump to episodes, and browse matching formats, statuses, studios, genres or tags.

- Highlight title-page external links with the selected accent glow on hover.

- Keep the engine-build window above competing topmost windows even when the application is inactive, without activating it.

- Default video scaling to native aspect ratio, including fullscreen; reset older scaling-mode preferences once so existing installations also start in Native.

- Rework title and episode selection: provider/release controls, bounded episode ranges and pages, protected scrolling, non-clipping actions, and provider-reported episode classifications shown with a color key.
- Bring title, release, provider, genre and player controls into line with shared app sizing, antialiasing and corner-radius rules; refine the title hero hierarchy.
- Add OpenSubtitles API-key settings and a registration link. Local subtitle files can be selected or dropped onto network streams; the drag overlay identifies subtitle, audio and video actions correctly. External audio remains local-video-only.
- Make the sidebar brand open the project’s GitHub page.
- Add pinned Anime4K shader assets and optional upscaler models to the component workflow and player processing choices; improve Compare frame handling for sources that reject a second decode.

## 0.2.0 — Rendering, RTX and MoonAnime update

- Update Standard AnimeJaNai to v0.22.2 and add opt-in TensorRT for RTX on Windows: verified dispatcher/runtime acquisition, in-process engine builds, separate engine/JIT caches, NVDEC, player/Compare/Benchmark selection and explicit refusal of backend substitution. Keep the classic Linux runtime; upstream publishes no Linux RTX binary.
- Show the Windows setup Welcome page and offer an explicit update or repair for registered installations, including builds sharing the same version number.
- Repair Linux AppImage staging: pass `DESTDIR` through CMake's environment launcher and accept both supported Qt platform-plugin layouts.

- Correct disabled-state detection for retained Linux desktop entries, and safely refresh or remove stale owned entries.
- Add opt-in presentation profiling and a cross-process focus/composition probe.
- Retire Runner work and dispatch references without destroying thread pools on their own workers.
- Preserve PQ/BT.2020 and BT.709 frame metadata in generated acceptance media.
- Show the installed component inventory as soon as Settings opens instead of waiting for a manual Refresh.

- Offer MoonAnime opening, ending and recap skips, with accent-tinted timeline ranges.
- Show frame previews and segment labels on timeline hover.
- Keep identical hover requests in flight instead of restarting their decode; ignore late replies after the current request finishes or is refused.
- Initialize video-scaling captions on the first Settings display and retain native combo-box arrows.
- Synchronize player chrome with native fullscreen/windowed state changes.
- Resize and right-anchor PiP clock labels when their text changes so the remaining time is not clipped.
- Update advertised API quota estimates to 299 requests/minute without a key and 599 with an API key; server quota headers remain authoritative.
- Add a responsive Home/Filters/Teams sidebar with matching Open video, Shortcuts and Settings rows at the bottom and an icon rail at compact widths; keep search and RPM in a single toolbar.
- Let Home headings scroll with the feeds, reserve a scrollbar gutter and page carousels by the visible card count without diverting vertical scrolling.
- Play MoonAnime again after the provider replaced its HLS master with a labelled list of direct per-quality media URLs: resolve those entries as the quality ladder, keep the legacy playlist shape working, and stop reporting a playable episode as having no source.
- Prefer MoonAnime when that provider is present; an explicit choice still wins, and a title without it keeps its first provider.
- Keep a presented picture across source changes and ordinary chrome repaints instead of clearing to black. A chrome-only repaint no longer consumes an armed presentation slot.
- Keep cached controls pixel-stable when neighboring controls hide at fractional display scales. Remove the experimental shared chrome sheets and retain the 8 MiB per-list cache budget.
- Render the interface at the display's own pixel grid: theme and player glyphs are painted vectors rasterised at the painter's device ratio instead of font characters or preloaded 1×/2×/3× bitmaps, the search field's **Enter** keycap paints its frame and return arrow in one antialiased pass and fills the caption as glyph outlines so it cannot pick up coloured subpixel fringes, artwork corners follow each surface's own radius with an antialiased alpha ramp, cached player chrome blits one texel per pixel within its 8 MiB budget, and rounded video and picture-in-picture corners are alpha coverage patches instead of a one-bit window mask. No global FXAA/TAA/MSAA and no change to raw frame capture, Compare's Nearest Neighbor filter or exported frames.
- Show "Preparing enhancement — playback paused" while an engine build holds playback, and "Finishing enhancement…" if closing that player takes more than a third of a second. The close wait is bounded at two seconds; if the native stop has not acknowledged by then, the close continues and the abandonment is logged.
- Add **Video scaling** after HDR playback: Fit, Fill, Stretch, 16:9, 4:3, original size, zoom, upscaler and downscaler. The first five modes are quick switches in the player settings flyout. PiP follows the display policy; Compare forces Fit while it owns the picture. Paused changes redraw after the asynchronous option writes complete. Windows framebuffer geometry and real Settings, live Fill/Fit, hover-preview and PiP screenshots are recorded; Linux pixel verification remains deferred.

## 0.1.0 — Player and usability update

The [update release](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0-update-20261101) covers the title-page redesign, player volume and seeking fixes, Compare exports, appearance settings, packaging and benchmark fixes, and AnimeJaNai diagnostics.

## 0.1.0 — First public build

The [first release](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0) introduced the Qt desktop client, libmpv player, optional processing, Compare workstation and Windows/Linux packages.

For implementation details, see the [architecture](docs/architecture.md), [player guide](docs/player-guide.md), [processing guide](docs/processing-guide.md) and [AnimeJaNai troubleshooting](docs/animejanai.md). Historical verification and open limitations are recorded in [release status](docs/release-status.md) and [redistribution evidence](docs/redistribution.md).
