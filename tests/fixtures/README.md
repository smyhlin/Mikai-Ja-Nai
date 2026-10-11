# Hermetic media and HTTP/HLS fixtures (M02)

Everything a later playback, HLS, preparation or failure-recovery proof needs,
with **no network, no CDN and no API key**. The assets are generated from
ffmpeg's own `lavfi` sources, so they are project-owned and licence-clean: no
third-party footage is downloaded, copied or re-encoded.

- `generate_media.cmd` — reproduces every asset from scratch.
- `media/` — the clip matrix (10 HEVC MP4s).
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

`generate_media.cmd` rewrites the full matrix, so it is also the way to recover
a damaged or partial `media/`/`hls/`. The script is self-documenting: each
block states what the clip is for and which later gate consumes it.

## The clip matrix

| File | Geometry | Depth | Codec | fps | Duration | What it is for |
|---|---|---|---|---|---|---|
| `motion_320x180_24fps_8bit_hevc.mp4` | 320×180 | 8 | HEVC Main | 24 | 12 s | The primary clip. `external_media_probe` needs 320×180, >10 s and colourful pixels. |
| `motion_320x180_23976fps_8bit_hevc.mp4` | 320×180 | 8 | HEVC Main | 24000/1001 | 12 s | 23.976 cadence against the 24 fps twin. |
| `motion_320x180_24fps_10bit_hevc.mp4` | 320×180 | 10 | HEVC Main10 | 24 | 12 s | Bit-depth switching at the probe geometry. |
| `motion_1920x1080_24fps_8bit_hevc.mp4` | 1920×1080 | 8 | HEVC Main | 24 | 6 s | HD, limited range (tv) BT.709. |
| `motion_1920x1080_24fps_10bit_hevc.mp4` | 1920×1080 | 10 | HEVC Main10 | 24 | 6 s | HD 10-bit, limited range. |
| `motion_1920x1080_23976fps_8bit_hevc.mp4` | 1920×1080 | 8 | HEVC Main | 24000/1001 | 6 s | HD 23.976. |
| `fullrange_1920x1080_24fps_8bit_hevc.mp4` | 1920×1080 | 8 | HEVC Main | 24 | 6 s | **Full range (pc)** BT.709. Every other clip is limited range, so plan §7.4's `range`/`matrix` pass-through has both cases. |
| `cut_saturated_black_320x180_24fps_8bit_hevc.mp4` | 320×180 | 8 | HEVC Main | 24 | 6 s | Hard cut + saturated + near-black, **with AAC audio**. |
| `nonmult32_motion_1920x1072_24fps_8bit_hevc.mp4` | 1920×1072 | 8 | HEVC Main | 24 | 4 s | Height 1072 is even but **not a multiple of 32** (1072/32 = 33.5) — the dyn-HW graph case. |
| `portrait_motion_1080x1920_24fps_8bit_hevc.mp4` | 1080×1920 | 8 | HEVC Main | 24 | 4 s | Portrait rotate/fit and preparation-plan geometry. |

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

## What is verified vs. what is not

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
