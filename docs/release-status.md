# Release status

Published packages, release-specific changes and checksums are available on the [GitHub releases page](https://github.com/smyhlin/Mikai-Ja-Nai/releases). The original [v0.1.0](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0) was followed by the [player and usability update](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0-update-20261101). Local rebuilds produce their own `SHA256SUMS`; a rebuild alone does not require a documentation update.

## What remains open

- Packages are unsigned. Linux builds also depend on compatible host graphics, audio, glibc and loader support; no package is universal.
- Third-party corresponding-source, license and redistribution obligations remain unresolved. Both platform profiles in `packaging/third-party.json` retain `publicReleaseReady: false`; see [redistribution evidence](redistribution.md). Publication does not establish rights clearance or a passed release gate.
- Wayland compositors control window stacking, including the AnimeJaNai engine-build overlay.

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
composition proof. A prior native hover canary obtained no paced-delay samples on due-now
callbacks, so its pacing gate did not pass. No new numerical result is claimed after the final
build-and-screenshot-only instruction.

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

The original Windows v0.1.0 packages could fail at startup because of an incompatible C++ runtime. The player and usability update includes the source fix. See its release notes for the changes and download details.

## Further evidence and procedures

- [Build instructions](building.md) and [package/release procedures](releasing.md)
- [AnimeJaNai loader and engine-build troubleshooting](animejanai.md)
- [Benchmark results and workload limitations](../bench_results.md)
- [Screenshot provenance and capture procedure](images/README.md)
- [Source snapshot packaging](../packaging/source/README.md)
