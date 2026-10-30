# Screenshot provenance

**Current gallery status:** **Blocked/pending.** No new gallery captures have been produced under the current release policy; the previous 2026-10-24 captures remain prior-run evidence only. The gallery PNGs in `gallery-release-0.1.0/` are retained as historical evidence, but they are not current verification and must not be represented as a fresh successful run. Overall release status remains blocked; see [release status](../release-status.md).

The gallery PNGs must be captured from the running Mikai application by the opt-in `release_gallery_probe` executable. They are actual widget grabs, not mockups or manual composites. The probe requests 1280×800 logical windows and records each actual logical and pixel size:

- `catalogue.png` — live anonymous catalogue cards and artwork from the configured public service.
- `player.png` — the application player showing a supplied original demo video.
- `compare.png` — the application's real comparison window, with its available non-AI spatial/shader choices.
- `media-info.png` — the real media-information Overview dialog after its asynchronous metadata read completes.
- `settings.png` — the real Settings appearance page, captured without saving changes.
- `appearance-before.png`, `appearance-preview.png` and `appearance-reset.png` — supporting captures of the real catalogue window immediately before the temporary appearance edits, with the preview applied, and after Reset has returned it to defaults. They are evidence of that run, not extra product surfaces.

A run writes `gallery-evidence.json` with application/Qt/platform details, screenshot paths and SHA-256 digests, observed widget/playback state, the operator-declared video credit/source, repeated Appearance-control response and references to the real catalogue-window before/preview/reset captures, errors, and close/worker-drain evidence. The run succeeds only if all five real surfaces, all three supporting appearance captures, and a clean application shutdown are observed. Unavailable catalogue data or artwork is an error; there is no fixture or fabricated substitute. The probe records but cannot independently verify the supplied video attribution.

## README images

`screenshots/` holds WebP downscales (1400 px wide) of the recorded captures, used by the top-level
readme so the page stays light. They are derived from `gallery-release-0.1.0/*.png` and carry no new
evidence: the PNGs and `gallery-evidence.json` remain the record, and a claim about a capture run must
cite those, not these copies.

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

The probe is developer/release tooling, not a product command or a CI screenshot generator. CI builds and exercises its own hermetic renderer smoke separately; it does not claim that this live-network gallery procedure was run. Captures are evidence from the individual run, not a guarantee that remote catalogue content or appearance is unchanged over time. Overall release status remains blocked; see [release status](../release-status.md). Screenshots and evidence describe a successful run only. Do not use them to imply sign-in, provider playback, AI inference or performance results. A new successful run is required before the gallery can be represented as current.