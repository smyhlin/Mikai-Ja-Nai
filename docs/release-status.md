# Release status

Published packages, release-specific changes and checksums are available on the [GitHub releases page](https://github.com/smyhlin/Mikai-Ja-Nai/releases). The original [v0.1.0](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0) was followed by the [player and usability update](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0-update-20261101). Local rebuilds produce their own `SHA256SUMS`; a rebuild alone does not require a documentation update.
The latest published release recorded here is [v0.2.0](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.2.0). Post-v0.2.0 work now lives on the `cppfixes` branch (the audit remediation consolidated at `022c98c`, followed only by documentation changes); `main` is at `7642f50` and does not contain the 0.2.1–0.2.2 changes or the audit remediation, both of which are listed as **Unreleased** in the changelog. Windows packages were rebuilt from `022c98c` locally (see below), but no release gate has run for them and nothing here is a published package. Public release remains blocked by the unresolved rights and verification gates below.

## What remains open

- Packages are unsigned. Linux builds also depend on compatible host graphics, audio, glibc and loader support; no package is universal.
- Third-party corresponding-source, license and redistribution obligations remain unresolved. Both platform profiles in `packaging/third-party.json` retain `publicReleaseReady: false`; see [redistribution evidence](redistribution.md). Publication does not establish rights clearance or a passed release gate.
- The native Quick player adds a Qt Declarative runtime/import closure (Qt Qml/Quick/QuickControls2, imported QML modules/styles and plugin dependencies) to the deployment requirements. The current final Linux and broad build/package gates are deferred; this status records no new Linux package test or complete production-UI verification. Earlier native core-only composition/resize/paused/live-resource evidence is limited to that core path and is not a Quick frontend, package, or release-gate result.
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
Its pacing gate remains incomplete; these observations are not a desktop-composition claim.

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
