# Changelog

Published changes are recorded with their downloads on the [GitHub releases page](https://github.com/smyhlin/Mikai-Ja-Nai/releases). Artifact checksums live with each release, not in the documentation.

## 0.2.2 — Unreleased

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
- Player and discovery enhancements:
  - Replace stock `QMessageBox` resume playback prompt with a custom themed dialog matching app design tokens.
  - Remove vertical scrollbar from discovery feed for clean full-width display; vertical scrolling supported via mouse wheel and explicit Alt + mouse wheel.
  - Group processing controls into Restore, Upscale and Interpolation; improve Anime4K shader selection.
  - Refine antialias settings controls, make episode buttons more compact and remove the redundant Episodes button from the title hero.

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
