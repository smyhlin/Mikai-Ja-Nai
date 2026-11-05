# Screenshot provenance

**Current gallery:** `gallery-2026-11-05/` holds seven images captured manually from the running Windows application on 2026-11-05. It supplies the top-level README gallery and the inline illustrations in the player, user, settings and processing guides.

**Historical records:** `gallery-2026-11-01/`, `screenshots/` (2026-10-30) and `gallery-release-0.1.0/` (2026-10-24) are unchanged probe-run captures with their original `gallery-evidence.json` files. They are retained as run evidence, not as pictures of the current interface; their player, Compare and media-information images must not be read as a description of what the application does today.

Neither the current gallery nor the historical records are release QA, rights clearance or approval for further distribution; see [release status](../release-status.md).

## Current gallery (2026-11-05, manual)

| File | Size | SHA-256 | Shows |
|---|---|---|---|
| `catalogue.png` | 1894×1384, 497071 B | `c62b0a308db58f80c9567683ac4658213cf995b3db39ad8f6c3bc98759fda54a` | Catalogue home: sidebar, search toolbar, request-quota badge, currently-airing and latest-update feeds, popular titles |
| `filters.png` | 1334×1419, 55410 B | `87948db23d2eb90cd7fe0940d0f8eb4ef3e3815a0332aa90680f8acf5a061573` | The **Catalogue filters** dialog with its genre list |
| `settings.png` | 1649×1489, 111218 B | `b123e31243f6fe27bb3bfbb1f735381bc3db9f4ecdd167013447af414ef0bf7c` | **Settings → Appearance** with a pink accent and a custom background gradient |
| `components.png` | 1649×1489, 130849 B | `df2d441b3bc3f914a6e6660c6ac3810ce6acf3fea35721b30c570610d884640a` | **Settings → Components** with TensorRT for RTX selected and its runtime reported as not installed |
| `player.png` | 1964×1279, 1268877 B | `c40fbca508da0b3596aab2673138b43a3d41844c9f4e42680882da008084201e` | The player window with a paused local clip, its chrome, timeline and controls |
| `compare.png` | 2244×1454, 1039187 B | `b372fe200e81cda89b5546876e76d9b1f252612aa986f9df0be33a0674c753ba` | The Compare window in side-by-side mode: A disabled, B selected on the bundled **Anime4K A (Fast)** preset |
| `media-info.png` | 1580×1110, 61758 B | `42cf5f606d29e053aafa5af7a998f9d05ec5a38a58d75bcd7af916483323f55d` | The **Media information** Overview dialog for that local fixture, showing path, container, dimensions, frame rate and codec |

Capture conditions:

- Platform: Windows, Qt 6.12, MinGW build `build/release/mikai.exe`. The running binary logged `mikai 0.1.0 (0d8ff35, built 2026-11-05T11:58:23Z)` at startup.
- Method: real window captures of the running application with a desktop capture tool. No mockups, compositing, edited pixels or fabricated catalogue data.
- Catalogue content: live catalogue and artwork from the configured Mikai service. The page was scrolled below the **Recently opened** section so the locally stored history of this machine does not appear in the committed image. The client used the operator's existing settings; the capture does not establish whether an API key was active.
- Isolation: the build tree and `MIKAI_NATIVE_ROOT` were isolated from the user's normal release install, but the user's Qt settings (`Mikai` / `Mikai Wrapper`) were **not** isolated. Window geometry, stored appearance and the local recently-opened list are therefore this machine's state.
- Privacy: no API token field is visible in any capture.
- Appearance: Ukrainian interface language, pink accent and a custom gradient. This is one appearance choice, not the shipped default.
- Components screen: the backend row shows **TensorRT for RTX** and the required runtime is shown as not installed. Nothing was downloaded or installed, **Apply changes** was not pressed, and no inference or playback was started during the four catalogue/Appearance/Components captures.
- `settings.png` is the Appearance page exactly as opened: no edit was saved and no **Apply**/**Save** was pressed. The filter dialog and Settings were dismissed without saving, and the application then exited normally (exit code 0).

Second session, same binary and build tree:

- `player.png`, `compare.png` and `media-info.png` come from a separate launch of the same `mikai 0.1.0 (0d8ff35)` Windows binary. Unlike the four captures above, this session **did** play video: it opened a project-generated 12-second, 320×180, 24 fps 8-bit HEVC colour-pattern clip (`tests/fixtures/media/motion_320x180_24fps_8bit_hevc.mp4`), let it decode, and paused it at about 2.042 s so a real decoded frame was presented in a normal window with its ordinary chrome.
- `player.png` shows that paused local file in the player, not provider or anime playback. No processing preset was selected; the picture is the original decoded frame.
- `compare.png` shows the real Compare window in side-by-side mode, opened on the same paused frame. Side A is disabled (the original); side B has the bundled **Anime4K A (Fast)** shader preset selected. The UI clips the right-hand preset label to `B: A (…`; the image does not establish that the selected shader finished rendering or provide a quality or timing result. No model, runtime download or AI inference was involved.
- `media-info.png` shows the app's metadata dialog for that fixture, including its path `F:\ZKS\mikai_janai\tests\fixtures\media\motion_320x180_24fps_8bit_hevc.mp4`. The path contains no Windows user name.
- The clip is a synthetic project fixture. These images do not show anime footage, provider streams, AI inference, GPU performance or a rights-cleared title.

Limitations:

- Not a probe run: there is no `gallery-evidence.json`, no fixed 1280×800 logical window size, no worker-drain attestation, and no clean-shutdown record beyond the normal application exit. No probe was run for any of the seven images.
- The captures cover one viewport, scale factor, language, theme and appearance. They are not a review matrix.
- `settings.png` and `components.png` share a size because they are the two Settings pages at the same window geometry.
- Catalogue entries, artwork and availability change upstream; a capture is evidence of that moment only.
- The player, Compare and media-information images show one synthetic 320×180 HEVC clip at one paused position, in one window geometry and one appearance. They do not establish playback quality, decode performance, HDR behaviour or that a provider stream plays.

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
