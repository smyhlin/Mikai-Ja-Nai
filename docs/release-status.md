# Release preparation status

The selected delivery is **source-first preparation plus locally tested packages**. The supplied runtimes are retained; public binary publication is on hold until their downstream source/license requirements are fulfilled. The installer is unsigned. No tag, push, upload or public binary release has been performed.

## Original HDR remux follow-up (2026-10-26)

The supplied local Dungeons & Dragons remux was inspected without copying or
hashing the full 80,851,156,810-byte movie. It contains 3840×2160 HEVC Main10
at 24000/1001 fps, limited-range PQ/BT.2020 and Dolby Vision profile 7
(RPU, base layer and enhancement layer present), with 1,000-nit mastering
metadata, MaxCLL 1,191 and MaxFALL 340. This metadata does not certify full
Dolby Vision enhancement-layer playback.

Read-only Windows display queries reported an active 3840×2160/120 Hz,
HDR-capable display in **SDR mode**, HDR disabled and 8-bit RGB. No system
display setting was changed. These observations concern HDR-source-to-SDR
tone mapping, not native HDR presentation.

At the original film's 3,000-second dark scene, bundled and stock Windows
runtimes passed 28/28 and 27/27 HDR requirements respectively: verified seek,
stable paused source identity, actual RGB10_A2/10-bit render depth, 384/768
RGB channels off the 8-bit lattice, processing gates and zero GL errors.
The captures were not materially different at the authored-fixture threshold;
that is not a claim of exact pixel equality or a production defect.

The corrected 60-second bundled-runtime fullscreen pilot started at
1,800.382 seconds and passed 25/25 requirements. It used NVDEC `cuda[p010]`
and a muted WASAPI 48 kHz stereo float output with progressing audio PTS;
decoder/VO drops were both zero, maximum inter-swap gap was 104 ms, and six
valid content samples plus first/last framebuffer and composited-screen
PNGs were captured. The earlier pilot exposed a probe-only epoch mismatch
that discarded content samples; its failure is retained, not accepted as
playback evidence. Low-luma center observations are diagnostic only.

Bounded original-film dark-scene checks, the corrected fullscreen pilot and
an unlogged playback smoke passed. Separate authored-fixture regressions
retained strict policy-effect and barcode checks. Windows and Linux hermetic
suites each passed 54/54 (137.39 s and 1,000.18 s). The first requested
60-minute original-film fullscreen run, fork Auto, completed after 3,600.3 s
and failed 1/705 requirements: 0 decoder drops, 110 VO drops (over the
unchanged budget of 3), 118 content samples, 0 frozen hashes, 0 media errors,
and a 146 ms maximum inter-swap gap. Muted WASAPI audio PTS progressed;
actual source start was 1,800.382 s. It recorded 86,211 center observations,
including 4,489 low-luma samples. These are diagnostic observations, not
4,489 proven black flashes; natural footage has no authored barcode and the
run does not establish frame-order identity.

The initial 60-minute fork-Auto run's post-run temporal correlation found 109
of 110 VO-drop increments within 1,000 ms of a content-hash sample; the
remaining increment was 13.685 s away. This temporal correlation does not
establish cause. Probe instrumentation was changed to offload the unchanged
upright, full-resolution center 3/4 crop SHA-256 and full-frame vertical flip to one
`std::async` job. OpenGL readback remains on the GUI thread; immutable
epoch/segment/source-position/capture-time metadata accompanies each job, GUI
polling is nonblocking, and shutdown pauses playback and snapshots clock/drop
counters before joining. Pixel data, thresholds, and hash fidelity were not
changed.

The fork-Auto 10-minute async run failed 1/13 because audio was unavailable:
600.0 s elapsed, 20 hash samples, zero decoder/VO drops, frozen hashes or
media errors, and an 80 ms maximum inter-swap gap; mpv logged that no playback
devices were available. A replay of the preserved pre-async binary under the
same remote/no-audio conditions also failed 1/13 for audio, with 600.0 s,
20 samples, zero decoder/VO/frozen/media errors and an 81 ms maximum gap.
Thus this is not a controlled A/V or pixel-load A/B and the zero-drop results
do not establish that async hashing resolved the earlier 110-drop result.
Read-only observations changed between the earlier runs: waveOut devices went
from three to zero and the framebuffer from 3840×2160 to 2732×1950.
Later observations reported `SM_REMOTESESSION=1`; its earlier value was not
recorded. For the async run's 20 samples, GUI readback took
3.591–7.678 ms (median 5.787 ms), worker flip 7.183–12.761 ms (median
9.397 ms), and worker hashing 65.321–88.150 ms (median 69.150 ms). Three
bounded source-center reference windows decoded 19/96/24 frames; their coded
PQ centers were consistent with dark content, but do not establish exact SDR
output or whole-frame blackness.

The final-program fullscreen 60-minute matrix completed with all three runs
failing 2/13 requirements (drop budget and required audio):

| Runtime/policy | Elapsed | Decoder / VO drops | Content hashes | Frozen / media errors | Max inter-swap gap | Audio |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Fork Auto | 3,600.3 s | 0 / 64 | 119 | 0 / 0 | 778 ms | Required audio unavailable |
| Fork Stable | 3,600.3 s | 0 / 73 | 119 | 0 / 0 | 824 ms | Required audio unavailable |
| Stock Stable | 3,600.4 s | 0 / 13 | 119 | 0 / 0 | 422 ms | Required audio unavailable |

All were fullscreen natural-media runs starting near 1,800 s with no loop and
the unchanged maximum-three-drop budget. Audio was unavailable in every
run; read-only checks around the matrix reported `SM_REMOTESESSION=1`
and zero waveOut devices, not continuously sampled session state. The first two runs' captured frame dimensions
transitioned from 2732×1950 to 5464×3900 (fork Auto at 2,063,471 ms; fork
Stable at 1,157,792 ms); stock remained 2732×1950. VO-drop clusters around
those dimension transitions are consistent with a possible DPI/capture-load
confound, not proof of its cause. Only 4/64 Auto, 4/73 Stable-fork and 2/13
Stable-stock VO increments fell within one second of captures in this matrix;
the earlier 109/110 correlation was therefore not reproduced. The full
source remained 4K; hashes used full-resolution captured FBO pixels without
additional analysis downsampling. mpv still scaled the video to the Qt
viewport, so this is not native-4K-pixel output proof. These failed required-audio
and drop-budget runs are not playback passes or evidence that the reported
flicker is fixed. A bounded windowed 10-minute control was run to examine
the fullscreen sizing/load confound; it does not establish causation or clear
the fullscreen gates. The bounded fork-Stable windowed control used the same final
program: it ran 600.2 s from source start 3,000.122 s, sampled 20 hashes,
recorded zero decoder/VO drops, frozen hashes or media errors, and a 61 ms
maximum inter-swap gap. Its framebuffer remained 854×480. It still failed
1/13 because required audio was unavailable, so it is a diagnostic control,
not a causal result, an all-clear, or a replacement for the failed fullscreen
hours. Receipt summary: `evidence/windowed-control-summary.json`.

Evidence is retained under
`F:/ZKS/mikai-real-hdr-20261026/evidence` and its sibling `runs` directory;
the final matrix summary is `evidence/original-final-hour-summary.json` and
its program SHA-256 is
`5190cf564429d11797599771e973a528aa5daaf68e5c7dd270b970d8a54823f8`. No
60-minute original-file pass or physical-HDR/full-DV certification is claimed
for this follow-up. The initial read-only display query reported HDR-capable
hardware operating in SDR, HDR disabled and 8-bit output; the later remote
session's display/HDR state was not queried. A separate Linux natural-media
probe remains unresolved: a 9-second WSLg/Pulse run printed a failing verdict
after 9 seconds but did not exit; an owned-process timeout ended it. Native
GDB captured the same post-verdict lifecycle hang: main-thread teardown waited
in `mpv_render_context_free` via `VideoSurface::releaseRenderContext` and
`~VideoSurface`, while libmpv's core thread waited in its playloop/audio path
and the AO thread waited in PulseAudio. A render-free breakpoint was reached;
the terminate/destroy breakpoint was not. No hash job remained pending after
the verdict. The trace does not establish whether the underlying issue is the
WSLg Pulse server, endpoint or client wait, and no safe minimal app-owned
interrupt was identified. A separate private Pulse software-clock control
completed with progressing audio PTS but failed its unchanged drop budget
(102 VO drops); it does not prove the cause of the earlier hang or constitute
hardware playback evidence. Linux acceptance remains unresolved, not passed.

Fresh final-snapshot CTest runs passed 55/55 entries on Windows (282.51 s)
and Linux (942.06 s), including the combined blob and 48 individual C++ suite
entries. These hermetic passes do not override the failed original-film
playback gates. Logs and JUnit reports are retained as
`evidence/windows-final-ctest.*` and `evidence/linux-final-ctest.*`.
The natural-media readiness/capture adjustments
are probe-only. The shared
shader-staging correction makes one configuration-specific producer populate
the common shader output, avoiding competing post-build writers; it is a
build-staging race fix, not an application GPU/rendering fix. These edits are
later than the earlier source snapshot, which does not contain them. A new
source handoff target is
`F:/ZKS/mikai-real-hdr-source-20261026/mikai-0.1.0-source.tar.gz`, with
external `SHA256SUMS` and `source-provenance.json` sidecars. Those sidecars,
not a self-referential hash in this document, identify the generated snapshot
and the bytes archived. It is a dirty-worktree snapshot, not commit-equivalent
or approval to publish binary packages.

## Historical baseline (2026-10-24)

The following records and package artifacts are the October 24 baseline. They
must not be read as verification of the HDR/frame-timing changes made on
October 26 or as current package builds.

| Check | Observed result | Scope |
| --- | --- | --- |
| Windows build and CTest | 5/5 CTest groups passed | Launcher, distribution, native staging, hermetic C++ suites and real bundle CLI contracts |
| Linux build and CTest | 5/5 CTest groups passed | Launcher, package, native staging, hermetic C++ suites and real bundle CLI contracts |
| Source packaging contracts | 5 passed | Public-file selection, privacy and hashes of actual archived bytes |
| CI/release Python contracts | 11 passed, 1 skipped | Local trusted-tag, ancestry and package/source behavior; Windows symlink creation was unavailable |
| Windows installer | Install, upgrade and uninstall passed | Current-user account; all 132 manifest-listed payload files, Unicode shortcut metadata, data sentinels and monitored integration values |
| Windows OS integration | Registry sandbox suite passed | Real WinAPI operations in GUID-owned HKCU hives; unsafe Default Apps launch is intercepted only in the test executable |
| Windows playback lifecycle | 0/114 requirements failed | Real NVIDIA OpenGL rendering, deferred pause, context recreation, stopped-source and refusal/recovery paths |
| Linux selected-bundle renderer | 0/114 requirements failed | Xvfb/llvmpipe; exact published Standard module and private dependency identities; fractional-DPR instrumentation unavailable |
| Linux runtime recovery | Passed | Exact named published bundle, media clock and nonblack framebuffer; no host-SONAME substitution |
| System-mpv configuration | Configure and product build passed | Separate Linux `MIKAI_LINUX_BUNDLE_MPV=OFF` path; not a hardware-playback claim |
| Real application gallery | Passed | Five core surfaces plus three appearance captures; anonymous live catalogue, Original playback, non-AI comparison and clean worker shutdown |
| Ukrainian catalogue | 1,738 finished, 0 unfinished | Refreshed and compiled with Qt 6.12 |

A 1,200.006-second same-context steady run recorded 30,135 GL compositions, zero black compositions, zero playback stalls, no owner/context/source-epoch changes and a maximum GUI timer gap of 764 ms. Its desktop samples were interrupted by other foreground windows, so this is **renderer/context-aging evidence, not a passing continuous visible-fullscreen certification**. A separate 75-second transition run passed with three advancing PiP-owner samples, zero black desktop/FBO samples and zero stalls. The strict visible-window pilot passed for 35 seconds; longer visible runs failed their desktop validity conditions when Explorer or the terminal host took over. A probe-only topmost experiment also failed validity and was removed. Those failed observations remain recorded, not reclassified as passes. A controlled uninterrupted desktop session is still needed to close that narrower visual-certification gap.

No GPU inference, model-quality result, RIFE throughput or universal codec/driver compatibility is claimed. Linux evidence uses software graphics, not a physical Linux NVIDIA display. Windows integration and installer checks do not certify another user, a fresh Windows account or machine-wide registration.

## Current source, UI and runtime verification (2026-10-26)
The records in this section describe the earlier depth+Stable-`clip` source
and its package snapshot; subsequent natural-media probe instrumentation
changes are not covered by its build, test or package results. Fresh
Windows/Linux hermetic-suite runs for the final documentation/source snapshot
are pending; the previous 54/54 results remain valid only for their recorded
source. The pre-depth 60-minute table below is historical and separate from
the final-source matrix. Current
depth+Stable-`clip` source builds passed all targets on Windows (37.83 s) and
Linux (82.48 s), including all five render callers and headless target.
Windows current CTest passed 54/54 in 147.79 s
(`windows-ctest-gamut-final.log` and paired JUnit); Linux current CTest passed
54/54 in 1,102.19 s (`evidence/linux/ctest-gamut-final.log` and `.xml`).
Current HDR FBO10 gates passed on Windows fork/stock and Linux fork; Linux
stock's short default-8 pilot passed counters but remained striped. The final
depth+`clip` 60-minute matrix completed with three PASS results and one
Linux-stock failure: its output was black and identity froze despite
progressing source/audio clocks. Full per-runtime metrics and limits follow
below; the failure is unresolved and has no assigned cause. Final private
packages have been produced; the Linux final archive passed relocated
negative CLI and positive local-player smokes. Visual review confirmed clear,
advancing video in both playing captures. This is packaged
Xvfb/software-GL/PulseAudio-null-sink evidence, not physical HDR/audio output.
Stopped pre-`clip` runs are not acceptance. The older source handoff target
and its sidecars below do not include the latest natural-media probe edits.
The newer source archive remains planned, not created or verified.

### Builds and application checks

| Area | Verified result | Evidence / scope |
| --- | --- | --- |
| Windows | Pre-depth CTest 54/54 (152.81 s); depth-only/pre-clip CTest 54/54 (126.65 s); current depth+`clip` CTest 54/54 (147.79 s). | `windows-ctest-caption-final.log`, `windows-ctest-depth-final.log`, `windows-ctest-gamut-final.log` and paired JUnit files. |
| Linux | Pre-depth revision completed in 14 Ninja steps and CTest 54/54 (813.53 s); current depth+clip build completed in 82.48 s and CTest passed 54/54 (1,102.19 s). | Current CTest: `evidence/linux/ctest-gamut-final.log` and `.xml`; `evidence/linux/build-final-ui.log` and `evidence/linux/ctest-final-ui.log` are pre-depth. |
| Depth-only Windows build | All targets built in 81.09 s; CTest 54/54 (126.65 s) before Stable-gamut correction. | `windows-ctest-depth-final.log` and paired JUnit are pre-clip. |
| Current depth+clip builds | Windows all targets built in 37.83 s; Linux all targets built in 82.48 s, including the five render callers and headless target. | Windows CTest passed 54/54; Linux CTest passed 54/54 (1,102.19 s). Current HDR probes passed; final-source timing matrix completed with 3 PASS and the unresolved Linux-stock failure detailed below. |
| Translations / UI | 1,759 finished messages, zero unfinished. Pre-depth UI07 passed 34/34 and all English/Ukrainian native-font/catalog captures at 580×820 and 940×820 were visually reviewed. The depth-only Windows UI run passed 34/34 in 3.143 s; app SHA-256 `74732f9fe46e1dad4569f974c8a9128001768405f1eb552ffa91f039ce2182d1`. | Controls/captions/risk text fit without horizontal scroll in reviewed captures. Current Windows CTest also passed 54/54 after the Stable `clip` correction; separate native-font UI captures remain from the previously reviewed revision. Reproduction command is in `tests/fixtures/README.md`, requiring `QT_QPA_PLATFORM=windows` and the built catalog, not fontless offscreen QPA. |
| Packaging / CI checks | Source packaging contracts rerun 5/5 on Windows (1.756 s) and Linux (0.440 s); Linux CI/release Python checks 14/14; Windows 13 pass, one symlink-privilege skip; all final workflow YAML parsed. | Contract evidence: `evidence/source-contracts-final.json`; workflow evidence: `evidence/workflow-yaml-final.json`; hosted CI was not executed. These checks do not imply public binary rights, installer acceptance or distribution. |
| Sanitizer configuration | Linux ASan/UBSan preset configured with `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all`. | Configure only, not an instrumented-suite pass. TSan runtime probe exited 66 with `FATAL: unexpected memory mapping`; no TSan application matrix claim. |
| Headless pixel-probe smoke | Initial probe-only run selected Qt software Mesa GL 3.0; the process exited 9 with `mpv_render_context_create: not supported` (the raw negative mpv error code was not recorded). After diagnostic-only `AA_UseDesktopOpenGL` before `QGuiApplication`, Windows GL 4.6/NVIDIA 616.92 decoded a movie with 72 frame updates, zero render/GL errors and 57,600/57,600 nonblack pixels; PASS in 8.483 s. Linux diagnostic PASS in 5.781 s. | Windows program SHA-256 `affbe93ab06a76b30cc4dca1eae3ba8683d37bc62719027fa59c899368fa19d0`, receipt `runs/fork-headless-desktop-after`. Linux program SHA-256 `3371baf3a62f24c369d141be4dece82a7a39297c83d526693d51828444a6e1b1`, receipt `evidence/linux/fork-headless-desktop-final/receipt.json`. Probe-only correction; no production change. |

### HDR/runtime gates

| Runtime / gate | Result | Limit |
| --- | --- | --- |
| Windows NVIDIA / `hdr_playback_probe --fbo10` | Current depth+`clip` fork and stock runs passed 27/27 requirements, actual `RGB10_A2` (`0x8059`), 10-bit R/G/B and render depth, 346/768 and 370/768 RGB channels off the 8-bit lattice, zero GL errors and no unsupported/fallback warning. App SHA-256 `743cc59d087df8c1bc4efb0c8fbef8d6e95e9fbeb70d8fc11ecf41be847bb66c`. | Probe-specific target, not proof the normal player surface has a 10-bit attachment or that output reaches a physical HDR display. The FBO `internal_format` and render-depth parameter are distinct. No physical HDR or 30-minute luma-variance certification. |
| Linux depth+`clip` fork HDR probe | Passed 29/29 requirements (30.535 s); actual `RGB10_A2` target, 10-bit RGB/depth, 385/768 RGB channels off the 8-bit lattice and zero GL errors. App SHA-256 `5359e7cfd80eeedb6f5c0bed3bd787d759bc5503fe6087c7ea7324c61c1c8f5b`. | Probe-specific, software-OpenGL/Xvfb target; not physical GPU/display output or the normal player widget's target. |
| Linux depth-contract control | An owned control used the same new layout/metadata and real RGB10_A2 attachment, with the target/channel and cached/report depth still 10; only the `MPV_RENDER_PARAM_DEPTH` row was deliberately omitted. With the parameter absent, the renderer implicitly assumed 8-bit depth: 0/768 RGB channels were off the 8-bit lattice, versus 385/768 in the restored current run. It failed only that check; the other 27/28 passed (zero GL errors). | Controlled omission of one render parameter, not an earlier-revision capture and not a diagnosis of old-stock visual corruption. Evidence: `evidence/linux/fork-fbo10-depth-omitted-before/run.log`. |
| Linux bundled fork | HDR probe passed 19 s; AAC-required pilot passed with p010/yuv10, PulseAudio null sink, strict identity `d1b87ddd80f314a80fc66d2d8f626eb26027e45b8bfaeba22c4197a16ae357ce`. | Software OpenGL/null sink; not physical GPU/display/DAC evidence. |
| Legacy Linux stock mpv 0.37/API 2.2 | `loadfile` compatibility, HDR gates, AAC at 48 kHz and progressing audio PTS passed in the exercised pilot. A current default-8 pilot passed its counters in 30.2 s (0 VO drops, 5 moving hashes, 649 barcode IDs) but its composite still showed vertical stripes. | Null-sink/software-OpenGL evidence; stock visual fidelity remains unresolved. |
| Windows audio | No WASAPI playback devices; AAC was disabled in Windows runtime/aging checks. | Windows runs were video-only; no physical audio claim. |
| Windows rollback / fractional-DPR | Final-code noise-HDR pacing rollback on default FBO8 exited 0 in 12.285 s without GLSL failure; rollback `--fbo10` exited 0 in 13.094 s; fractional-DPR 1.25 lifecycle had 0/114 failed (23.695 s). | `runs/fork-hdr-rollback-gamut-final`, `runs/fork-lifecycle-depth-gamut-final`; probe evidence, not physical HDR output. |

### Final-source 60-minute paint-stage matrix

Authoritative receipt: `F:/ZKS/mikai-hdr-pacing-acceptance-20261026/evidence/final-source-hour-matrix.json`.
The same continuous 3,660-second no-loop source and ordinary three-VO-drop
budget were used. A content-sample count is not the count of advanced unique
frames; `advanced` is reported separately. Every run exercised immediate
pacing only, with zero deferred operations, so these hours do not prove
future-deadline Qt timer waiting or causally explain the original drift report.

| Platform/runtime | Duration | Decoder / VO drops | Barcode readbacks | Content samples (advanced) | Max inter-swap gap | Audio / requirements | Result |
| --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| Windows fork | 3,600.4 s | 0 / 0 | 86,389 | 119 (118) | 112 ms | Video-only; 0/10 failed | PASS |
| Windows stock | 3,600.4 s | 0 / 1 | 86,388 | 119 (118) | 113 ms | Video-only; 0/10 failed | PASS |
| Linux fork | 3,600.1 s | 0 / 3 | 86,384 | 119 (118) | 132 ms | AAC PTS progressing, PulseAudio null sink; max skew 0.148 s; 0/667 failed | PASS |
| Linux stock 0.37 | 3,600.2 s | 0 / 3 | 86,383 | 110 (0) | 178 ms | AAC/source clocks progressed, max skew 0.055 s; 109 frozen-hash failures; 903/1,581 failed | FAIL |

The Linux stock run's first, final and composited images were black; identity
0 remained frozen from source time 4.417 s while source/audio clocks
progressed. No new 20-minute transition was observed, and no cause is
assigned. This is an unresolved visual/identity failure, not a passing
counter-only soak. The three passing runs' final-source program SHA-256 is
Windows `082aca12c4ec98b822a68b3b3e3ca5796271ddb28130afd7c2df2a6d5aa49110`
and Linux `a776f3f635bdaa3daaa6af67d2b77a5c146be2af62bd7cd7914d51a89e0b26ed`.
Pacing operations were immediate only: Windows fork/stock 86,396/86,395;
Linux fork/stock 86,384/86,385. The Linux scope is Xvfb/llvmpipe with a
PulseAudio null sink; Windows was video-only. No physical GPU, display,
audio-clock, or uninterrupted visible-desktop certification is implied.

### Recorded 60-minute paint-stage runs (pre-depth revision)

All four runs passed the stated timing/identity acceptance on the continuous 3,660-second no-loop source: ordinary VO-drop budget 3 unchanged, zero decoder drops and zero frozen-frame, identity-order or media failures.

| Platform/runtime | Duration | VO drops | Barcode IDs | Moving hashes | Max inter-swap gap | Audio |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Windows fork | 3,600.1 s | 1 | 86,376 | 118 | 95 ms | Video-only |
| Windows stock | 3,600.0 s | 3 | 86,374 | 119 | 118 ms | Video-only |
| Linux fork | 3,600.4 s | 0 | 86,395 | 119 | 66 ms | AAC/PulseAudio null sink; final A/V skew 0.031/0.032 s |
| Linux stock | 3,600.2 s | 1 | 86,390 | 119 | 75 ms | AAC/PulseAudio null sink; final A/V skew 0.034 s |

Windows pacing-path counts: fork 86,385 immediate / 0 deferred; stock 86,382 immediate / 0 deferred (15.84 MB stock timing log), `evidence/windows-pacing-path-counts.json`. Deadlines were already elapsed, so these runs exercised immediate rendering—not future-deadline Qt timer waiting. Clock-unit normalization/raw-nanosecond comparisons were exercised separately; pure 32 ms future-deadline helper tests cover the deferred contract. The soak does not establish a causal explanation for the original drift report.

Pre-depth-revision application SHA-256: Windows `b92ebc3188f122768fcce71cb5e8ef3fddac5e251f03bc8d48401676a0e323d8`; Linux `2fc1415e52f591ec86e8a8e49fd290611ff7e0a05e0414d83891a47879dab3c5`. Linux run evidence: `evidence/linux/fork-paint-60m-final`, `evidence/linux/stock-paint-60m-final`; Windows records: `fork-paint-60m-final`, `stock-paint-60m-final`. Samples were cached-FBO reads at paint stage correlated with `frameSwapped`; first/last cached-FBO and Linux composited images were saved after stopping. They do not certify uninterrupted unobscured desktop operation or physical output.

### Stock Linux visual-compatibility gate remains unresolved

Earlier pre-depth stock Linux cached-FBO/composited images contained full-height thin black vertical stripes; the fork composite from that revision was clean. In the current final-source hour matrix, stock's first/final/composited images are black and the fork's final composite is clear and advancing, as recorded above. Standalone stock mpv 0.37 NVDEC-p010 and CPU-yuv10 raw/windowed captures were clean. A separate 30-second in-process stock-app EGL/no-log run met short-run counters (0 decoder/VO drops, 6 advancing hashes, 696 barcode IDs, 723 ms max gap, 0.010 s A/V skew) but remained visually striped. A stock mpv 0.37 `mpv_surface_smoke` 10-bit-target run failed 15/113 requirements (1 skipped, 1 instrumentation unavailable): during playback the image was near-black with no measured motion; parked/resized/reparented views were black; RGB24 `lavfi` recovery was lit and moving. Evidence: `evidence/linux/stock-render-lifecycle-10bit/run.log`; app SHA-256 `03a767355dbd6f407769f2fe1aef0773ba5b1c1ed488ecff5ad0d84ebcb40c20`; runtime SHA-256 `f2aa472878adee5f44419b6cce90ccecc43bf81595ac16dbf223de190cdead0a`. Earlier minimal Qt/libmpv controls are not attribution evidence: one omitted the API-required current-context guard before `report_swap`, and later scratch controls have unresolved render-thread property-read contracts. Production swap callers already have the guard; no production regression or fix is established there. The observed old-stock WSL/Mesa visual behavior remains unresolved and is not assigned to new pacing. No stock visual-quality pass, root cause, physical-output proof, source-matched A/B, or production EGL/core-profile/hardware-decode workaround is claimed. Earlier failed old-sampler soak evidence and `c13` remain in KB-30; the drop budget was not widened.

The 3-second SIGSTOP negative test failed at a measured 3.041-second output/source-progress gap; its 1,000-drop allowance applied only to that negative test. Ordinary timing acceptance retains the 3-drop budget.

The final private package artifacts and their checksums are recorded below;
the Linux relocated positive player smoke was visually reviewed and passed as
scoped below. The final source handoff target is
`F:/ZKS/mikai-0.1.0-hdr-fixes-source-20261026/mikai-0.1.0-source.tar.gz`,
a local dirty-worktree snapshot based on `66a8229`, not a commit-equivalent
archive. Its external sidecars are
`F:/ZKS/mikai-0.1.0-hdr-fixes-source-20261026/SHA256SUMS` and
`F:/ZKS/mikai-0.1.0-hdr-fixes-source-20261026/source-provenance.json`; the
archive/tree hash must not be embedded in archived documentation.

## Final private QA packages (2026-10-26)

The final Windows and Linux package artifacts have been produced from the
current frozen source. They remain private review artifacts, not public
downloads or redistribution approval. Windows portable payload members match
the release manifest/current build (133 members); the Windows setup is
unsigned and has not been freshly installed. Linux's producer package
contract passed. Linux final-payload smoke completed: the relocated app's
three actual CLI negative cases exited 2 with SDK/plugin search paths removed
and system PATH only, and a local-player smoke opened continuous 10-bit SDR
video plus AAC with the pinned fork for 13.364 s and exited gracefully (no
timing log). Visual review of
`evidence/linux/packaged-positive-final/playing-first.png` and
`playing-later.png` confirmed clear, visibly advancing test-pattern video
with color bars and barcode, no black/striped image, and no optional-component
modal. This is a packaged Xvfb/software-GL/PulseAudio-null-sink playback pass,
not physical HDR/audio evidence. Both platforms retain
`profiles.publicReleaseReady=false`.

### Main working-tree `dist-mikai` deployment

The current app was copied to `dist-mikai/mikai.exe` (SHA-256
`55e1b9bf2adad3f8a051f575a5be13e69abc0a2347963d8f6c93059e552bc06f`), and
the Ukrainian catalog to `dist-mikai/translations/mikai_uk.qm` (270,000
bytes; SHA-256
`b67b34f15f4163777c160ba92b2940b8339dd1785e5a1f32c84f4149a591b7d6`; 1,759
finished, zero unfinished messages). `VERSION` records the uncommitted
worktree state without claiming commit equivalence. All 24 Qt/MinGW/mpv
runtime dependency hashes matched the final verified portable package; no
reparse-point ancestors were accepted. Previous app/catalog/version files
were backed up outside the repository at
`F:/ZKS/mikai-hdr-pacing-acceptance-20261026/dist-backup-before-hdr-pacing/`.
The user `dist` app was not launched and no native store was mutated. Evidence:
`evidence/dist-runtime-compatibility.json` and
`evidence/dist-deployment-final.json`. This local deployment is not a public
package or release approval.

| Artifact | Location | Size (bytes) | SHA-256 |
| --- | --- | ---: | --- |
| Windows portable ZIP | `release-artifacts/hdr-pacing-20261026-final/windows/mikai-0.1.0-windows-x64-portable.zip` | 70,659,029 | `a17d5de7239b11edd349a710e537b0d27fe853d5c21e6b5eea9d2b4fbc1bd62b` |
| Windows per-user setup | `release-artifacts/hdr-pacing-20261026-final/windows/mikai-0.1.0-windows-x64-setup.exe` | 53,446,008 | `aca21b3e2f196178330b410a8ac91691d1e54afe49765a8dc4501715c3225822` |
| Linux archive | `release-artifacts/hdr-pacing-20261026-final/linux/mikai-0.1.0-linux-x86_64.tar.gz` | 74,570,720 | `31e34dd07151430090c0758aadef9d70239dfaa4091e6d01c7d5d4ba94bd0242` |

Windows packaged app SHA-256: `55e1b9bf2adad3f8a051f575a5be13e69abc0a2347963d8f6c93059e552bc06f`.
Linux packaged app SHA-256: `fe0defbaa7713c3b1810ac503900cbbeaee3d8da5c9c916866201c68229b285b`.
Windows final portable's three actual CLI negative cases exited 2; evidence:
`evidence/windows-packaged-bootstrap-final.json`. Linux final archive
bootstrap evidence is `evidence/linux/packaged-bootstrap-final.json`;
positive local-player receipt is
`evidence/linux/packaged-positive-final/receipt.json`.
Windows final-code HDR rollback `--fbo10` passed (13.094 s), and fractional-DPR
1.25 render lifecycle passed (23.695 s); receipts:
`runs/fork-hdr-rollback-gamut-final` and
`runs/fork-lifecycle-depth-gamut-final`. These do not certify physical HDR output or installer behavior. The Linux
positive local-player process smoke was visually reviewed; its Xvfb/software-
GL/PulseAudio-null-sink evidence is not physical output.

## Superseded intermediate local package artifacts (2026-10-26)

These superseded private QA artifacts are retained as historical evidence,
not current packages. The Windows artifacts predate the Settings checkbox
label fix; the Linux package has a separate prior relocated-bootstrap smoke
whose scope does not transfer to the final archive. The public binary-rights
hold is unchanged.

| Artifact | Location | SHA-256 |
| --- | --- | --- |
| Windows portable ZIP | `release-artifacts/hdr-pacing-20261026/windows/mikai-0.1.0-windows-x64-portable.zip` | `2540e7b06f9db64d33afefda44ab24a0ab94722c1fb1830f2ab84ff2e21e893d` |
| Windows per-user setup | `release-artifacts/hdr-pacing-20261026/windows/mikai-0.1.0-windows-x64-setup.exe` | `234488078cec111adadbae4bf9fd5070ce8d40c163feda66073889516c2b8eae` |
| Linux intermediate archive | `release-artifacts/hdr-pacing-20261026/linux/mikai-0.1.0-linux-x86_64.tar.gz` | `17e8761955f4798acbb52f83e95fbf988417b723277e3d944cefdc3102052e10` |

Linux packaged application SHA-256: `42703a1b26daa717ad52445fd0dc661da3037248edc25380557b510fbfc5447b`.

## Historical local package files (2026-10-24)

All paths below are local, ignored build outputs. They are not public download links.

| Artifact | Location | SHA-256 |
| --- | --- | --- |
| Windows portable ZIP | `release-artifacts/windows-verified/mikai-0.1.0-windows-x64-portable.zip` | `84bab8fcb09321c3521e19409f6de2531991e2ae4dfdb739a85a6579794b9efa` |
| Windows per-user setup | `release-artifacts/windows-verified/mikai-0.1.0-windows-x64-setup.exe` | `60ba8054321420f2439eefca11784738bd76d1672cac4688828ae6852451b4b3` |
| Linux bundled Qt/mpv archive | `release-artifacts/linux-verified/mikai-0.1.0-linux-x86_64.tar.gz` | `31c594ac5a33376af9c5dac99b3b5d621490f3460f8c9fc04c85212528ad3ab5` |

The Linux archive is dependency-based: host libc/loader, graphics drivers and audio facilities remain required. It is not a universal standalone Linux package. Both platform directories include checksum files; the Windows directory also includes the producer's release JSON. Older review/candidate directories are historical evidence, not the current handoff.

These binaries were produced from the verified worktree. Their producer records are retained as generated; they are not presented as clean tagged builds or bit-for-bit reproducible releases. A preparation commit does not retroactively change their recorded build provenance.

## Source, history and privacy

- Generated `dist-mikai/` files were removed from the unpublished history through a separately verified mirror. All 297 source commits, author/committer identities and message bytes were retained; commit IDs changed. Eleven refs were updated atomically.
- Current source-tree and index contents were preserved. All 649 tracked/nonignored working files at cutover were verified unchanged; actual distribution, native/model data and preferences were not deleted.
- The rewritten reachable history has no generated distribution paths and no blobs exceeding GitHub's 100 MiB limit. Full backups and the commit map are retained outside the repository for recovery.
- Redacted secret scanning reviews the reachable history and the exported public source. One synthetic `X-API-Key` redaction-test fixture has exact history/source fingerprints allowlisted; the rule itself is not disabled. No working `.env` was read or included.
- The source snapshot hashes each member as it is actually archived. Size/read guards are not an atomic filesystem snapshot and do not detect every possible concurrent edit. See [source packaging](../packaging/source/README.md).
- Public gallery evidence is under `docs/images/gallery-release-0.1.0/`. Screenshots are real widget captures; the video is project-generated and contains no third-party footage. The media-information image shows an owned scratch path, not user data; its JSON text redacts that scratch root.

The source archive, source-only secret scan and preparation commit are recorded in the local handoff. The preparation commit does not certify the unresolved uninterrupted-desktop observation. No unverified generated executable, installer, model store or cache is intended for the source commit.

## Evidence locations

- Windows acceptance: `.probe-out/release-acceptance-226b5e67d71e466c94f7b46d1489dfef/` (builds, final CTest log/JUnit, source/CI contracts, redacted history reports, Linux receipts).
- Exact final installer: `.probe-out/installer-smoke-windows-verified-20261024/installer-smoke.json` (installer hash, process exits, payload/sentinel/integration checks).
- GUI gallery: `docs/images/gallery-release-0.1.0/gallery-evidence.json` and its eight PNGs.
- Playback investigation: owned `mikai-playback-acceptance-20261024/evidence` scratch tree outside the repository; failed/invalid-context observations remain distinguishable from passing runs.

## Public binary distribution hold

`profiles.publicReleaseReady` remains **false** for `windows-x64` and `linux-x86_64`. Successful builds, tests and screenshots do not clear the source/license gate.

The matching Windows `mpv-winbuild` run `37965200722` logs show GPL-enabled libmpv and static FFmpeg GPL version 3 or later, not an LGPL-only runtime. The exact deployed Windows library reports mpv `v0.41.0-dev-g19c0fcf89` and FFmpeg `N-127263-g3fe6c8dee`.

Retained materials in `release-artifacts/corresponding-source/` include official Qt 6.12.0 source, pinned mpv source, the runtime-reported FFmpeg base commit `3fe6c8dee248df6732c400bced46d8d0557d3444`, and the primary Windows/Linux build recipes, with hashes and acquisition records. `completeCorrespondingSource` is **false**: matching downstream patches, transitive static/private-library source and license closure, and relevant provider/build fulfillment remain incomplete. Source versions and downloaded base archives are not proof of unmodified binary-to-source correspondence.

Optional native components/models remain excluded from the base packages and retain their own rights restrictions. See [redistribution](redistribution.md), [release procedure](releasing.md) and the component manifests before any public binary release. No publication flag should be cleared merely because local QA passed.
