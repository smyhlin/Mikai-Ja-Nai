# Release status

Published packages, release-specific changes and checksums are available on the [GitHub releases page](https://github.com/smyhlin/Mikai-Ja-Nai/releases). The original [v0.1.0](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0) was followed by the [player and usability update](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0-update-20261101). Local rebuilds produce their own `SHA256SUMS`; a rebuild alone does not require a documentation update.

### Post-v0.2.4.1 feature adoption & playback hardening (`#261`–`#280`) — 2026-11-25

Completed feature adoption and bug-fix passes (`#261`–`#280`) on `main` following the `v0.2.4.1` release:

- **Hikka Playback Continuity (`#262`–`#266`, `#273`, `#275`, `#277`):** Added pure deterministic `PlaybackSelection` (`shared link > favourite team > preferred provider > first available`, string-aware `"12.5"` / `"OVA"` episode ordering), per-title `FavouriteTeams` persistence (`title/favouriteTeams`) with `★ ` badge + first-position sorting + context menu toggle on `TitlePage`, versioned `mikai-janai://play` deep links (`PlaybackLink` copy/paste/CLI launch with episode & timecode resume), and extended `backend::externalLinks()` with AniDB, ANN, Wikipedia, and MangaUpdates.
- **Qt Quick Player UX, Scaling & Debanding (`#265`, `#267`–`#272`, `#274`, `#276`, `#278`–`#280`):** Restored native Qt vertical wheel scrolling on Homepage (`discoveryScroll_`) & Search Grid to match `TitlePage` (`#267`); added `0.5s` seek-in-progress gate, press-and-hold `2×` speed (with overlay-cancel guards), and `180ms` cumulative keyboard skip (`#265`, `#278`); added built-in mpv **Debanding** presets (`Off` / `Light` / `Medium` / `Strong` with HDR-safe `deband-grain=4`) in `VideoSession`, `SettingsGeneralTab`, and the live player `Q` menu (`#268`, `#274`, `#276`, `#280`); added **Non-Linear Stretch (NLS)** display scaling mode with auto-axis GLSL output hook shaders preserved across Anime4K switches (`#269`, `#272`, `#279`); bypassed low-gain (`< 1.15x`) RIFE interpolation (`#270`); and zeroed `hdr-peak-percentile` / `hdr-contrast-recovery` in `HdrToneMappingPolicy::Stable` (`#271`).
- **`FramePreviewService` (`po5/thumbfast`) Enhancements (`#261`):** Added software HDR `bt.2020` tone-mapping (`zscale` + `tonemap=hable`), album-art / still-image video track guard, and tighter `512 KiB` local demuxer readahead.

### v0.2.4.1 reliability, live Mikai API v1.5, and gallery verification — 2026-11-24

[Release v0.2.4.1](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.2.4.1) was tagged at `1dbc4a11`. Completed continuous bug-finding and hardening passes (`#191`–`#260`) across threading/lifetime safety, TensorRT shared-runtime management, live Mikai API v1.5 integration, local persistence, and documentation accuracy:

- **Cross-thread Qt lifetime & concurrency hardening (`#191`, `#247`–`#250`, `#257`–`#260`):** Eliminated off-GUI-thread `QPointer` destruction and worker-thread races across `SettingsBenchmarkTab`, `VideoRenderBridge::createRenderContext`, `CompareWindow::renderCandidate`, `CompareClipExport`, `CompareGifExport`, `MainWindow` update downloader, `PlayerController::WorkerSlot`, `ClipExport`, `VideoSurface::GuiSlot`, and `MpvVideoItem::GuiSlot` by replacing worker-shared `QPointer` instances with atomic raw pointers / alive tokens cleared on the GUI thread during teardown.
- **TensorRT / `aji-runtime` Linux fix (`#251`):** Removed the redundant `--per-copy` flag from `aji-runtime ensure` in `AnimeJanaiRuntimeStore.cpp` (since `appId(backend)` already embeds a 24-hex path hash) and accepted legacy `refs/<appId>-<8hex>.json` reference files so shared TensorRT runtime installation and verification succeed cleanly on Linux.
- **Live Mikai API v1.5 schema alignment & URL safety (`#252`–`#255`):** Fixed `SearchPage` query parameters (`sort=mal_rating`, `statuses=finished`, lowercase genre slugs), `SearchPageTeams` sort keys (`count`, `updated`), and `/posters/ua` nested `result`/`poster`/`anime.titles` parsing with client-side team filtering; added `encodePathSegment()` across all 36 REST path-concatenation endpoints in `MikaiClient.cpp`.
- **Atomic WatchHistory backup & retry (`#256`):** Switched `WatchHistory` `.bak` writes to atomic `QSaveFile` and re-queued failed `Item` flushes back into `pending_` on write failure.
- **Live 19-PNG documentation gallery (`docs/images/gallery-2026-11-24/`):** Rebuilt `ReleaseGalleryProbe` (OpenGL 3.3 CoreProfile, `QQuickWindow::setGraphicsApi(OpenGL)`, `Basic` style, `ensureMpvNumericLocale()`, live `search-results-grid.png` stage) and captured 19 live PNGs with `gallery-evidence.json` (`success: true`, `errors: []`, 36/36 live catalogue posters, 50 live Action search posters, 57.05% appearance preview delta, exact reset parity); audited all 13 docs and 164 relative links with 0 broken links.

Verification:
- **ASan + UBSan (`build/linux-asan`):** Full 79-test CTest completed with **0 AddressSanitizer and 0 UndefinedBehaviorSanitizer errors** (only environmental `TestVideoScaling` CUDA `cuInit` shadow-map conflict and locked-desktop `TestWindowRaise` focus check).
- **TSan (`build/linux-tsan`):** Focused 10-suite concurrency/async CTest reported **0 data races in Mikai-Ja-Nai source code** (remaining TSan warnings came solely from uninstrumented `libgcc_s.so.1` inside mpv LuaJIT threads, `libQt6Test` watchdog teardown, and GCC 16 `libtsan` thread-registry checks).
- **Release tests & translations (`build/linux-release`):** Passed `TestAnimeJanaiRuntime`, `TestApiClient`, `TestWatchHistory`, `TestQuickMenuModel`, `TestPlaybackControls`, `TestClipExport`, `TestCompareClip`, `TestCompareGif`, `TestMpvAsync`, and `mikai_translation_completeness`.

### v0.2.4 Linux release candidates & Categories/Search/Teams overhaul — 2026-11-22

Completed the 60-issue overhaul across Categories (#94–#113), Search (#114–#133), and Teams (#134–#153), followed by deep UI/UX audit and polish passes: `TitleCard` `360×140` grid sizing with `FlowScrollHost`, full-width Format pill bar, `Ctrl+K` search keycap, compact `26–28px` filter chips & `Clear all`/`Clear selected`, `RangePager` alignment, sidebar bottom button parity, `TitlePage` episode-grid overlap fix (`fitEpisodeSection()` + right-aligned `episodeJump` + `32px` `providerBar`), removal of duplicate bottom fullscreen button with missing `"compress"` icon in `PlayerControls.qml`, crisp `120px/notch` vertical wheel scrolling, and elimination of literal `(s)` plurals across the UI.

Verification: `qmllint` passed with 0 warnings on `PlayerControls.qml` and `PlayerView.qml`; QML icon audit confirmed 0 missing icon references across all 20 QML files; `packaging/ci/check_translations.py --catalog translations/mikai_uk.ts` passed; offscreen Qt geometry audit confirmed 0 widget overlaps and 0 squashed controls across `SearchPage` (1280×820 & 900×700) and `TitlePage` (15-episode layout). Fresh 0.2.4 captures added under `docs/images/gallery-2026-11-22/` (`catalogue-home.png`, `search-results-grid.png`, `title-page.png`). Linux release candidates (`mikai-0.2.4-linux-x86_64.tar.gz` and `mikai-0.2.4-x86_64.AppImage`) built and SHA-256 verified locally.

### v0.2.3.2 Linux release candidates & preview overhaul — 2026-11-21

Commit `6cae66c1` plus HLS preview fixes. Overhauled hover preview & Clip Cut Window preload pipeline (#43–#48): event-driven `mpv_set_wakeup_callback`/`mpv_wakeup` worker, lowest-resolution HLS variant routing (`previewSourceUrl`) + `hls-bitrate=min`, `start=<t>` cold open, `FastKeyframe` scrubbing + post-arrival exact refinement, `vd-lavc-skipframe=nonref`, `HoverPreviewImageProvider` 4-frame ring buffer + sync lookup, 450 ms post-first-frame `prewarm()`, `ClipController` shared cache + sequential forward filmstrip + off-GUI 228×128 scaling + single-pass tile retry + zero-seek `ClipSizeEstimate` + process-wide `ClipEncodingProfile` cache.

Verification: 4/4 CTest entries passed — `TestHoverPreview`, `TestCompareClip`, `TestClipExport`, `TestClipSizeEstimate`. New 18-image gallery added under `docs/images/gallery-2026-11-21/` (`catalogue.png`, `filters.png`, `title-page.png`, `player.png`, `player-hover-preview.png`, `player-settings-menu.png`, `player-processing-menu.png`, `player-pip.png`, `clip.png`, `compare.png`, `media-info.png`, `settings.png`, `settings-customization.png`, `settings-general.png`, `settings-shortcuts.png`, `settings-processing.png`).

### v0.2.3.1 hotfix published — 2026-11-21

[Release v0.2.3.1](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.2.3.1) was tagged at `1cae547`, the Linux artifact revision. The Linux AppImage and tarball were rebuilt from `1cae547`; the Windows application payload identifies `21bd1c9`. The Windows packaging commits `80409dc` and `1cae547` updated the system-DLL import allowlist and installer file-version handling without changing the application payload. All six release assets were downloaded and the five product files matched the published `SHA256SUMS`.

The release is unsigned, contains no Debian or Arch package, and was published at the user's direction while `publicReleaseReady` remains false. This publication does not clear third-party license, corresponding-source or redistribution obligations. Linux packages passed version, offscreen startup/runtime selection, shader-count and TLS-dependency checks; the Windows portable archive passed integrity checks and the installer passed an isolated Wine install/version/uninstall smoke. No full-suite, live playback/inference or native Windows clean-machine installation pass is claimed.

| Artifact | Bytes | Built from | SHA-256 |
|---|---:|---|---|
| `mikai-0.2.3.1-windows-x64-setup.exe` | 62,953,873 | `21bd1c9` | `9edd8a1474dc9ad241f6b9e918fe102fc0b2639b3bf7460f2a1d9884e1f2d245` |
| `mikai-0.2.3.1-windows-x64-portable.zip` | 84,806,254 | `21bd1c9` | `4ae1f2e52c10a7a61f16c93aa97b63b45deb8a41c3092dc6d6c8f6d1fad038ef` |
| `mikai-0.2.3.1-windows-x64-release.json` | 86,238 | `21bd1c9` | `cfc62beb09b5ac68c8867c010311f73c91ff67d33d73c0ad9a0d77ad0ecf6b7b` |
| `mikai-0.2.3.1-x86_64.AppImage` | 93,342,200 | `1cae547` | `074199c0140307af6997d0c6a7c2a00860c730982d9377fd1226da2e2d83f6c1` |
| `mikai-0.2.3.1-linux-x86_64.tar.gz` | 48,310,834 | `1cae547` | `12f814e0cdc9bb6acf30b4c31482660139e6f6f5c2e7c849323dbbbd98fd1ead` |

The 508-byte `SHA256SUMS` covers the five product files above; downloaded verification included the checksum file itself.

The v0.2.3 release was published on 2026-11-20 at
[GitHub](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.2.3), tagged at the newest
Linux artifact revision, `d9e7522`. The Linux AppImage and tarball report that revision;
the Windows receipt identifies `8b5cd5e`. The user-directed publication proceeded while
`publicReleaseReady` remains false; it does not clear third-party licensing, corresponding-source
or redistribution obligations.

### v0.2.3 published — 2026-11-20

The Linux artifacts were rebuilt from `d9e7522`; the existing Windows artifacts and receipt
identify `8b5cd5e`. All six release assets were downloaded from GitHub, and the five product
files matched the published `SHA256SUMS`. The release is unsigned, includes no Debian or Arch
package, and was published at the user's instruction despite the repository's blocked
redistribution-readiness status. No full-suite, playback or clean-machine installation pass is
claimed.

| Artifact | Bytes | Built from | SHA-256 |
|---|---:|---|---|
| `mikai-0.2.3-windows-x64-setup.exe` | 62,715,142 | `8b5cd5e` | `e96e097ff9c62325959510097a85058c6166980c7a6ee73ff52a7200c71b3510` |
| `mikai-0.2.3-windows-x64-portable.zip` | 84,661,337 | `8b5cd5e` | `7e76259a53c978853d676838d4c51639699b5e52b1b411e4f8780ae9cbfcfb40` |
| `mikai-0.2.3-windows-x64-release.json` | 88,985 | `8b5cd5e` | `7be13efbf3a65c85e292ffb108c69e52d7954bbfd39b76d217616e535cb2ec4d` |
| `mikai-0.2.3-x86_64.AppImage` | 93,297,144 | `d9e7522` | `1ff6a98979ef19390a78e05d7b1b8d7942738b74812d9272849aef57434bb1f6` |
| `mikai-0.2.3-linux-x86_64.tar.gz` | 48,253,778 | `d9e7522` | `cc1d1a8997342f579f0282a676c96b5ab8016bb0f852614132509295bf7c0010` |

## Application state — 2026-11-13

| Area | Current source state | Evidence or limitation |
|---|---|---|
| Catalogue and local state | Search, title/provider episodes, schedules and local watch history use the Widgets application and backend API client. | Upstream catalogue and providers control availability; no live catalogue/provider end-to-end run was performed for this status update. |
| Player | A Qt Quick `PlayerWindow` and typed `PlayerController` use one `VideoSession`/libmpv core. PiP retains that session; episode, audio/subtitle, settings and right-click menus are in the Quick scene. | The Windows production-player probe exercised a synthetic local video, controls and close/reopen; Linux desktop parity and real provider playback were not established. |
| Playback state | Watch history stores episode offsets; the Quick player queues a token-guarded resume offer. The AnimeJaNai reconfiguration path captures and restores the playhead and holds a picture during the transition. | The reported repeated resume prompt, black transition and playhead drift still need an end-to-end title-page/real-upscaler regression on this build, including paused and playing cases. Implementation is not proof that the report is resolved. |
| Compare and Clip | Compare captures a held frame and renders candidate sides; Clip opens a separate seekable-media editor. | The Windows synthetic-video probe opened Clip and verified both Compare pictures and playback resume on close. It did not verify clip export, every Compare candidate or native model inference in Compare. |
| Optional processing | Anime4K, Standard AnimeJaNai v0.27.0 / package 3.11.0 and prepared Native workflows are separate. TensorRT for RTX is Windows-only; runtimes and models are acquired separately. | The earlier RTX 5090 engine observations below belong to the previous runtime cohort, not an inference proof for the new pins. |
| Settings and storage | Settings cover appearance, processing, components and benchmarks. Native/model storage follows `MIKAI_NATIVE_ROOT` → writable executable-adjacent store → app-local data. Standard TensorRT additionally uses the upstream shared per-user store and Components relocation controls. | Relocation moves shared runtime packs, not models/engines. Windows loaded DLLs can block a move; close other AI apps and restart first. Existing user stores are never test fixtures. |
| Updates | The source checks GitHub for stable release notifications at startup or on demand. | Automatic installation and nightly channels remain listed in [TODO](../TODO.md); the notification does not download or install updates. |

### November mpv input update

The current pins are the Windows `2026-11-15-41868e1757-addons-43698509897` DEV archive
and Linux `2026-11-15-41868e1` with a local FFmpeg TLS rebuild. Both live libmpv runtimes
reported client API 2.6 and decoded the 320×180 HEVC fixture from a local file and a
certificate-verified local HTTPS server. Both ABI 1 scene detectors distinguished a
localized cut missed by the legacy threshold from an unchanged frame. Focused Windows
Qt regressions also passed asynchronous property/node replies, client API identity and
the numeric-C-locale child scenario; this was not a full-suite run.
The Linux rebuild used isolated Ubuntu 22.04 build inputs, preserving the baseline rather
than substituting host FFmpeg libraries.

These checks are not RIFE inference, native-model throughput, Qt fullscreen/VRR or new
installer evidence. The upstream G-SYNC note describes `mpv.exe`; it does not prove the
same behavior in the Qt/libmpv window. Exact input hashes and remaining source/license
obligations are in the [Windows](../packaging/provenance/windows-runtime.json) and
[Linux](../packaging/provenance/linux-runtime.json) provenance ledgers.

### AnimeJaNai 3.11.0 source integration

The current source uses v0.27.0 ABI-9 shims, the 3.11.0 RIFE graph cohort,
runtime-2 file inventories and aji-runtime v0.2.1. The five spatial graphs were
downloaded from the new release source and still match their existing pins.

Windows/MinGW and Linux/GCC development builds passed. Both `all_qmllint` targets
completed without QML warnings after repairing the Quick verification targets and
declaring target-derived module imports. Existing Linux `CrashLog.cpp` compiler
overflow warnings remain; this is not a warning-free build claim.

Ten focused checks passed on each platform: AnimeJaNai runtime/configuration,
native components, RIFE pins, component state, downloads, layering, mpv async,
native manifests, translation completeness and provenance. The focused Settings
retranslation case also passed on each platform with the enhanced-scene edit
preserved. The Ukrainian catalogue was refreshed and compiled with no unfinished
messages.

Actual Widgets surfaces passed 44 checks each on Windows and Linux/Xvfb:
moving and restoring a shared runtime while preserving its 16 MiB payload size,
refusing normal main-window close during relocation, and saving/reloading the
independent per-chain experimental scene flag. Screenshots and JSON are under
`.probe-out/upstream-3.11.0/ui-windows-fixed/` and `ui-linux-themed/`.
Separate executions of each official v0.2.1 CLI passed eleven adoption, repack,
reference, unsupported-GPU and relocation checks per platform.

Production acquisition and full hash verification preceded real 1080p GPU runs:
classic TensorRT on Windows and Linux/WSL, plus TensorRT-RTX on Windows. Each
serialized an engine from an empty owned cache, produced 2× frames and reused the
unchanged engine/build log in a fresh-process warm run with identical output.
See the [hardware ledger](../bench_results.md#2026-11-13--animejanai-3110-shared-runtime-verification).
These are video-ABI checks, not sustained throughput, end-to-end RIFE, bare-metal
Linux desktop, Qt frontend inference-state or fullscreen VRR proof.

The installer and portable receipts below predate this source integration.
No rebuilt distribution or clean-machine installation is claimed for these updates.

### Current Windows review build and scoped checks

The local `release-artifacts/` top level was rebuilt from `edfc45b-dirty` on 2026-11-13
(local time), using Qt 6.12 and MinGW. The receipt records
`edfc45b6a77ab72c155518979bf777dda116f527-dirty`; the executable extracted from the portable
ZIP reported `mikai 0.2.3 (edfc45b-dirty, built 2026-11-12T22:45:51Z)`. The dirty suffix
means the package cannot be identified by the commit alone. The package receipt's timestamp
is `2026-11-12T22:49:18Z`. Hashes were checked against `SHA256SUMS`; previous top-level
files were retained in a dated backup.

| Local review artifact | Bytes | SHA-256 |
|---|---:|---|
| `mikai-0.2.3-windows-x64-portable.zip` | 82,302,471 | `ae4a23cec53bcfba8d93ea605f2fd71ff82b414ac1b7f86238c2897b52c5583e` |
| `mikai-0.2.3-windows-x64-setup.exe` | 61,456,066 | `854416b50849ee7070fda5c34ac37dad96995f5fc0e591c447981f0b43e7a497` |
| `mikai-0.2.3-windows-x64-release.json` | 129,753 | `25ee1cb42dfbd23c6068f986e1691e83d9647956b8213ea608e04162ee89adce` |

The Windows `dialog_probe` at the QML-audit change passed **157/157** requirements on its
synthetic FFV1 fixture, across player open and reopen. It covered track-menu keyboard input,
PiP hover preview and dismissal, a mixed local/remote file drop, preparation progress,
scaling selection, Compare, clip-window invocation and menu input. Two black-frame
instrumentation checks were unavailable because the known-bright mode was not enabled.
All 18 maintained QML files passed a direct Qt 6.12 `qmllint` run with the complete generated
module metadata and zero warnings. These are focused development checks, not a clean-machine
package, installer, full-suite or Linux verification.

The installed Windows player's historical `mikai.log` and corresponding
`.rtxengine.build.log` identify the 4× Real-ESRGAN AnimeVideo v3 fp32 graph at
`1×3×1080×1920` on an RTX 5090. The builder reported **0.181218 s** engine generation and
**0.33 s** warm-up; the player then logged engine deserialization, a
`1920×1080 → 7680×4320` configured slot and CUDA-graph capture. These are observations of
that specific run, not an independent output-quality test or throughput benchmark. A
subsequent code change distinguishes cached-engine warm-up from compilation and does not
treat a pre-completion background slot as ready. A later
[isolated video-ABI hardware check](../bench_results.md#2026-11-13--tensorrt-rtx-cold-build-and-cache-verification)
at source revision `0d151d2` verified an empty-cache 1080p RTX build, cached reuse, separate
720p/1080p engines and GPU-readback luma. It bypassed mpv and the preparation popup, so it
does not establish the frontend state machine, playback quality or sustained throughput.

## What remains open

- Packages are unsigned. Linux builds also depend on compatible host graphics, audio, glibc and loader support; no package is universal.
- Third-party corresponding-source, license and redistribution obligations remain unresolved. Both platform profiles in `packaging/third-party.json` retain `publicReleaseReady: false`; see [redistribution evidence](redistribution.md). Publication does not establish rights clearance or a passed release gate.
- The native Quick player adds a Qt Declarative runtime/import closure (Qt Qml/Quick/QuickControls2, imported QML modules/styles and plugin dependencies). Development builds and focused checks above are not a current Linux package, clean-machine installation, desktop-parity or full release gate.
- Broad suite attempts were not green and did not complete: Windows reported Quick rendering/foreground/tint assertions, missing benchmark fixtures, provider-URL expectations and packaging-fixture failures; Linux hit the 180-second launcher-contract deadline before interruption. The release-related RIFE/installation fixture mismatches found in those attempts were corrected and the focused checks above passed. No full-suite pass is claimed.
- Standard AnimeJaNai provenance now records v0.27.0, package 3.11.0 and runtime-2 separately from historical private ABI-8 builds. Updated pins and local acquisition do not clear inference/model redistribution or GPL corresponding-source obligations.
- Wayland compositors control window stacking, including the AnimeJaNai engine-build overlay.
- A Windows 0.2.1 run reported `animejanai.build.pollRefused` shortly after a TensorRT background build started. The saved log does not establish why mpv refused the poll or whether the engine build itself failed; this remains unverified on hardware.

## Playback observation tracking

HDR dark-scene instability and periodic frame substitution (KB-29 and KB-30) remain under measurement. Rendering fixes and bounded tests do not establish that every black flash is resolved; see [known bugs](../known_bugs.md). A historical Linux stock-runtime hour run ended with black, frozen output; its cause was not established.

Pacing causality for "very fast, then very slow" playback remains under measurement (KB-30).
Chrome-only repaints no longer consume an armed presentation slot. `disarm` counts passes that
actually stop an armed timer; `lead_ms` is measured at the swap, including composition and the
vsync wait. The latest recorded measurements before the user stopped further probes are:

| Paused chrome-only capture | Actual DPR | Paint p50 | Paint p95 |
|---|---|---|---|
| Canonical historical before-side | Not recorded | 410 µs | 490 µs |
| Later Windows capture at `9bdd850` | 2.1875 | 2044 µs | 2388 µs |

These are not matched-DPR measurements and do not demonstrate a 30% reduction. The later capture
passed 24/24 load/context checks with zero black framebuffer samples; that is not desktop
composition proof. The Windows audit's full CTest run passed 69 of 70 entries; the 30-second
hover canary recorded 1,815 gestures and 726 new-frame paints but no paced-delay samples.
That historical pacing gate was incomplete; those observations are not a desktop-composition claim.

Later software-OpenGL CI canaries passed with a 50 ms mpv timing offset in a separate phase:
Windows recorded 32 paced samples at 31 ms p95, and Linux/Xvfb recorded 120 at 37 ms p95
(250 ms budget). The normal-offset phases advanced playback but had no paced samples. These
validate the timer handoff under that probe configuration, not normal-offset pacing or desktop
composition. The Linux run used the locally cached older mpv runtime.

The shared-sheet experiment (`659af02`, `99d7768`) was removed because hiding a neighboring
control changed unchanged pixels at fractional DPR and the sheets bypassed the cache's 8 MiB
budget. Per-control caches remain.

## Presentation baseline (Windows before-side)

The presentation path is instrumented for a recorded before/after baseline of
the R13/R14 symptoms (black flicker on in-window switches and during paused
playback, slow alt-tab). `MIKAI_UI_PROFILE=1` enables the counters and the
`[ui-profile]` stderr lines; `--frame-timing-log` now forces the same counters
and adds paint delay, skip reason, mpv `frame-drop-count` and
`vo-delayed-frame-count`, engine pause hold, pacing disarms and chrome dirty
requests to each paint line. The fullscreen stability probe gained
`--focus-switch` and `--baseline` modes; both are developer tooling, are run
through `mikai_test_launcher` in an owned disposable tree, and write JSONL rows
that name an unmeasured value `"unavailable"` rather than zero.

The hover, loadfile, geometry and chrome before-side values were recorded at `8deb325`.
That historical run reported `MEASURED`, with paused bright video and native
foreground validated throughout the requested hover interval.

Activation values use the subsequent complete readiness-barrier run at `463b2a3`:
every plain, processing-overlay and dock-hover cycle was valid,
occluded and observed, with no timeout, unsupported capture or foreground loss.
Both runs precede GOAL-B/D/H1 and all presentation product fixes. Earlier
incomplete/interfered runs remain preserved, not silently promoted to passes.

| Metric | Row / key | Value |
|---|---|---|
| Pacing disarms in a 30 s hover storm over paused video | `hoverStorm.pacingDisarms` | 0 |
| **Activation → first visible composited desktop frame** p95, plain (verified-occluded off-centre crop; timeouts censored as ≥ 2500 ms, not a paint/swap proxy) | `focusSwitchResult.phases[0].activationToVisibleCompositionP95LowerBoundMs` | 11 ms; 10 valid samples |
| Activation → visible composition p95, processing overlay | `focusSwitchResult.phases[1].activationToVisibleCompositionP95LowerBoundMs` | 4 ms; 5 valid samples |
| Activation → visible composition p95, dock hover | `focusSwitchResult.phases[2].activationToVisibleCompositionP95LowerBoundMs` | 4 ms; 5 valid samples |
| Activation-to-first-swap p95, plain (secondary diagnostic; `frameSwapped`, not physical scanout) | `focusSwitchResult.phases[0].activationToFirstSwapP95Ms` | 22 ms; 10 valid samples |
| Black samples across 20 `loadfile` transitions | `baselineResult.blackSamplesLoadfile20` | 0 / 463 samples; 20 transitions completed |
| Surface-geometry writes per normal→fullscreen round trip (legacy key says “transition”) | `baselineResult.surfaceGeometryWritesPerFullscreenTransition` | 2 per round trip = 1 per mode edge |
| `ChromeList::paint` mean per composition during the paused hover storm | `baselineResult.chromePaintMeanMicros` | 284.996 µs; 1850 paints |

A run whose `profileCountersEnabled` is false, whose
`baselineStart.preconditionsValid` is false (paused bright frame / native
foreground not verified), or whose `hoverStorm.valid` is false does not produce
these numbers: `baselineResult.result` reads `INVALID`/`INCOMPLETE`, the row
names `invalidReason`, and the probe exits `3` instead of silently passing.
A complete baseline requires `baselineResult.result == MEASURED`; a complete
focus run requires `focusSwitchResult.result == PASS`. Both require
`visibleCompositionComplete == true`. An `INCOMPLETE` timeout still supplies
failure evidence: its latency is censored at the 2500 ms deadline rather than
dropped, but it cannot establish the activation target passed.
`hoverStorm.pacingDisarms` is the validated interval only, not ticks made while another
application owned the foreground. Raw goal reports and capture files are local-only tracking
artifacts, not versioned release evidence.
The idle-GPU lavfi runs did not reproduce an eight-second activation delay or
a black load transition. The processing overlay is UI stress, not a real
TensorRT build. These observations do not prove behaviour during TensorRT builds,
physical scanout latency, or DirectFlip promotion; the latter metric remains
unsupported on this WGL path. Later presentation changes must retain this
before-side evidence rather than describing these zero-event observations as
a demonstrated fix.

## Recorded verification

### Audit remediation consolidated — 2026-11-11

The C++/DX audit remediation — task PRs #1–#14 plus follow-ups #17–#26 — is consolidated on
`cppfixes` at `022c98c`. Every branch and pull request that carried it has been merged or
recorded as superseded, and the remote holds only `main` and `cppfixes`.

Verified here:

- The merged tree compiles on Windows MinGW64 with Qt 6.12: `mikai`, `mikai_test_launcher`,
  `mikai_tests` and all 30 probe targets build and link, and `mikai --version` prints
  `mikai 0.2.2 (022c98c, built 2026-11-11T18:38:11Z)`.
- That build found one defect introduced by a merged branch, fixed in `022c98c`: the deferred
  X-D24 stylesheet rewrite dropped `QStringLiteral`'s closing parenthesis in `SettingsUi.cpp`,
  which made the file unparsable. The branch carrying it had never been compiled.
- The Windows packages were rebuilt from `022c98c` with `packaging/windows/package.ps1`:

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| `mikai-0.2.2-windows-x64-portable.zip` | 72,021,328 | `d40dce2ae1fdb7384d5037eea11b9bb7440d6206ba16f69e2fcda10827843f86` |
| `mikai-0.2.2-windows-x64-setup.exe` | 54,660,665 | `01493923e099784d0d4ba60bafd79946d095536060f7ef35c77009115c0b7cb6` |
| `mikai-0.2.2-windows-x64-release.json` | 84,529 | `61d5a2e9e747095f8d09d0dd0e876b6551c31802714474b463fa7bc2fd6523fa` |

- The packaged `VERSION` file, the release evidence and the binary agree on `022c98c`. The
  payload is 138 files, including `mpv/libmpv-2.dll` from the pinned
  `mpv-dev-x86_64-20261024-git-b4f9f5bfed.7z` (archive SHA-256
  `2efe77b9e3028c32cd74c9d871c5d2cd8315758d0da3f59bf64c867c8e7e6d0c`). The evidence file
  records `publicationStatus=blocked`.

Not verified here:

- No QtTest suite, probe, native inference or benchmark was executed for this tree. The audit
  branches were reviewed by reading and grep, and their CI runs never reached a green build, so
  the fixes recorded in the changelog are compile-checked changes, not reproduced-and-observed
  behaviour. Open symptoms are listed in [known bugs](../known_bugs.md).
- The Linux lane, the distribution packages and the sanitizer jobs were not run for this tree.
- An earlier packaging run on the same tree stamped the binary with the revision cached at the
  last configure (`9768155`) while its `VERSION` file named `022c98c`. `MIKAI_BUILD_REVISION` is
  resolved at configure time, so a commit made after the last configure is invisible to
  `cmake --build`; reconfigure before packaging. The artifacts listed above were rebuilt after
  reconfiguring and agree with themselves.

### 0.2.0 published — 2026-11-05

[Release v0.2.0](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.2.0) publishes
nine assets: the Windows setup, portable ZIP and build receipt; the Linux AppImage,
tarball, Arch package with its `.SRCINFO`, and Debian package; and one `SHA256SUMS`.
The Linux artifacts were built from `ae81f59`, the Windows ones from `1e94fc7`, and both
revisions are stated in the release notes. `main` and the annotated `v0.2.0` tag were
pushed at `ae81f59`. A downloaded `mikai-0.2.0-linux-x86_64.tar.gz` verified against the
published manifest.

This upload does not resolve the `publicReleaseReady: false` blockers in
`packaging/third-party.json`, does not carry the `packaging/ci/release.py` gate's
verification archives, and is not a passed release gate; it is a local build published at
the user's instruction. The Windows binaries predate the two Linux-found fixes
(`b05a90f`, `a7bed6c`).

### Linux release artifacts rebuilt — 2026-11-05

All four Linux packages were rebuilt at `6ee0c15` and promoted into
`release-artifacts/`; the Windows packages were left at their earlier `1e94fc7`
build. Each Linux artifact answers `--version` with the recorded revision and
resolves its bundled runtime at startup:

| Artifact | Size | SHA-256 | `--version` |
|---|---:|---|---|
| `mikai-0.2.0-linux-x86_64.tar.gz` | 45,528,993 | `d0262efc4b305483584ca20cd022a866f8b9a4f479500517851b3da6621f3da3` | `0.2.0 (6ee0c15, built 2026-11-05T17:25:49Z)` |
| `mikai-0.2.0-x86_64.AppImage` | 75,569,656 | `188b6b17350e0f38cc5a65c583ff7a935ebcee1b32c6b3c45c19d3253aa79c01` | `0.2.0 (6ee0c15, built 2026-11-05T17:28:17Z)` |
| `mikai-0.2.0-1-x86_64.pkg.tar.zst` | 43,435,772 | `b96e2ddfb6b648029af545d5d90155a30425fb7e57891762baef1e53577655b6` | `0.2.0 (6ee0c15, built 2026-11-05T17:24:36Z)` |
| `mikai_0.2.0_amd64.deb` | 66,565,436 | `3231f347e5f4d6c95491bebba11bb6c3cc4249ad63110d99c4830e5a7505ffd7` | `0.2.0 (6ee0c15, built 2026-11-05T17:25:50Z)` |

- The tarball, the AppImage and the package install trees each logged
  `player runtime standard stock-…` for the bundled runtime; the Arch entry was
  observed after `pacman -U` inside `archlinux:latest` with `qt6-base` and
  `libglvnd`, the Debian entry inside the `mikai-deb-builder:trixie` container.
  That is runtime-selection evidence, not playback, GPU or distribution-matrix
  verification.
- `packaging/arch/PKGBUILD` carried `pkgver=0.1.0` past the 0.2.0 bump, so
  `makepkg` looked for a source tarball the driver never exports. The version now
  comes from `project(mikai VERSION …)` through `@VERSION@` substitution.
- The Arch package was built with `--skip-check`; `makepkg`'s `check()` fails in
  this packaging layout. The suite rejects the bundled runtime twice: the test
  launcher stages the closure into a flat `mpv/` directory, while the package
  configures `MIKAI_LINUX_MPV_INSTALL_SUBDIR=lib/mikai/mpv` and the executable
  therefore looks in `lib/mikai/mpv`; and once a host `libmpv` has been loaded in
  the same process, the loader keeps the host's `libass.so.9` (0.17.5, which
  lacks `ass_set_composite_deferred`) instead of the staged closure. The installed
  package is unaffected, as the container run above shows. The test gate is
  recorded as not run, not as passed.
- The AppImage was staged against the Qt 6.12.0 `linux_gcc_64` SDK; the earlier
  build in this session used the host's Qt 6.12 and dragged host KDE icon and Qt
  Quick libraries into the image (166 MB), while the SDK build carries the leaner
  75 MB Qt deployment the previous artifact had.


### AnimeJaNai v0.22.2 implementation — 2026-11-05

Windows and Linux application builds succeeded for the v0.22.2 implementation. The Windows build `0d8ff35` was also used for the [manual UI gallery](images/README.md); the screenshots include an RTX backend selection with the runtime not installed. No RTX acquisition, engine build, native inference, benchmark or Linux visual parity is established by those checks. No test suites or probes were run for that implementation under the current verification instruction.

These source-tree changes postdate the published packages. A local application build or screenshot is not a rebuilt release artifact or evidence of redistribution clearance.

### Final Windows implementation checks — 2026-11-04

The final pre-commit application and translation build passed with four jobs in **16.85 s**:
`cmake --preset release`, then
`cmake --build build/release --target mikai release_translations --parallel 4`.
The launched application identifies the checked working tree as
`0.1.0 (e22b8c8-dirty, 2026-11-04T21:39:01Z)`. Implementation commits follow these checks;
the embedded revision is not a claim that the later documentation commits were compiled.
The Ukrainian catalog compilation reports **1938 finished translations, zero unfinished**.

Actual shipping-application screenshots record:

- The first-open Video scaling card with its captions, hints and native combo-box arrows.
- The API-key caption showing the supplied **299 anonymous / 599 keyed requests per minute**.
  These are fallback estimates; server quota headers take precedence.
- A live switch to Fill and back to Fit through the player flyout, with the initial Fit preference restored.
- A decoded timeline-hover thumbnail, PiP playback and return to the player, and windowed player chrome.
- The corrected PiP remaining-time label, resized and right-anchored after the displayed text changes.

Goal folders, raw reports and screenshots remain ignored, local-only files. Screenshots establish
the pictured UI states, not a five-minute focus/occlusion matrix, a numerical pacing improvement,
native inference or cross-platform parity. The screenshot clip was video-only because this Windows
session exposed no audio playback devices; no audio-output verification is claimed.

At the user's instruction, **no further probes or test suites were run** during these final
implementation checks. The full CTest result **66/66 in 247.87 s at `7e88871` is historical**,
not a test result for the final working tree. Earlier native hidden-framebuffer scaling checks
covered playing and paused changes and Compare Fit release; they do not substitute for Linux pixels.

An earlier cold active engine-build close at `28ebad9` returned in 12 ms and destroyed the player
in 121 ms. It did not exercise the delayed finishing notice or the two-second abandonment path.
The original long compositor matrix, matched-DPR paint-cost target and long native-stop paths
remained unverified at that point. Native Linux verification was deferred; the later Linux
application build above is not a visual or native-inference verification.

These observations apply to the identified runs, not automatically to later builds or every machine:

- The Windows Haruna review on 2026-10-31 passed 58/58 CTest entries and native playback-control checks. Two 70-second fullscreen/PiP runs reported no black or stalled samples, including one Anime4K Fast A run. No TensorRT inference was tested in those runs.
- A Windows portable rebuild from `95c4c8c` matched its payload inventory and answered `--version` with Windows system directories on `PATH`. Its setup was not independently extracted or installed for that rebuild; no upgrade, repair or clean-machine result is claimed.
- A preceding Windows portable build completed the 1080p baseline and Anime4K Fast A benchmarks. At the minimum Settings window size, the Benchmark page overflowed horizontally; at the default size, it did not.
- The Linux packages built from `2637edf` answered `--version` in their respective local package checks. This did not establish GPU inference or general distribution compatibility.
- At revision `f0d2122`, the recorded Linux CI job passed and the Windows CI job failed `DistContracts`. An earlier local Windows release-gate run passed 52/55 CTest entries; the aggregate suite, `TestMpvAsync` and `TestDiagnostics` failed. These are historical results, not current CI status. The release publication gate has no recorded passing run.

### Linux ASAN sanitizer notes — 2026-11-07

Linux ASAN/UBSAN builds under WSL detect small leaks (≈1 KiB) in the NVIDIA driver stack
(`libcuda.so`, `libnvidia-ml.so`) when `NativeGpuProbe` queries compute capability. These are
driver-originated and cannot be fixed in application code. UI timing tests (`TestFeedRender`,
`TestI18n`, `TestPublicApiUi`) also fail under ASAN due to instrumentation slowdown exceeding
`QTRY_VERIFY_WITH_TIMEOUT` limits. The Windows non-ASAN gate remains the release criterion;
Linux ASAN failures of this form are excluded from the release gate.

The original Windows v0.1.0 packages could fail at startup because of an incompatible C++ runtime. The player and usability update includes the source fix. See its release notes for the changes and download details.

## Further evidence and procedures

- [Build instructions](building.md) and [package/release procedures](releasing.md)
- [AnimeJaNai loader and engine-build troubleshooting](animejanai.md)
- [Benchmark results and workload limitations](../bench_results.md)
- [Screenshot provenance and capture procedure](images/README.md)
- [Source snapshot packaging](../packaging/source/README.md)
