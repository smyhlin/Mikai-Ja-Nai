# Screenshot provenance

**Current gallery:** `gallery-2026-11-21/` holds eighteen PNGs captured by the opt-in `release_gallery_probe --full-docs-gallery` run on 2026-11-21 from the running Linux x86_64 application (mikai 0.2.3.2, Qt 6.12.0, xcb/Xvfb). It supplies the inline illustrations in the player, user, settings and processing guides.

**Historical records:** `gallery-2026-11-05/` (manual Windows captures, 2026-11-05), `gallery-2026-11-01/`, `screenshots/` (2026-10-30) and `gallery-release-0.1.0/` (2026-10-24) are retained as run evidence, not as pictures of the current interface; their player, Compare and media-information images must not be read as a description of what the application does today.

Neither the current gallery nor the historical records are release QA, rights clearance or approval for further distribution; see [release status](../release-status.md).

## Current gallery (2026-11-21, probe)

| File | Size | SHA-256 | Shows |
|---|---|---|---|
| `catalogue.png` | 1280×800 | `4bd2a4f7439986ec408aa21148cca41da0a809571a97ecfb4f61223bfcfec091` | Catalogue home: sidebar, search toolbar, request-quota badge, live feeds & posters |
| `filters.png` | 760×780 | `df6713ef81d099c2c876895aab93af28e856210e8ff832aba7e4656c72e2bdf8` | Catalogue filters dialog (genres, tags, years, format, status, season, studio, sort) |
| `title-page.png` | 1280×800 | `5dcae3aa7267b098d6079dd01c1ffb3c9340683b456ba1555f77856fac2d2e11` | Live Title Page with poster, synopsis, metadata, provider tabs, episode list |
| `player.png` | 1280×800 | `9dfa612f998e283bfc79a9c9c07474895225e20d75346a0ad88aad5d6d178ac7` | Qt Quick/QML player paused on 1080p fixture with timeline, transport controls, codec badges |
| `player-hover-preview.png` | 1280×800 | `e4e11d3698cf2433ca690bb2438b4b4e1dce2c1bfd93cfade68a56420493c0ec` | Timeline hover thumbnail preview card with decoded frame |
| `player-settings-menu.png` | 1280×800 | `5d5f0481a0dac39fb492d3ead91d5d96fd0c5ceb554949567117ff9202b89769` | Player Settings popover menu (quality, speed, audio, subtitles, scaling) |
| `player-processing-menu.png` | 1280×800 | `a3654ee77562e5e99342f2aedf17fd2de9cfb1c8d32a6e9e912dcef4950c1fef` | Player Enhancements/Processing popover menu (Original, Anime4K presets, AnimeJaNai, Native) |
| `player-pip.png` | 480×270 | `f8648eb789cc9f02c10c3d5b1e0cf91a1ad9d2bc4819525af237500d132e7033` | Redesigned compact Picture-in-Picture window (auto-hide on focus/mouse leave, full-surface drag, minimal overlay) |
| `clip.png` | 1060×820 | `33de0b67ec34ca27100d6db5424f3d65367a7b4822b3493ca0f16f9753d20d8c` | Cut Moment / Clip editor window with sequential filmstrip, trim handles, quality selector, and size estimate |
| `compare.png` | 1280×800 | `7c0f583ee9a553ea0a19a2423a53883a5d6d6ec20eb8f4a1dcffe7a69e0d323a` | Side-by-side Compare window (Original vs Anime4K A Fast) |
| `media-info.png` | 1280×800 | `73538bc70eeceba45a7c01c0509da059733615b40019dbecedb7dc47ddb887df` | Media Information overview dialog |
| `settings.png` | 1280×800 | `7f8abb686699b229bdafd6b707261c626fd20bb2970d8404efbbb38f7a49802e` | Settings → Customization → Appearance (accent, corner radius, background gradient) |
| `settings-customization.png` | 1280×800 | `2a116fea9ebbdb38e0333dce097f8952de894fff750e3fdf495bb3f9bcaea272` | Settings → Customization → General (Dark/Light theme, title & name language) |
| `settings-general.png` | 1280×800 | `39b4e2e28e78ba3aa8c6c9df3c2eb8f126c0952a7c27fb9bc3c885b3bc5575b1` | Settings → Application → Access & General (interface language, HDR policy, Stable/Nightly update channel, auto-update, scaling, API key) |
| `settings-shortcuts.png` | 1280×800 | `5564a9cdd706392b58ab2688da61649ccaaf4ebc4d7091cca3797572d7c39b76` | Settings → Application → Shortcuts (Anime4K preset hotkeys) |
| `settings-processing.png` | 1280×800 | `9ba353b3bbfd98b46715a0d0e7c93ae14ed6dfe07ed0881f07f6e3b544ec5182` | Settings → Playback → Processing (Standard vs Native runtime, backend, recipe) |
| `appearance-before.png` | 1280×800 | `0726f6cb55e789f4a78d18d6c190e5073ce01869aa5add4fe6412b2876d2f857` | Catalogue baseline before live appearance preview |
| `appearance-preview.png` | 1280×800 | `65b92d7f7ea9046391b96ea46b31d025714a3514d66dd74a2e6e6a2acdb0e9f1` | Catalogue live preview after accent/gradient/radius changes |

Capture conditions:

- Platform: Linux x86_64, Qt 6.12.0, xcb/Xvfb. The running binary logged `mikai 0.2.3.2` at startup.
- Method: opt-in `release_gallery_probe --full-docs-gallery` captured real widgets at a fixed 1280×800 logical size and wrote a `gallery-evidence.json` record beside the PNGs. No mockups, compositing, edited pixels or fabricated catalogue data.
- Fixture: 18s 1080p synthetic fixture plus live anonymous catalogue.
- Catalogue content: live catalogue and artwork from the configured Mikai service (anonymous).
- Privacy: no API token field is visible in any capture.

## Historical probe galleries

The probe-based galleries below were produced by the opt-in `release_gallery_probe` executable, which grabbed actual widgets at a requested 1280×800 logical size and wrote a `gallery-evidence.json` record beside the PNGs. Their files and JSON are untouched.

`gallery-2026-11-01/` is the 2026-11-01 Windows run: seven non-duplicate PNGs plus the raw evidence. The raw evidence records eight captures; the omitted `appearance-reset.png` has the same SHA-256 as `appearance-before.png` (prefix `75970cee89bf`). Its player capture shows a project-generated `testsrc2` fixture without a MoonAnime segment or hover preview. Like every record here it reflects the layout of its own run, which has since changed.

`screenshots/` preserves the 2026-10-30 run, whose omitted reset capture has the same SHA-256 as its `appearance-before.png` (prefix `c4da62985a25`); both records remain in `screenshots/gallery-evidence.json`. That run predates the redesigned Home/Search header and player timeline features.

`gallery-release-0.1.0/` is the distinct 2026-10-24 run. Its raw JSON is unchanged and retains the reset record, whose digest equals its `appearance-before.png` digest (prefix `b3b308e2ba43`; full digest in `gallery-release-0.1.0/gallery-evidence.json`); only the redundant historical reset PNG is omitted. The corresponding raw JSON filenames resolve to `appearance-before.png` through their identical recorded hashes, without editing or losing any capture record.

Those runs recorded their environment, widget identities, observed state and digests, and those records still describe what those runs observed. They do not describe the current interface, they are not the source of the current README gallery, and their player frames must not be presented as current playback evidence.

## Historical TitlePage capture (2026-10-31)

`docs/images/screenshots/title-page-current-20261031.png` (stored as `screenshots/title-page-current-20261031.png`) is a separate manually captured TitlePage image from the running application on 2026-10-31. It is **not** part of any gallery evidence set and does not alter or replace any historical gallery file or digest.

- Platform: Windows, Qt 6.12.0, MinGW build `build/release/mikai.exe`.
- API: live anonymous Mikai API; no API key used or recorded. Captured title is ref `7461` ("Мачуха та зведені сестри, які не цькують мене" / "My Stepmother and Stepsisters Aren't Wicked").
- Isolation: staged temporary home/config/data/cache under `build/release/_capture-root`; the app was launched normally, not through a probe, with no user data destroyed.
- Window: 1505×1085 client pixels captured; interface language Ukrainian (system default), default appearance, no playback started.
- MPV runtime was available in the build tree but no video was played; this is a static widget grab.
- Not a probe run: no `gallery-evidence.json`, no clean-shutdown or worker-drain attestation, and no evidence beyond the file digest `47d76554533b674c914abf6c705012c8392dba22ac216834ff9ae637f7b763ea`.

## Capture procedure

### Manual capture (current gallery)

A manual capture is sufficient for documentation images. No probe run is required, and none is prescribed here; the historical probe galleries above remain valid evidence of the runs they recorded.

1. Build the revision you intend to document and launch the real application on a desktop session. Confirm the binary with `--version` or the startup log line (`mikai <version> (<commit>, built <timestamp>)`).
2. Confirm the window you are capturing belongs to that process, and that no privacy-sensitive control is visible: close or scroll away from API-token fields, and keep machine-specific catalogue history out of frame.
3. Capture the real window with an OS or desktop tool. Do not composite, mock up, or edit catalogue data, artwork or interface state.
4. Record what the image depends on: application revision, Qt/platform, interface language, accent and gradient, window size in pixels, whether anything was saved or applied, and whether any component was installed.
5. Compute and record a SHA-256 digest for each file, and list the images with their content in this file. Replace the previous gallery section rather than mixing old and new captures.
6. Recapture whenever the interface, the shown state or the capture inputs change; do not keep presenting an older image as current.

### Probe gallery (historical route)

The earlier galleries came from the opt-in probe with `MIKAI_BUILD_PROBES=ON`, run on an interactive desktop whose available work area is at least 1280×840, with working desktop OpenGL and the real desktop platform plugin (Windows or X11); offscreen/minimal platforms are refused. A run required a bright original local video of at least eight seconds with ready video-track metadata (audio optional), an installed Standard mpv runtime module, and network access to the catalogue API and artwork. It used only Original playback and non-AI comparison; it did not acquire models, runtimes or invoke AI inference.

From the build directory, for example:

```text
cmake -S . -B build -DMIKAI_BUILD_PROBES=ON
cmake --build build --target release_gallery_probe
set MIKAI_MPV_MODULE=C:\path\to\installed\standard\mpv\libmpv-2.dll
build\release_gallery_probe.exe --output-dir C:\path\to\new-gallery-output --video C:\path\to\original-demo.mp4 --video-credit "Creator; CC0 1.0" --video-source "Accurate source or creation details"
```

A run writes `gallery-evidence.json` with application/Qt/platform details, screenshot paths and SHA-256 digests, observed widget/playback state, the operator-declared video credit/source, repeated Appearance-control response and references to the real catalogue-window before/preview/reset captures, errors, and close/worker-drain evidence. The run succeeds only if all five real surfaces, all three supporting appearance captures, and a clean application shutdown are observed. Unavailable catalogue data or artwork is an error; there is no fixture or fabricated substitute. The probe records but cannot independently verify the supplied video attribution.

Use an absolute `MIKAI_MPV_MODULE` path to the installed Standard libmpv module DLL (not the mpv command-line executable); the probe does not search a user's install or download a runtime. If it cannot locate the Qt plugin tree, set `MIKAI_PROBE_QT_PLUGINS` to the Qt installation's `plugins` directory. It clears `MIKAI_API_KEY` and `MIKAI_PROBE_BUNDLE`, launches a verified copy in a fresh owned temporary root, and redirects app settings, home, cache, data, native store, `.env` lookup and working directory there. The temporary root is removed after the child exits; never point output into it or set isolation markers yourself. The application has a 180-second capture deadline and the isolation wrapper kills a child that exceeds 240 seconds.

The probe is developer/release tooling, not a product command or a CI screenshot generator. CI builds and exercises its own hermetic renderer smoke separately; it does not claim that this live-network gallery procedure was run.

## Using these images

Captures describe a single observation. Do not use any image here to imply sign-in, provider playback, AI inference, performance results or release QA. Catalogue content and appearance can change, and a fresh manual capture is required whenever the interface or shown state changes.
