# Release status

Published packages, release-specific changes and checksums are available on the [GitHub releases page](https://github.com/smyhlin/Mikai-Ja-Nai/releases). The original [v0.1.0](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0) was followed by the [player and usability update](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0-update-20261101). Local rebuilds produce their own `SHA256SUMS`; a rebuild alone does not require a documentation update.

## What remains open

- Packages are unsigned. Linux builds also depend on compatible host graphics, audio, glibc and loader support; no package is universal.
- Third-party corresponding-source, license and redistribution obligations remain unresolved. Both platform profiles in `packaging/third-party.json` retain `publicReleaseReady: false`; see [redistribution evidence](redistribution.md). Publication does not establish rights clearance or a passed release gate.
- Wayland compositors control window stacking, including the AnimeJaNai engine-build overlay.

## Playback observation tracking

HDR dark-scene instability and periodic frame substitution (KB-29 and KB-30) remain under measurement. Rendering fixes and bounded tests do not establish that every black flash is resolved; see [known bugs](../known_bugs.md). A historical Linux stock-runtime hour run ended with black, frozen output; its cause was not established.

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

The before-side values below were recorded from
[`win-before-baseline-04/samples.jsonl`](ai/GOAL-00/win-before-baseline-04/samples.jsonl)
at `8deb325`, before GOAL-B/D/H1 or the presentation fixes were integrated.
The run reported `MEASURED`: paused bright video and native foreground were
validated throughout the 30-second hover interval. Of ten activation attempts,
nine were valid; one lost foreground ownership and is excluded explicitly.
There were no composition timeouts or unsupported captures.

| Metric | Row / key | Value |
|---|---|---|
| Pacing disarms in a 30 s hover storm over paused video | `hoverStorm.pacingDisarms` | 0 |
| **Activation → first visible composited desktop frame** p95, timeouts censored as ≥ 2500 ms (verified-occluded off-centre screen crop, not a paint/swap proxy) | `baselineResult.activationToVisibleCompositionP95LowerBoundMs` | 11 ms; 9 valid samples |
| Activation-to-first-swap p95 (secondary diagnostic; `frameSwapped`, not physical scanout) | `baselineResult.activationToFirstSwapP95Ms` | 24 ms; 9 valid samples |
| Black samples across 20 `loadfile` transitions | `baselineResult.blackSamplesLoadfile20` | 0 / 463 samples; 20 transitions completed |
| Surface-geometry writes per fullscreen transition | `baselineResult.surfaceGeometryWritesPerFullscreenTransition` | 2 |
| `ChromeList::paint` mean per composition during the paused hover storm | `baselineResult.chromePaintMeanMicros` | 284.996 µs; 1850 paints |

A run whose `profileCountersEnabled` is false, whose
`baselineStart.preconditionsValid` is false (paused bright frame / native
foreground not verified), or whose `hoverStorm.valid` is false does not produce
these numbers: `baselineResult.result` reads `INVALID`/`INCOMPLETE`, the row
names `invalidReason`, and the probe exits `3` instead of silently passing. Read
the five values only from a `MEASURED` run; that requires
`visibleCompositionComplete == true`, so a timeout is censored at the 2500 ms
deadline instead of being dropped. `hoverStorm.pacingDisarms` is the validated
interval only, not ticks made while another application owned the foreground. The probe invocation, fixture options and extraction contract are in
[the GOAL-00 report](ai/reports/GOAL-00.md).
The idle-GPU lavfi run did not reproduce an eight-second activation delay or
a black load transition. It does not prove behaviour during TensorRT builds,
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
