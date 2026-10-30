# Screenshot provenance

**Current gallery status:** A successful capture run completed on 2026-10-30; its full-resolution PNGs and unmodified `gallery-evidence.json` are in `screenshots/`. The Settings capture shows the default gradient is Blue (`#6797e4`). The 2026-10-24 run in `gallery-release-0.1.0/` remains a separate historical record. In each run, the raw JSON still names `appearance-reset.png`, but its recorded digest is identical to `appearance-before.png`; that byte-identical duplicate PNG is omitted, while both capture records and their full digests remain unchanged in the respective `screenshots/gallery-evidence.json` and `gallery-release-0.1.0/gallery-evidence.json`. The shared SHA-256 prefixes are `957ad8686071` (2026-10-30) and `b3b308e2ba43` (2026-10-24); see those JSON files for the full digests. Therefore the omitted reset filename resolves to `appearance-before.png` by the recorded identical digest in that run. This capture is not release QA, rights clearance or approval for further distribution; see [release status](../release-status.md).

The gallery PNGs must be captured from the running Mikai application by the opt-in `release_gallery_probe` executable. They are actual widget grabs, not mockups or manual composites. The probe requests 1280×800 logical windows and records each actual logical and pixel size:

- `catalogue.png` — live anonymous catalogue cards and artwork from the configured public service.
- `player.png` — the application player showing a supplied original demo video.
- `compare.png` — the application's real comparison window, with its available non-AI spatial/shader choices.
- `media-info.png` — the real media-information Overview dialog after its asynchronous metadata read completes.
- `settings.png` — the real Settings appearance page, captured without saving changes.
- `appearance-before.png`, `appearance-preview.png` and `appearance-reset.png` — supporting captures of the real catalogue window immediately before the temporary appearance edits, with the preview applied, and after Reset has returned it to defaults. They are evidence of that run, not extra product surfaces. In both retained runs the before/reset files are byte-identical; the reset JSON record and hash remain intact, but the redundant reset PNG is omitted and is represented by `appearance-before.png`.

A run writes `gallery-evidence.json` with application/Qt/platform details, screenshot paths and SHA-256 digests, observed widget/playback state, the operator-declared video credit/source, repeated Appearance-control response and references to the real catalogue-window before/preview/reset captures, errors, and close/worker-drain evidence. The run succeeds only if all five real surfaces, all three supporting appearance captures, and a clean application shutdown are observed. Unavailable catalogue data or artwork is an error; there is no fixture or fabricated substitute. The probe records but cannot independently verify the supplied video attribution.

## Current captures

`screenshots/` contains the seven non-duplicate current PNGs (five product surfaces plus appearance-before/preview) and the unmodified `gallery-evidence.json` from the successful Windows run on 2026-10-30. The raw evidence records all eight screenshot captures, including `appearance-reset.png`. The reset capture has the same SHA-256 as `appearance-before.png` (prefix `957ad8686071`; full digest in `screenshots/gallery-evidence.json`), so the duplicate file is omitted; the raw reset record and its digest are preserved. `screenshots/` is the source for the full-resolution top-level README images; no downscaled derivative copies are retained.

`gallery-release-0.1.0/` remains the distinct historical 2026-10-24 run. Its raw JSON is unchanged and retains the reset record, whose digest equals its `appearance-before.png` digest (prefix `b3b308e2ba43`; full digest in `gallery-release-0.1.0/gallery-evidence.json`); only the redundant historical reset PNG is omitted. The corresponding raw JSON filenames therefore resolve to `appearance-before.png` through their identical recorded hashes, without editing or losing any capture records.

The current run's `gallery-evidence.json` records a 12-second project-generated FFmpeg `testsrc2` video fixture (`tests/fixtures/generate_media.sh`; the operator-declared credit is not independently verified), live anonymous catalogue metadata and artwork, capture timestamps, widget identities, state and SHA-256 digests. These are real widget grabs, not AI-generated or composited images. The run completed on Windows on 2026-10-30; its captures are stored here without image edits.

## Capture procedure

Build the opt-in probe with `MIKAI_BUILD_PROBES=ON` and run it on an interactive desktop whose available work area is at least 1280×840, with working desktop OpenGL. Use the real desktop platform plugin (Windows or X11); offscreen/minimal platforms are refused. The run requires a bright original local video of at least eight seconds with ready video-track metadata (audio is optional), an installed Standard mpv runtime module, and network access to the anonymous catalogue API and artwork. The probe uses only Original playback and non-AI comparison; it does not acquire models, runtimes or invoke AI inference. A successful run requires all five surfaces; a catalogue/API/artwork failure leaves the gallery incomplete.

From the build directory, for example:

```text
cmake -S . -B build -DMIKAI_BUILD_PROBES=ON
cmake --build build --target release_gallery_probe
set MIKAI_MPV_MODULE=C:\path\to\installed\standard\mpv\libmpv-2.dll
build\release_gallery_probe.exe --output-dir C:\path\to\new-gallery-output --video C:\path\to\original-demo.mp4 --video-credit "Creator; CC0 1.0" --video-source "Accurate source or creation details"
```

Use an absolute `MIKAI_MPV_MODULE` path to the installed Standard libmpv module DLL (not the mpv command-line executable); the probe does not search a user's install or download a runtime. It clears `MIKAI_API_KEY` and `MIKAI_PROBE_BUNDLE`, launches a verified copy in a fresh owned temporary root, and redirects app settings, home, cache, data, native store, `.env` lookup and working directory there. The temporary root is removed after the child exits; never point output into it or set isolation markers yourself. The application has a 180-second capture deadline and the isolation wrapper kills a child that exceeds 240 seconds. `--video-credit` is required and must accurately state the creator and license/permission; `--video-source` is optional but should document the source.

The probe is developer/release tooling, not a product command or a CI screenshot generator. CI builds and exercises its own hermetic renderer smoke separately; it does not claim that this live-network gallery procedure was run. Captures are evidence from the individual run, not a guarantee that remote catalogue content or appearance is unchanged over time. Overall release status remains blocked; see [release status](../release-status.md). Screenshots and evidence describe a successful run only. Do not use them to imply sign-in, provider playback, AI inference or performance results. A fresh successful run is required whenever the application appearance or capture inputs change.