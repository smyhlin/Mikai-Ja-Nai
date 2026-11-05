# Native-Linux verification of the Windows-recorded QA material — 2026-11-05

The Windows runs behind this repository's goal reports recorded a passing player-probe
gate. This document is the native-Linux counterpart: the same tree, built and exercised
on this desktop. The Windows evidence tree (`docs/ai/`, plus three local
`qa_matrix*.txt` diffs) was produced on Windows and has since been deleted from this
checkout; the commits it described remain reachable from
`goal/E-playback-matrix-proof`, so the probe code is still inspectable there.

All three diffs touch only `src/probes/player/FullscreenStabilityProbe.cpp`. Their
changes — the QA-matrix mode, the `isPipActive()` exposure rule, the `idleWait` settle
pump, and the pre-monitor peer warm-up — are present in this tree behind later
integration commits, so building and running it exercises them.

Result: the tree builds and its own test gate is green on Linux after two fixes, but the
probe gates that Windows recorded as passing do **not** pass here.

## Environment

- Arch Linux, kernel 7.2.11, Plasma/Wayland (no XWayland; `/tmp/.X11-unix` absent).
- Screen HDMI-A-1, 1920x1080 at device pixel ratio 2.
- System Qt 6.12.0, GCC, `mpv` 0.41.0 on the host, plus the pinned TLS runtime
  `stock-f9e3b9b60ddf` from `release-artifacts/mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst`.
- Build trees: `build/linux-release` (system mpv, probes on) and `build/linux-bundle`
  (pinned runtime, probes on).

```sh
cmake -S . -B build/linux-release -DCMAKE_BUILD_TYPE=Release -DMIKAI_BUILD_PROBES=ON
cmake --build build/linux-release --target mikai mikai_tests --parallel 12
cmake --build build/linux-release --target mpv_surface_smoke load_transition_probe --parallel 8
cmake --build build/linux-bundle  --target mpv_surface_smoke load_transition_probe fullscreen_stability_probe --parallel 8
```

App, tests and all three probes build cleanly at `bf37701`; `mikai --version` reports
`0.2.0 (bf37701, built 2026-11-05T17:48:20Z)`.

## CTest

| run | result |
|---|---|
| `ctest --test-dir build/linux-release --timeout 900` at `bf37701`, before fixes | 96 %, 3 failed: `mikai_tests`, `mikai_tests_TestFeedRender`, `mikai_tests_TestPublicApiUi` |
| same, after the two fixes below | 100 %, 68/68 in 567.05 s |

The `mikai_tests` blob failure in the first run did not reproduce: it passed twice in
isolation (71.94 s, 73.60 s) and again inside the second full run. `HoverPlaybackCanary`
is skipped under the launcher because the launcher runs the probe with the offscreen
platform, where no paced paint sample can exist.

### Two Linux-only failures, both fixed

1. `TestFeedRender::anAuthoritativeBackgroundBuildFailureSurfacesItsBoundedLogTail`
   — `animeJanaiBuildFailureEnginePath("aji", "[aji] background engine build failed: …")`
   returned `""`. `animeJanaiBuildFailureLogPath` stripped the `[aji] ` header only for
   the `animejanai` prefix, so the `aji` prefix never matched the marker. mpv delivers
   the message text with the bracket and the prefix separately, so both shapes reach the
   parser. Fixed in `b05a90f`.
2. `TestPublicApiUi::settingsPagesFitTheDialogMinimum` — `components` in `uk` needed
   10 px of horizontal panning. The components page's action card is 306 px wide at the
   dialog minimum while the page gives 296 px, because `kSettingsDialogMinWidth = 580`
   leaves a 312 px page viewport at this font's metrics. Fixed in `a7bed6c` by raising the
   floor to 600 px; the English page and every other audited page still fit.

## Probes

### `mpv_surface_smoke --render-lifecycle` — FAIL, 4/114

```sh
./build/linux-release/mpv_surface_smoke --render-lifecycle --out /var/tmp/linux-probe-out/lifecycle
```

Windows recorded PASS (114/114). Here:

- `the platform placed the host in the minimized state` — Wayland never reports the
  minimize the probe asked for.
- `the settled minimized surface performs no video paints` — follows from the above.
- `playback resumed after the paused resize` — no new output frame within 4 s of
  `setPaused(false)`.
- `the main surface still rendered output frames after the second one was destroyed` —
  same stall class.

The minimize pair is platform semantics; the two resume checks are frame production
after a pause, not diagnosed further here.

### `load_transition_probe` — FAIL, the retained picture is black on Linux

```sh
./build/linux-bundle/load_transition_probe .probe-out/goals/focus-1080p-6s.mp4 \
    --output-dir /var/tmp/linux-probe-out/plain-load
./build/linux-bundle/load_transition_probe .probe-out/goals/focus-1080p-6s.mp4 \
    --output-dir /var/tmp/linux-probe-out/bundle-load --context-loss
```

| run | gap | samples | `fb_black` | `retained` | first output | result |
|---|---:|---:|---:|---:|---:|---|
| Linux, pinned runtime, plain load | 2572 ms | 126 | 125 | 125 | 75359 ms | FAIL (2/24) |
| Linux, pinned runtime, `--context-loss` | 2561 ms | 121 | 120 | 0 | 86708 ms | FAIL (3/25) |
| Linux, host mpv 0.41, `--context-loss` | 2704 ms | 133 | 135 | 132 | 89120 ms | FAIL (3/25) |

The Windows records it is compared against, from the now-deleted evidence tree:

| Windows run | gap | samples | `fb_black` | `retained` | result |
|---|---:|---:|---:|---:|---|
| `windows-load-after` | 2738 ms | 84 | 0 | 80 | FAIL (23 checks, 3 failed — not on the black gap) |
| `windows-load-after-9bdd850-band-audit` | 2747 ms | 18 | 0 | 17 | PASS (24/24) |

Windows paints the held picture through the same 2.7 s gap; Linux reports the picture as
retained (`hasRetainedPicture()` true) while the sampled framebuffer is black for every
gap sample. So the "no black between the last outgoing frame and the first new output"
guarantee the Windows runs demonstrate does not hold here. A scale-1 run
(`QT_SCALE_FACTOR=1`) shows the same numbers, so this is not the HiDPI band.

The hold is served by `engineFrameHoldFbo_`, filled by `captureEngineFrameHold()` from
`render_->render(...)`. On this platform that render produces black for the previously
decoded frame, which is the most likely cause and is not proven by these samples. Not
changed: no fix was verified.

The `--context-loss` stage also fails its own check:
`a paused picture and its pause intent survive a real context recreation (result=1)`.

### `fullscreen_stability_probe --qa-matrix` — INCOMPLETE, no desktop capture

```sh
QT_QPA_PLATFORM=wayland MIKAI_MPV_MODULE=$PWD/build/linux-bundle/mpv/libmpv.so.2 \
  ./build/linux-bundle/fullscreen_stability_probe \
  "$PWD/.probe-out/goals/focus-1080p-cfr-360s.mp4" 300 /var/tmp/linux-probe-out/matrix --qa-matrix
```

Exit 3 after the 12 s calibration window. The probe's own record:

> the desktop grab never returned a bright pixel in the 12015 ms calibration window
> although the composed framebuffer was bright 367 times (crop 96x64 at 489,324, screen
> HDMI-A-1, dpr 2); peer occlusion was not attempted

`QScreen::grabWindow` returns a null image under this Wayland compositor: the desktop
capture the mode needs is unavailable, so no five-minute matrix verdict exists for
Linux. The framebuffer side was bright and playing throughout the calibration window.

An X11 attempt was made to separate the capture limitation from the mode itself: Xvfb
`:99` at 1920x1080x24 with `QT_QPA_PLATFORM=xcb`. That run refuses before calibration:

> native activation of another process's window is not available on this platform

The probe activates a separate helper process to drive its ten away/back cycles, and
Xvfb has no window manager to honour the request. `kwin_x11` is the only window manager
installed here and it exits on a library symbol mismatch
(`/usr/lib/libkwin-x11.so.6: undefined symbol _ZN12ScreenLocker7KSldApp4lockE…`), so no
X11 window manager could be provided. Both display servers lack one of this mode's two
prerequisites — desktop capture and native activation of a foreign window — which is the
recorded result.

### `fullscreen_stability_probe --hover-canary` — INCOMPLETE, no paced sample

```sh
QT_QPA_PLATFORM=wayland MIKAI_MPV_MODULE=$PWD/build/linux-bundle/mpv/libmpv.so.2 \
  ./build/linux-bundle/fullscreen_stability_probe \
  "$PWD/.probe-out/goals/hover-motion-48s.mp4" 30 /var/tmp/linux-probe-out/hover --hover-canary
```

`pacedSamples = 0`, `paintDelayP95Ms = unavailable`, 1792 gestures, 1760 paint calls,
3532 chrome-dirty applications, 0 chrome-only disarms. The Windows pacing canary ended
the same way: no paced sample, so the paused-hover p95 is unmeasured on both platforms.

## What this establishes

- The Windows-produced probe code compiles and runs on Linux; the three diffs' logic is
  in this tree.
- The application and its 68-entry CTest gate are green on Linux after two fixes that
  only Linux surfaced (log-attribution asymmetry, translated settings row width).
- Windows-recorded probe passes do not transfer: the retained-picture black gap during a
  source switch is Linux-specific and unexplained, and this platform withholds desktop
  capture, native foreground and a dependable minimize state.
- The probe runs were made at `bf37701`; the two fixes touch `AnimeJanaiBuildLog.hpp`
  and `Metrics.hpp`, neither of which the probes exercise, so none of the probe verdicts
  would change at `a7bed6c`.
- `release-artifacts/` holds the Linux packages built at `6ee0c15`; the two fixes
  (`b05a90f`, `a7bed6c`) postdate them, so those packages do not contain the
  settings-width change.
