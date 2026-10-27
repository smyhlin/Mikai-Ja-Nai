# Hermetic media, HTTP/HLS and AJI fixtures

Project-owned fixtures for playback, HLS, model/configuration preparation and
failure recovery, with **no outbound network, CDN or API key** required.
The media assets are generated from ffmpeg's own `lavfi` sources; no
third-party footage is downloaded, copied or re-encoded. The AJI fixtures below
are source-generated ONNX bytes and a test-only dispatcher, not production
models or inference binaries.

- `generate_media.cmd` — reproduces the media/HLS assets from scratch on Windows.
- `generate_media.sh` — the Linux/bash port: the same clips under the same
  filenames, plus the HDR regression clips (blocks 12–13) and the moving
  frame-identity clip (block 14). Skips loudly per block when `ffmpeg` or the
  block's encoder is missing; acceptance jobs must fail if required assets are
  absent rather than substituting a synthetic source.
- `media/` — the clip matrix (10 committed HEVC MP4s, plus generated-on-demand
  HDR and frame-identity clips described below).
- `hls/` — three real HLS renditions with real MPEG-TS segments.
- `http_fixture.cpp` — the loopback fixture server (CMake target `mikai_http_fixture`).

## Generating the assets

```cmd
set FFMPEG=C:\path\to\ffmpeg.exe
tests\fixtures\generate_media.cmd
```

`FFMPEG` defaults to `ffmpeg` on `PATH`. No Python, no Conda. The script only
runs `ffmpeg`: lavfi sources → `libx265`/`libx264` → MP4 and HLS. Nothing else
is required, and nothing reaches the network.

On Linux, the port works the same way:

```bash
FFMPEG=/path/to/ffmpeg tests/fixtures/generate_media.sh
```

`MEDIA_DIR`/`HLS_DIR` optionally relocate the output roots. Missing tools and
failed encodes are named per block, and the final summary exits nonzero if any
required clip, HLS playlist, or HLS segment is absent. Acceptance jobs also
check their selected assets explicitly; the nightly soak lane does not fall
back to a generic lavfi source.

`generate_media.cmd` (and its Linux port) rewrites the full matrix, so either
is also the way to recover a damaged or partial `media/`/`hls/`. The scripts
are self-documenting: each block states what the clip is for and which later
gate consumes it.

## The clip matrix

| File | Geometry | Depth | Codec | fps | Duration | What it is for |
|---|---|---|---|---|---|---|
| `motion_320x180_24fps_8bit_hevc.mp4` | 320×180 | 8 | HEVC Main | 24 | 12 s | The primary clip. `external_media_probe` needs 320×180, >10 s and colourful pixels. |
| `motion_320x180_23976fps_8bit_hevc.mp4` | 320×180 | 8 | HEVC Main | 24000/1001 | 12 s | 23.976 cadence against the 24 fps twin. |
| `motion_320x180_24fps_10bit_hevc.mp4` | 320×180 | 10 | HEVC Main10 | 24 | 12 s | Bit-depth switching at the probe geometry. |
| `motion_1920x1080_24fps_8bit_hevc.mp4` | 1920×1080 | 8 | HEVC Main | 24 | 6 s | HD, limited range (tv) BT.709. |
| `motion_1920x1080_24fps_10bit_hevc.mp4` | 1920×1080 | 10 | HEVC Main10 | 24 | 6 s | HD 10-bit, limited range. |
| `motion_1920x1080_23976fps_8bit_hevc.mp4` | 1920×1080 | 8 | HEVC Main | 24000/1001 | 6 s | HD 23.976. |
| `fullrange_1920x1080_24fps_8bit_hevc.mp4` | 1920×1080 | 8 | HEVC Main | 24 | 6 s | **Full range (pc)** BT.709. Every other clip is limited range. No range/matrix pass-through code consumes this case today; see “Current consumers” below for what actually reads these fixtures. |
| `cut_saturated_black_320x180_24fps_8bit_hevc.mp4` | 320×180 | 8 | HEVC Main | 24 | 6 s | Hard cut + saturated + near-black, **with AAC audio**. |
| `nonmult32_motion_1920x1072_24fps_8bit_hevc.mp4` | 1920×1072 | 8 | HEVC Main | 24 | 4 s | Height 1072 is even but **not a multiple of 32** (1072/32 = 33.5) — the dyn-HW graph case. |
| `portrait_motion_1080x1920_24fps_8bit_hevc.mp4` | 1080×1920 | 8 | HEVC Main | 24 | 4 s | Portrait rotate/fit and preparation-plan geometry. |
| `hdr_dark_gradient_1280x720_24fps_10bit_pq_hevc.mp4` | 1280×720 | 10 | HEVC Main10, BT.2020 + PQ (SMPTE ST 2084) | 24 | 16 s | Generated, uncommitted 10-bit PQ dark-level ramp with a right-edge 150–1000-nit highlight ramp. |
| `hdr_dark_noise_1280x720_24fps_10bit_pq_hevc.mp4` | 1280×720 | 10 | HEVC Main10, BT.2020 + PQ | 24 | 16 s | Generated, uncommitted lossless near-black temporal shadow-noise fixture, encoded losslessly to preserve its signal. |
| `motion_frame_id_1280x720_24fps_10bit_hevc.mp4` | 1280×720 | 10 | HEVC Main10, BT.709 | 24 | 16 s | Generated moving `testsrc2` with an 8-bit per-frame barcode; frame-order soak decodes identity from the rendered framebuffer. |
| `continuous_frame_id_audio_640x360_24fps_10bit_hevc.mp4` | 640×360 | 10 | HEVC Main10, BT.709 + AAC | 24 | 3660 s | Generated continuous 8-bit frame-ID barcode with 48 kHz stereo AAC for no-loop aging and audio-PTS acceptance. |

### The HDR regression clips (blocks 12–13)

The two `hdr_*` clips are generated on demand, not committed. Both are 10-bit HEVC Main10 with BT.2020 primaries and SMPTE ST 2084 (PQ) transfer. The authored gradient fixture combines slowly changing dark levels with a right-edge highlight ramp spanning approximately 150–1000 nits, exercising tone mapping below the peak rather than relying on a flat peak patch. The companion fixture is lossless near-black temporal shadow-noise; lossless encoding preserves its signal. Do not claim these source shapes prove a particular flicker mechanism or root cause. Their presence is mandatory for HDR acceptance; do not substitute a generic source. Generated media resides outside the source tree in the acceptance setup.

### Native HDR Settings UI capture (Windows)

Use the real Windows Qt platform and the built Ukrainian catalog so font
metrics and translated captions match the UI acceptance capture; do not use
the fontless `offscreen` QPA for layout claims. The probe's isolation launcher
creates an owned executable copy and separate empty native store. Do not
manually place/run the probe beside a populated application `native` store.

```powershell
cmake --build build/release --target native_settings_probe
$env:QT_QPA_PLATFORM = 'windows'
$env:MIKAI_PROBE_TRANSLATION = (Resolve-Path 'build/release/mikai_uk.qm').Path
.\build\release\native_settings_probe.exe --hdr-settings-only
```

### Continuous frame-order and HDR acceptance

The frame-ID clip defaults to 16 seconds. Its visible 8-bit barcode identifies advancing frames in the decoded/rendered image. For continuous same-source-epoch aging, generate a 61-minute clip (3660 seconds) with an ffmpeg build that includes `libx265`; do not loop a short file:

```bash
FRAME_ID_SIZE=640x360 FRAME_ID_DURATION_SECONDS=3660 \
  MEDIA_DIR=/path/to/acceptance/media \
  HLS_DIR=/path/to/acceptance/hls \
  FFMPEG=/path/to/ffmpeg tests/fixtures/generate_media.sh
```

Build the standalone probes and run the continuous source with EOF treated as a
failure:

```powershell
cmake --build build/release --target frame_order_soak_probe hdr_playback_probe
$env:MIKAI_PROBE_BUNDLE = 'C:\isolated\mpv-runtime-bundle'
$env:MIKAI_CLIENT_FRAME_PACING = '1'
.\build\release\frame_order_soak_probe.exe `
  --media 'C:\isolated\media\continuous_frame_id_audio_640x360_24fps_10bit_hevc.mp4' `
  --minutes 60 --no-loop --require-audio `
  --frame-timing-log 'C:\isolated\evidence\fork-timing.log' `
  --artifacts 'C:\isolated\evidence\fork-frames'
```

On Linux, use the same continuous fixture and acceptance options with the Linux
probe executable and bundle directory:

```bash
MIKAI_PROBE_BUNDLE=/path/to/isolated/mpv-runtime-bundle \
MIKAI_CLIENT_FRAME_PACING=1 \
  build/linux-release/frame_order_soak_probe \
  --media /path/to/acceptance/media/continuous_frame_id_audio_640x360_24fps_10bit_hevc.mp4 \
  --minutes 60 --no-loop --require-audio \
  --frame-timing-log /path/to/evidence/fork-timing.log \
  --artifacts /path/to/evidence/fork-frames
```

Use a separate run and distinct outputs for stock mpv. A host without a real
audio endpoint cannot establish physical audio output; a null sink can test
AAC/PTS progression only, not a physical DAC.

Repeat in a separate executable working directory with the stock runtime bundle
and distinct log/artifact paths. Keep both bundles and outputs outside the app's
normal native/user-data tree. The probe reads an 8-bit barcode from a cached
FBO at paint stage, correlated to `frameSwapped` for each distinct mpv
rendered-output frame; it checks modulo-forward identity order and `time-pos`
progress and reports readback overhead. Sampling is observational: it does not
force repaints or alter product rendering defaults. The
2-second output/source-progress gap and 1-second audio-PTS skew are explicit
limits. It allows forward frame skips; a backward identity jump, stale identity,
source-clock stall, missing drop counters, missing/non-progressing audio PTS
when `--require-audio` is set, or early EOF fails. Strict completion also
fails if the source ends before the requested duration; an observed EOF
regression changed from an earlier 12.8-second run exiting 0 for a 30-second
request to an early-EOF failure (exit 1). Failure handling does not classify
the outcome by comparing message text. The probe's
`--frame-timing-log <path>` also sets mpv's global engine timing-log path and
writes probe-specific timing samples to `<path>.probe`. The application's
separate buffered timing log uses the same option name in the app executable.
The optional
`--artifacts` directory receives `first-frame.png` and `last-frame.png`, captured
from cached FBO samples and saved only after the probe stops. Do not save
images periodically during a run.

### Natural footage

Use `--natural-media` explicitly for a lawfully available film without the
authored frame-ID barcode. Both probes accept `--start <seconds>`; non-finite,
negative and nonnumeric offsets are rejected. A requested seek must be
observed near that position before comparisons or the aging baseline begin.
For HDR-source-to-SDR verification, keep the original movie at a lawful local
path and outside the repository; pass that path as `--hdr` (or aging `--media`)
and an owned SDR fixture as `--sdr` where required. Select `--natural-media`
explicitly, use a bounded `--start <seconds>`, and use `--fullscreen` for
visible aging runs. The HDR probe verifies source metadata, seek/readiness,
render-target/depth and policy gates; it does not certify native HDR output,
full Dolby Vision enhancement-layer use, or physical display behavior.

The HDR probe captures `auto.png`, `stable.png` and `vivid.png` at the same
paused source position/epoch, plus `sdr.png` for SDR recovery. The strict HDR
visibility check uses the settled Auto capture after post-seek frame progress;
it does not retry until a non-black frame appears.
In natural-media mode, the Stable/Vivid comparison is diagnostic: a dark frame
can legitimately fall below the authored fixture's material-difference
threshold. That threshold is not an exact pixel-equality test. Without
`--natural-media`, the fixture's visible-policy-effect assertion remains
required.

The aging probe accepts `--hdr-policy auto|stable|vivid` and optional
`--fullscreen`. It keeps source-clock, content-hash, drop-counter, EOF,
duration and optional audio-PTS checks. Natural mode disables only the
authored barcode/order contract and reports it unavailable. At least one
valid content sample is required, or two when the requested and observed
runtime permit the sampling interval and retry allowance. Playback is muted;
`--require-audio` checks an actual selected AO/clock, not audible output.

When natural-media timing evidence is enabled, center-pixel RGB/luma and
low-luma transition timestamps are diagnostic only. A dark pixel, intentional
black cut or fade is not automatically a rendering failure. Correlate
candidate timestamps with decoded source frames; sparse content hashes and
a single center pixel do not certify the absence of brief black flashes.
Full-frame hash work may be offloaded to one asynchronous job, but OpenGL
readback remains on the GUI thread; this does not itself prove that readback
or capture has no timing cost. Run-hour comparisons require comparable display
geometry and audio-device availability: a missing AO/playback device fails
`--require-audio`, and a run in a changed remote-session/display/audio state
is not a controlled load or A/V comparison.

Original films and their captured images belong outside the repository and
must not be added to the generated fixture set or public source archive.

Run HDR playback acceptance once with fixes enabled and once with the kill
switch:

```powershell
$hdr = 'C:\isolated\media\hdr_dark_gradient_1280x720_24fps_10bit_pq_hevc.mp4'
$sdr = 'C:\isolated\media\motion_frame_id_1280x720_24fps_10bit_hevc.mp4'
$env:MIKAI_HDR_FIXES = '1'
.\build\release\hdr_playback_probe.exe --hdr $hdr --sdr $sdr --fbo10
$env:MIKAI_HDR_FIXES = '0'
.\build\release\hdr_playback_probe.exe --hdr $hdr --sdr $sdr --fbo10
Remove-Item Env:MIKAI_HDR_FIXES
```

The probe requires mpv-reported PQ/10-bit metadata, the Anime4K HDR refusal
and SDR activation, distinct stable and vivid rendered results with fixes
enabled, unchanged policy output under rollback, and no OpenGL errors. With
`--fbo10`, it requests `RGB10_A2` before showing the widget and verifies the
actual attachment's RGB channel bits and mpv render depth, plus paused-PQ
samples off the 8-bit lattice. This verifies the probe's requested target,
not the normal player widget or physical HDR output. Run on a real visible
OpenGL surface, not `QT_QPA_PLATFORM=offscreen`. `MIKAI_HDR_FIXES=0` rolls
back the HDR policy changes and processing gates; it does not disable truthful
FBO-format probing.

### Current consumers

- `motion_320x180_24fps_8bit_hevc.mp4` is the clip `mikai_http_fixture` serves
  as `/media.mp4` and `/retry.mp4`, which `external_media_probe` plays back
  over HTTP; the `hls/` renditions are what `hls_recovery_probe` streams
  through the same fixture server.
- `frame_order_soak_probe` (T-401/T-210, built from
  `src/probes/player/FrameOrderSoakProbe.cpp`) replays the clip given via
  `--media`; its order-acceptance source is the frame-ID clip above. The probe
  asserts the decoded barcode from the displayed framebuffer, rendered-frame
  progress, monotonic mpv source time and per-segment drop budgets.
- No range/matrix pass-through test exists, and no code consumes the
  fullrange, 10-bit, 23.976, non-mult-32 or portrait clips' metadata cases
  today; those matrix entries are future gate inputs. When a pass-through
  test lands, this section must say so and name it.

### The cut clip's structure

`cut_saturated_black_320x180_24fps_8bit_hevc.mp4` is three concatenated
segments, so the scene detector and any interpolation path see two genuine hard
cuts:

| Time | Content | Purpose |
|---|---|---|
| 0.000–2.000 s | `testsrc2` motion, full chroma | A normal moving scene. |
| 2.000–4.000 s | saturated green, white box moving across | **Hard cut at 2.000 s** into a saturated segment. |
| 4.000–6.000 s | near-black (0x080808), dim box moving | **Hard cut at 4.000 s** into a near-black segment. |

Audio runs the whole clip: a 440 Hz tone throughout, plus an 880 Hz tone from
2.000–4.000 s, so an A/V sync check has something real to hear.

The saturated and near-black segments are not still frames — each carries a
moving box — so the clip has motion everywhere, which is what a temporal gate
needs.

### Why `testsrc2`

`testsrc2` is ffmpeg's built-in pattern with genuine inter-frame motion and
saturated colour bars. A still colour frame would make every interpolation and
temporal-cadence gate vacuous, so the fixture deliberately uses motion
throughout.

## The HLS renditions

`hls/v180`, `hls/v360` and `hls/v720` each hold a real media playlist and three
real MPEG-TS segments (six seconds, two-second target duration):

| Rung | Geometry | Depth | Codec |
|---|---|---|---|
| `v180` | 320×180 | 8 | H.264 |
| `v360` | 640×360 | 8 | H.264 |
| `v720` | 1280×720 | 10 | HEVC Main10 |

The **master playlist is not a file here**: `mikai_http_fixture` synthesises it
per request, because the signed query strings (including the deliberately
expired one) have to be attached at request time. Bandwidth in the master is
measured from the real segment sizes on disk rather than invented.

## The fixture server

CMake target `mikai_http_fixture`, built with:

```sh
cmake --build build/release --target mikai_http_fixture
```

It links only `Qt6::Core` and `Qt6::Network`, listens on `127.0.0.1`, and makes
no outbound connections. Console target: it prints its bound port and URL list
on stdout.

```sh
cmake -E make_directory .probe-out
build/release/tests/fixtures/mikai_http_fixture.exe --port 0 --facts .probe-out/header-facts.json
```

| Option | Meaning |
|---|---|
| `--port <n>` | TCP port; `0` (default) picks a free one. |
| `--port-file <f>` | Write the bound port to a file, for a parent that cannot read stdout. |
| `--facts <f>` | Write the received-header facts to JSON as requests arrive. |
| `--media-dir <d>` / `--hls-dir <d>` | Override the asset roots. |
| `--require-accept-language` | Answer `400` when a request carries no `Accept-Language`. Off by default. |

### Endpoints

| Path | Behaviour |
|---|---|
| `GET /media.mp4` | The primary 320×180 clip, byte-identical to the file in `media/`. |
| `GET /retry.mp4` | **404 on the first request, then the same bytes as `/media.mp4`.** The documented contract `external_media_probe` relies on. |
| `GET /media/<name>` | Any other generated clip, by basename. |
| `GET /hls/master.m3u8` | Adaptive master over the three real renditions. Variant URIs are **relative**, so `parseVariants()` must resolve them against the master URL. |
| `GET /hls/master.m3u8?fresh=1` | The same ladder after re-extraction — the `v360` rung comes back with a **valid** signature. This is the `QualityCache::forget()` recovery path. |
| `GET /hls/v<rung>/playlist.m3u8` | A real media playlist, served **only** when its signed query string validates. Otherwise `403`. |
| `GET /hls/v<rung>/segNNN.ts` | A real MPEG-TS segment. |
| `GET /manifest.json` | The generated matrix as JSON. |
| `GET /_facts`, `GET /_facts/summary` | The recorded header facts, and the same reduced to counts. |

### Range requests, HEAD and connection handling

`Range` supports all three single-range forms a media client sends: closed
(`bytes=100-199`), **open-ended** (`bytes=100-`, which runs to the end of the
entity) and suffix (`bytes=-50`, the last N bytes). A satisfied range answers
`206 Partial Content` with `Content-Range: bytes <first>-<last>/<size>` — the
unit is separated by a space, as RFC 7233 requires. An unsatisfiable or
malformed range answers `416` with `Content-Range: bytes */<size>`. A multi-range
spec is deliberately not honoured and gets the whole entity. `Accept-Ranges:
bytes` is always advertised, and `ETag`, `Last-Modified` and `Cache-Control:
no-store` are sent.

`HEAD` is supported alongside `GET` and **sends no body**: it reports the exact
`Content-Length` a `GET` would return, which is what a HEAD-based size probe
needs.

Each request is answered on its own connection and the response always carries
`Connection: close`. One request per connection is the boring choice that keeps
the server free of body-drain bookkeeping and any blocking read: a request body
is never consumed, and a non-`GET`/`HEAD` request is refused with `405` without
reading its body — the close discards it. The request header may still arrive
across several TCP reads; it is buffered until the blank line (capped at 64 KiB,
then `431`).

### The expired-signed-URL failure and recovery

On the default master the `v360` rung is signed with an expiry **one hour in
the past** — the stale URL a real client would still be holding in its quality
cache. Requesting that variant's playlist returns `403` with a JSON body naming
the remedy. Re-fetching `/hls/master.m3u8?fresh=1` returns the same ladder with a
live signature, and the same variant then serves `200`.

So the gate is **a visible 403 followed by a visible success — never a silent
success**. The signature is checked for real: it is derived from the rung name,
the expiry second and a fixed fixture salt, so a tampered or stale query string
fails for an actual reason rather than by convention.

### Recorded header facts

The fixture **records** what actually arrived, so a gate can assert on it rather
than assume it. For each request: sequence number, method, path, the *names* of
the query parameters, status, the `Accept-Language` value (a locale, not a
secret), whether that value is comma-free, whether a `User-Agent` was present,
whether a `Range` header was sent (with its spec), and the bytes served.

**Signature and token values are never recorded, echoed or logged** — only the
query parameter *names*. `GET /_facts/summary` reduces all of it to counts, which
is what the R8 gate (header present, and comma-free where required) reads.

Note the deliberate asymmetry on `Accept-Language`: presence is what a CDN
actually rejects, so `--require-accept-language` enforces **presence** only. The
comma-free property is recorded, not enforced, because the Qt extraction path
legitimately sends a multi-entry `Accept-Language` while mpv's
`http-header-fields` option requires the single comma-free form — enforcing both
would make the fixture unusable by one of the two real clients.

## AJI test fixtures and custom-model coverage

`IdentityOnnxFixture.hpp` constructs a genuine serialized ONNX `ModelProto` in
memory: a single `Identity` node (`X` → `Y`), FLOAT tensors with fixed shape
`[1, 3, 4, 4]`, IR version 8 and default-domain opset 19. It is intentionally a
tiny parser/staging fixture, not a useful upscaler or representative model.
The Windows-only `aji_dispatcher_fixture.cpp` exports the C ABI symbols
`aji_api_version()` (8) and `aji_rife_heavy_supported()` (1); it lets the
runtime tests exercise ABI-export-dependent setup without a production
dispatcher. On non-Windows the dispatcher target is not built, and the
custom-model staging test is skipped.

`TestAnimeJanaiRuntime` covers metadata/import identity, duplicate rejection,
content hashing, planner availability and staging of SHA-pinned ONNX bytes,
profile serialization with ordered model steps/chains, and refusal after
registered content changes or cancellation. These tests exercise validation
and file/config preparation; **they do not execute ONNX tensors, establish
operator compatibility, validate scale/architecture hints, build TensorRT
engines, run DirectML/TensorRT inference, or measure playback/throughput**.
A valid ONNX header/container and the tiny Identity graph are not evidence that
an arbitrary imported graph works on a native backend.

The test suite is isolated by the CTest launcher and uses its disposable run
directory, but native runtime/model paths still follow the executable-local
native root. Keep the test executable and its runtime DLL environment
consistent with the configured launcher (`PATH` includes the Qt binary
directory, and the Qt plugin directory and mpv DLL are passed explicitly).
For focused test work, build `mikai_tests` and invoke the configured launcher
with one suite name as its optional trailing argument. The launcher
supplies the required shaders, mpv DLL, Qt runtime/plugins and disposable
scratch parent. See the [safe PowerShell invocation](../../AGENTS.md#running-individual-test-suites). Test startup refuses an existing populated or unsafe adjacent
native tree; isolation does not make arbitrary manually launched binaries safe.

## Recorded media/server verification

The following observations record the earlier media/server fixture validation;
they are not a claim that those probes were rerun for the AJI manager change.

**The media assets are verified** — measured from the encoded files themselves,
not assumed: every clip's codec, depth, geometry, frame rate, duration, audio and
colour range; the cut clip's three segments measure distinctly (mean luma
~121 → ~148 → ~24.5), confirming both hard cuts and the saturated and near-black
segments; the primary clip measures real motion (YDIF 10–17) and real chroma
(SATAVG ~108). The three HLS renditions each have a real playlist and three
real, non-empty MPEG-TS segments.

**The server's runtime behaviour is verified against its current source.** With
the session attached, a fresh `mikai_http_fixture` instance built from this
source (`--port 0`, `--require-accept-language`) was exercised end to end:
`external_media_probe` reported `RESULT: PASS`, 21/21 — the real file-picker
acceptance, local 320×180 colored pixels, HTTP 320×180 pixels, the failed-URL
retry chip, retry recovery of the same URL, and file-URL reopen — and
`hls_recovery_probe` reported `RESULT: PASS`, 23/23 — the expired signed
variant fails visibly as 403, the cache entry is forgotten, fresh extraction
returns a valid signature, and a real 640×360 colored framebuffer renders with
the retry cleared. Each new run needs a fresh fixture: the one-shot
`/retry.mp4` 404 is per-process state (`retryServed_` re-arms on a `/media.mp4`
fetch), so re-running a probe against a fixture whose 404 was already consumed
replays bytes where the contract promises a failure.
