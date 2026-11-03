# Release status

Published packages, release-specific changes and checksums are available on the [GitHub releases page](https://github.com/smyhlin/Mikai-Ja-Nai/releases). The original [v0.1.0](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0) was followed by the [player and usability update](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0-update-20261101). Local rebuilds produce their own `SHA256SUMS`; a rebuild alone does not require a documentation update.

## What remains open

- Packages are unsigned. Linux builds also depend on compatible host graphics, audio, glibc and loader support; no package is universal.
- Third-party corresponding-source, license and redistribution obligations remain unresolved. Both platform profiles in `packaging/third-party.json` retain `publicReleaseReady: false`; see [redistribution evidence](redistribution.md). Publication does not establish rights clearance or a passed release gate.
- Wayland compositors control window stacking, including the AnimeJaNai engine-build overlay.

## Playback observation tracking

HDR dark-scene instability and periodic frame substitution (KB-29 and KB-30) remain under measurement. Rendering fixes and bounded tests do not establish that every black flash is resolved; see [known bugs](../known_bugs.md). A historical Linux stock-runtime hour run ended with black, frozen output; its cause was not established.

Pacing causality for "very fast, then very slow" playback is also still under measurement
(KB-30). GOAL-F removed one confirmed static aggravator — an armed presentation slot could be
disarmed and its frame presented by a repaint Qt asked for, which is the burst-then-stall
signature — and corrected what the log says about it: `disarm` now counts only passes that really
stop an armed timer, and `lead_ms` is measured at the swap rather than at the end of `paintGL`, so
it includes FBO composition and the vsync wait. The canonical before-side stays
[`GOAL-E`](ai/reports/GOAL-E.md): paused chrome-only p50 410 µs / p95 490 µs. The after-side run
`docs/ai/GOAL-E/windows-load-after-pacing` passed its 23 load/context checks, but paused chrome-only
p95 was 899 µs (p50 676 µs). That is not a 30 percent reduction. The same run still recorded 3429
dirty chrome-cache misses and 543 ms of chrome raster, so the remaining cost is the dirty overlay,
not an unmeasured pacing slot. No pacing-cost fix is claimed.

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

The hover, loadfile, geometry and chrome before-side values come from
[`win-before-baseline-04/samples.jsonl`](ai/GOAL-00/win-before-baseline-04/samples.jsonl)
at `8deb325`. That run reported `MEASURED`, with paused bright video and native
foreground validated throughout the requested hover interval.

Activation values use the subsequent complete readiness-barrier run,
[`win-before-focus-05/samples.jsonl`](ai/GOAL-00/win-before-focus-05/samples.jsonl)
at `463b2a3`: every plain, processing-overlay and dock-hover cycle was valid,
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
`hoverStorm.pacingDisarms` is the validated
interval only, not ticks made while another application owned the foreground. The probe invocation, fixture options and extraction contract are in
[the GOAL-00 report](ai/reports/GOAL-00.md).
The idle-GPU lavfi runs did not reproduce an eight-second activation delay or
a black load transition. The processing overlay is UI stress, not a real
TensorRT build. These observations do not prove behaviour during TensorRT builds,
physical scanout latency, or DirectFlip promotion; the latter metric remains
unsupported on this WGL path. Later presentation changes must retain this
before-side evidence rather than describing these zero-event observations as
a demonstrated fix.

## Recorded verification

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
