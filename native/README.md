# `native/` — source pins, provenance and rights record

Everything the native (AnimeJaNai) track needs in order to be *permitted* and
*reproducible* to exist, plus the acquisition records for what has actually been
obtained, and what each acquisition does and does not prove.

**Nothing here is on the Qt/MinGW build path.** The root `CMakeLists.txt` binds
mpv **headers only** — a header-only API loader (`find_path` for `mpv/client.h`
and `mpv/render_gl.h`) — and links no mpv import library; the application
resolves `libmpv-2.dll` at run time through `LoadLibraryExW`/`GetProcAddress`.
The plain application takes no native-SDK dependency, and a machine without the
native SDK configures and builds the app unchanged.

**The Qt application carries the player-side Native surfaces but performs no
Native execution.** It owns the Native contracts and typed vocabulary
(`NativeTypes`, `NativeCatalog`, `NativeRecipe`, `NativeProfiles`,
`NativePreparationPlan`, `NativeManifest`, `NativeArtifactStore`,
`NativeSpatialAssets`, `NativeRuntimeSelection`, `NativePrefs`, `NativeMenu`,
`NativeStatus`) plus the `NativeAssetsPanel` / `NativeRifeControls` request
paths. It does not load an engine and does not run an inference chain. The
**requested** preference, the **effective** selection and every **disabled**
entry are kept apart, and anything this build cannot run is disabled with its
**named** reason (`native.disabled`, the recipe's refusal keys, the menu's
"Not available: " reason), never swapped for a silent substitute. Native
execution needs a bundle and a runtime inference path, and **neither exists
yet** — see *What exists and what does not*.

**The mpv AnimeJaNai fork builds here, and its isolated core now loads.** Its
source pin is recorded below, `patches/mpv/` holds an applicable `GET_META` diff
against it, and the pinned fork builds from this repository. Loading that core
as an isolated module **passes** once the stage closure matches the Qt/GNU
cohort — matching client/header API `131077`, mpv `v0.41.0-UNKNOWN`, and every
swap/transport/correlation case reporting `RESULT: PASS`. A full bundle
candidate is being assembled but is **not validated or installed**, and no GUI
pixel run has happened.

## Contents

| Path | What it is |
|---|---|
| `manifest/sources.json` | Pinned source identities, ABI contract, build-configuration pins, and a clearly separated block of local measurements |
| `manifest/components.json` | Every artefact a shipped bundle would contain, each with a licence and an explicit `cleared` / `blocked` / `external` decision |
| `manifest/THIRD_PARTY_NOTICES.md` | The same findings as prose, with citations, and the reasoning behind each decision |
| `manifest/videojanai-release.json` | Release-asset inventory for the VideoJaNai 2.1.0 components |
| `tools/manifest_check.cpp` | `mikai_manifest_check` — recomputes recorded digests and validates component/source/rights identity |
| `tools/native_inspect.cpp` | `native_inspect` (M11) — read-only ONNX inspection CLI over the official ONNX Runtime C API |
| `tools/bundle_tool.cpp` | `mikai_bundle` — verify, describe, manifest, list and install runtime bundles; only `verify` loads a module |
| `bundle/manifest.schema.json` | The runtime-bundle manifest schema (M05) |
| `bundle/stock.json` | Layout example: the Standard bundle — one stock mpv DLLMODULE and its closure (M05) |
| `bundle/native.json` | Layout example: the Native bundle — the fork core, the ABI-8 shim and the TensorRT/ORT closure (M05) |
| `permission-request.md` | A drafted, **unsent** request to the inference shim's copyright holder. ESC-1 is still open (`blocking`) for bundle clearance — see *Status* |
| `patches/0001-content-addressed-cache-and-prepared-only.patch` | The applicable shim diff: FNV content token in the cache key, `AJI_ENGINE_CACHE`, `AJI_PREPARED_ONLY`, and removal of foreign-engine deletion |
| `patches/0002-native-snapshot-and-control.patch` | C status ABI v2 diff: negotiated observational snapshots, completion-ticket drain, suspend/resume, and measured per-stage engine/model SHA-256; readiness remains `Unavailable` until verified prepared-plan identity exists. **Applicability + union build observed** — `strict` configure applies `0001`+`0002` (build 6.81 s) and now `0001`+`0002`+`0003` (build 23.69 s), compiling/linking `aji.dll`, `aji_trt.dll`, `mikai_native_prep`, `mikai_native_verify`, `native_abi_probe` and a new `native_prepared_runtime_probe`; ABI v2 probes exit `0` (default + real-context); native state stays `Unavailable`, `effective`/prepared empty despite legacy-active |
| `patches/0003-managed-prepared-plan.patch` | Managed prepared-plan C ABI/filter bridge; builds in the strict SDK union (`0001`+`0002`+`0003`, exit `0`, 23.69 s) but establishes no `Prepared`/`Active` or inference state; a later odd-420 crop-kernel correction is source-only/unbuilt |
| `include/aji_native.h` | C-compatible protocol/schema v2 snapshot/control declarations; prepared build keys remain absent unless independently attested |
| `tools/native_abi_probe.cpp` | SDK-independent v2 export/protocol probe with bounded-buffer and restricted-load checks for `aji.dll` |
| `patches/mpv/0001-get-meta-mikai-native.patch` | Applicable pinned-fork schema/protocol-1 Probe producer: typed cached snapshots, actual hardware-context device facts and lifecycle epochs; private non-copy NVDEC smoke passes 14/14, not Native activation |
| `patches/notes/` | The superseded three-part patch **specification**. Records what was required; not applied and not a build input |
| `tools/mikai_native_prep.cpp` | `mikai_native_prep` (M15) — out-of-band preparation CLI that builds a plan **in-process** through the TensorRT builder API |
| `tools/mikai_native_verify.cpp` | `mikai_native_verify` — binds a real engine on the GPU and compares it to a CPU reference against fixed tolerances |
| `sdk-acquisition.md` | How the MSVC/CUDA/TensorRT/ONNX Runtime/DirectML/Windows SDK trees were obtained, and what was proven with them |
| `spatial-acquisition.md` | The five spatial ONNX graphs: source pin, hashes, licence, and the CPU reference observation |
| `rife-acquisition.md` | The four RIFE v4.14 graphs: archive pin, extraction, structural check, weight-data comparison, rights position |
| `videojanai-acquisition.md` | VideoJaNai 2.1.0 component archives: digests, roles and terms evidence |

M04's `CMakeLists.txt` and `build-msvc.md` are real: an optional MSVC + nvcc
project that stages the pinned read-only source, applies `patches/` to the
staged copy and builds `aji`, `aji_trt`, `aji_dml` and the
`tools/mikai_native_prep` CLI. `patches/0001-content-addressed-cache-and-prepared-only.patch`
and `patches/0002-native-snapshot-and-control.patch` both apply under `strict`
configure, and the strict native SDK union build **compiled/linked** `aji.dll`,
`aji_trt.dll`, `mikai_native_prep.exe`, `mikai_native_verify.exe` and
`native_abi_probe.exe` (0001+0002, 6.81 s; with `0003` added, 23.69 s). The ABI
v2 probes then ran exit `0`;
`tools/native_abi_probe` is the opt-in, SDK-independent ABI v2 export/protocol
probe. The earlier three-part specification survives in
`patches/notes/` and is not a build input, and `patches/mpv/` holds
the separate fork `GET_META` diff. The build refuses to configure unpatched by
default; see `build-msvc.md`. `bundle/` carries the manifest schema and the two
layout examples (M05).
Native ABI v2 snapshots report engine/model measurements for diagnostics only.
They do not establish a verified prepared-plan identity: this shim has no
validated prepared-plan attachment producer or native controller/preflight.
Therefore configured legacy and resize-only chains remain `Unavailable`, with
no effective slot/plan or Prepared/Active claim. Drain and resource
suspend/resume remain real diagnostic operations, not readiness attestations.

## Scope of use and licence basis

**Owner grant, 2026-10-04 (in-session).** The project owner grants
**unrestricted OpenSource use of this project's own native integration code, its
patch series and its recorded decisions** (recorded in
`manifest/components.json#projectIntegrationGrant`). This is the owner's grant
over code this project owns; it does **not** relicense, license or verify any
third-party binary or source. The repository's root `LICENSE` is **The Unlicense**,
covering this project's own code only; third-party components are excluded from
it. Every third-party component keeps its own per-component licence and decision
below.

The third-party components still carry their own terms, and the earlier recorded
scope for the project was **private, local and non-commercial**, which remains
the basis for the components whose own terms are non-commercial or unverified.
This section is recorded because several findings turn on the difference between
*using* a component and *redistributing* it.

- **CC BY-NC-SA 4.0 — the spatial upscaler weights.** §2(a)(1) licenses
  reproduction and sharing "for NonCommercial purposes only", and §1(k) defines
  NonCommercial as "not primarily intended for or directed towards commercial
  advantage or monetary compensation". §3 adds attribution, a warranty
  disclaimer and ShareAlike, which engage only on a Share. Local non-commercial
  use stays within the located grant, but **redistribution is not cleared**:
  ESC-2 is `blocking`, and no recorded decision places the five graphs into a
  distributed bundle. The §3 conditions remain obligations if a Share ever
  occurs. This is not a claim that CC BY-NC-SA is permissive.
- **The inference shim's upstream terms are not verified.**
  `the-database/animejanai-inference` declares no `LICENSE`, no SPDX identifier
  and no per-file headers, and its GitHub licence field is `null`; no upstream
  licence text has been located and no upstream grant has been obtained. This is
  a **third-party** component and is **not** relabelled OpenSource by the
  project's own owner grant. The project records its decision to use it
  (ESC-1), and the drafted request in `permission-request.md` remains unsent; a
  shipped notice must not claim an MIT licence from upstream.

## Status: ESC-1/2/3 are open (blocking); ESC-4/ESC-5 are resolved

ESC-1, ESC-2 and ESC-3 carry `severity: blocking` in
`manifest/components.json`; ESC-4 is `RESOLVED` (licence obtained) and ESC-5 is
`RESOLVED` (architecture — `trtexec` is not shipped); ESC-6 is advisory. The
2026-10-04 owner grant is recorded at
`components.json#projectIntegrationGrant` and covers this project's own
integration code, patch series and recorded decisions only. Read the findings,
resolutions, `scopeNote`/`noticeBasis` and `residualAccepted` fields there
rather than a summary here.

These points must not be lost in that summary:

- **The owner grant is not an upstream grant.** The inference repository still
  declares no licence and its licence field is still `null`; the third-party
  shim's upstream terms are **not verified**. The 2026-10-04 grant is the basis
  for this project's own integration only, and every shipped notice has to say
  so in those terms.
- **The owner grant is not a capability.** It does not build a bundle, produce a
  runtime core, or run one frame of native inference.
- **Third-party redistribution is not cleared by the owner grant.** The
  2026-10-04 owner grant covers this project's own integration code, not any
  third-party binary or source. For the shim the upstream terms are **not
  verified**, so a **shipped** Native bundle that carries it has no
  redistribution clearance and none is claimed. The draft
  [permission request](./permission-request.md) remains unsent, and no
  redistributable bundle is assembled or promised.

### What exists and what does not

| Piece | State |
|---|---|
| Shim source at the pinned commit | pinned, digest-verified, staged and patched by `native/CMakeLists.txt` |
| Shim patch | one applicable diff applied under `AJI_PATCH_POLICY=strict`; **not** the full M14 SHA-256 `buildKey` integration |
| mpv-fork `GET_META` patch | current typed Probe diff applies against the immutable pin; current owned fork compiles and private real-CUDA lifecycle smoke passes 14/14 (`build-msvc.md`) |
| The pinned mpv fork core | **built** here from the pinned source (273 steps, Release) |
| That core loaded as an isolated module | **passes** (exit `0`) once the stage closure matches the Qt/GNU cohort, with matching client/header API `131077` and mpv `v0.41.0-UNKNOWN`; swap refusal, transport, correlation, `pending 0` and error-end-file all `RESULT: PASS` |
| The original `Win127` load failure | **UNRESOLVED** as to the missing symbol; completing the stage closure fixed this scenario, but no particular missing export has been identified |
| Preparation helper `mikai_native_prep` | built and working; builds a plan **in-process** through the TensorRT builder API — no `--trtexec`, no child process |
| TensorRT components the app needs (per machine) | **provisioned, not shipped** — the AnimeJaNai 3.6.3 packs are pinned by asset byte count and SHA-256; the app reads the compute capability from the driver's NVML, selects the exact `trt-sm{MA}{MI}` pack (or `trt-ptx` when the release publishes none for that generation), downloads the missing set on this machine, proves every file by digest into the app-owned store, and uses the bundle's own resource as-is when the bundle already declares it. With the GPU's own pack a real build succeeds; with another generation's it fails naming the missing resource. Evidence: `native_component_probe` `--print`/`--fetch`/`--from-sdk`. This closes the sm120-only bundle gap by provisioning. It redistributes nothing |
| Spatial raw engine | **produced** (1 380 548 bytes, `input:1x3x64x64`, RTX 5090 `GPU-f76e436f-…`, exit `0`); the source graph's digest was unchanged by that run |
| Spatial CPU reference export | **passes** (fp16 in `1x3x64x64` → out `1x3x128x128`, declared output `N=-1` retained); a non-finite output is refused with `reference.output.nonFinite` |
| RIFE CPU references, all four graphs | **pass** on the original graphs, using one genuine 64×64 PPM pair with the source 11-channel packing and `t = 0.5`; 5 451 samples differ between the two frames |
| RIFE raw engines, all four | **serialized** through the real TensorRT builder API, outside any playback path |
| `mikai_native_verify`, **RIFE lite** | **passes** (exit `0`) — `maxAbsoluteError` `0.0122070312`, RMSE `0.000512728931`, both inside the unchanged `0.02`/`0.005` bounds, no overrides |
| `mikai_native_verify`, **RIFE lite ensemble** | **passes** (exit `0`) — `maxAbsoluteError` `0.00341796875`, RMSE `0.000227462237` |
| `mikai_native_verify`, **RIFE full** | **rejected** — RMSE `0.00198529105` inside bound, `maxAbsoluteError` `0.0561523438` over `0.02`. New O3 current flags build `13003908` bytes and verify fails (exit `9`): `maxAbsoluteError` `0.0385742188`, RMSE `0.00104668487`, 8 outside |
| `mikai_native_verify`, **RIFE ensemble** | **rejected** (exit `9`) — RMSE `0.00104451097`, `maxAbsoluteError` `0.0358886719` over `0.02`. New O3 current flags build `13549972` bytes and verify fails: `maxAbsoluteError` `0.0244140625`, RMSE `0.000785205742`, 4 outside |
| `mikai_native_verify`, spatial HD Performance | **verifies** — same unchanged engine (`sha256 65918177…`) on the unchanged original graph and corrected planar reference: `maxAbsoluteError` `0.00610351562`, RMSE `0.000591099329`, `0` outside tolerance, bounds unchanged. The earlier `0.02734375` rejection (malformed interleaved tensor) is historical |
| What a RIFE pass covers | **raw engine bytes, bindings and numerics only**: `manifestVerified: false` and `prepared: false`. It is **not** `Ready` or `Active` |
| Diagnosis of the rejected engines | **unresolved**; cross-engine FP16 math is the working hypothesis, not a conclusion. The unchanged bounds are not to be widened to make a rejection pass |
| CPU optimisation diagnostic | on the same original spatial graph and input, `DISABLE_ALL` vs `ENABLE_ALL` measured RMSE `8.5556432276e-06` — provider optimisation is **not** the cause. The earlier GPU-vs-`DISABLE_ALL` difference (`maxAbsoluteError` `0.02734375`) was measured on the malformed interleaved tensor and is historical; on the corrected planar reference the engine verifies |
| `native_prep_probe` over the real in-process helper | seven behaviours pass (see `build-msvc.md`) |
| Earlier self-built bundle candidate verify | **passed** after static export classification and isolated per-dependency loads: 270 files, 139 DLLs, 138 restricted-search child-process dependency loads, one mpv module accepting `@mikai_native`. Evidence: `.probe-out/bundle-isolated-dll-load-verify-stdout.log`. This is **not** M05 completion, not prepared inference, and not acceptance of the newly selected author release |
| Native bundle | **`0.1.0-dev.2` installed and load-verified**. `current-native` points at `native-0.1.0-dev.2`; `.1` remains for rollback. Installed verify exit `0`: 83 files, client API `131077`, core accepts `@mikai_native` (`.probe-out/native-crt-closure-verify-installed.log`). Current `mpv_surface_smoke` also passes through this exact installed module with processing off: inspected 1280×720 framebuffer, 921600/921600 non-black samples, 297305/921600 motion, 24 fps, zero decoder/VO drops, zero live filters (`.probe-out/native-bundle-dev2-off-surface.log` and `.png`). Not startup discovery, prepared readiness or inference. Stored runtime choice was not written. The old `.2` closure that failed `Win32 127` is superseded |
| Native-off GUI surface through the owned runtime | **passes** (exit `0`) after full candidate hash validation: the Native-kind candidate is explicitly selected as `0.41.0-native.1`, a real 320×180 HEVC 24 fps video plays with `contextReady` true, position `0.542 → 2.542` over a 12 s duration, **921600/921600** non-black samples and **290264/921600** motion, **0** decoder and **0** VO drops, and **0** live `vf` chain entries with Native off. The inspected `mpv_surface_smoke.png` (1280×720) is a visibly colored motion test pattern |
| Widget/lifecycle probe on that candidate | `widget_pixel_probe` **PASS 21/21**, 0 skips, exit `0`; inspected 640×360 PNG is colored; GL 4.6 with 32 live GL-logger messages, 0 `ErrorType`, `glGetError` 0, motion 30.44%. Late events after close remain unknown and the 256×144 15 fps reopen refreshes metrics and the `vf`-absence check. Descriptor digest changed intentionally (NOTICE); the module core digest is unchanged |
| What those surface proofs cover | **real Qt/OpenGL through the owned native runtime with Native off** — they are **not** the native filter, **not** CUDA interop, **not** active inference and **not** task completion. Nothing is `Prepared` or `Ready` |
| Qt preparation runner, GPU work lease, recipe publication | **do not exist yet** |
| M12 (RIFE assets) | **partially satisfied, not done**; two RIFE engines (lite, lite-ensemble) verify numerically; full and ensemble are rejected; models are still not `Prepared` and not `Ready` |
| `Prepared` / `Active` / `Ready` for any engine | **not reached** (two RIFE engines verify numerically but are `manifestVerified: false`, `prepared: false`) |
| DirectML → Qt WGL interop | **actual probe, honest refusal** — on an attached session (`3.3.0 NVIDIA 616.92`) the legacy `SHARED` path imports the D3D11 texture and blits the Qt framebuffer with exact RGBA corner pixels (`d3dTextureImported:true`, `framebufferBlit:true`, PNG written); the **required** `SHARED\|SHARED_NTHANDLE` registration fails `wglDXRegisterObjectNV` `0xC007006E` → `CAPABILITY_UNAVAILABLE`, exit `2`, `directMlMayBeEnabled:false`, no CPU fallback. DirectML stays disabled with that factual reason; where offered this is an honest refusal, not an NN proof. Evidence: `.probe-out/wave-native_dml_interop_probe.log` |
| ABI v2 export + probes | **observed** — strict SDK union (0001+0002+0003) compiled/linked the shim tools plus a new `native_prepared_runtime_probe` (exit `0`, 23.69 s, parent session bg235; a later odd-420 crop-kernel correction is source-only/unbuilt); `native_abi_probe` exit `0` on default (ABI 8 unchanged, protocol/schema 2/2, refusals `Unavailable`, canaries untouched) and on a real context (UUID `GPU-f76e436f-…`, resize-only `64x64 -> 96x96`, ticket 1 completed, `drain=0 suspend=0 resume=0`, post-resume frame completion) — native state stays `Unavailable`, `effective`/prepared empty despite legacy-active. No NN fidelity/admission/`Prepared`/`Active`. Evidence: `.probe-out/native-abi-v2-default-union.log`, `.probe-out/native-abi-v2-real-context-union.log` |
| Original HD owned import (M11) | **exit `0`** — `modelId user:532fe8…`, declared output `N=-1` preserved, CPU run `N=1` `320x180 -> 640x360`, eligibility `eligible (verified reference)`, `published:true`; inspection only, not an engine and not prepared. Evidence: `.probe-out/native-builder-original-spatial-import.log` |
| Producer fork cadence build | **compiles exit `0`** with `av_d2q` and observed rotation/crop/color facts; updated 17780-byte `patches/mpv/0001-get-meta-mikai-native.patch` exported against base `63d127174d…` |

No binary has been vendored into the repository.

## User-selected author runtime baseline

The native integration base is the exact author release
[`2026-09-17-63d127174d`](https://github.com/the-database/mpv-winbuild/releases/tag/2026-09-17-63d127174d),
acquired independently into
`F:/ZKS/mikai-native-sdks/mpv-author-2026-09-17/`. Both the requested
`mpv-x86_64-20260917-git-63d127174d.7z` and its matching
`mpv-dev-x86_64-20260917-git-63d127174d.7z` match GitHub's published SHA-256
digests; `manifest/sources.json#mpv-author-winbuild` pins the archives.
The workflow commit is `e44cfd8af3eb0aa41b3740f66be4e5a1fcf4171e`,
not the release-tag commit. The exact resolved custom-libass commit is still
unestablished.

Observed against those unmodified release binaries:

- `mpv.exe` plays the real 320×180 HEVC motion fixture through NVDEC and
  OpenGL, exits `0`, and produces an inspected coloured window capture.
  It exposes `animejanai`, `sub-ass-render-threads`, and `sub-gpu-blur`.
- `mpv_loader_probe` passes with client/header API `131077`.
- `mpv_surface_smoke` reports `RESULT: PASS` through the existing
  Qt/OpenGL surface: real moving pixels, no decoder/VO drops in the sampled
  interval, and no native filter in the live chain.
- `widget_pixel_probe` passes **21/21**, exits `0`, and has no skips:
  source replacement and close/reopen refresh cadence, geometry, drop counters,
  and native-filter absence. Its inspected 320×180 framebuffer is coloured,
  with no GL `ErrorType` or `glGetError` errors. These are native-off rendering
  and lifecycle proofs, not inference or performance comparisons.

- `interaction_probe` passes **149/149**: real quality changes, Anime4K
  pixel effect above a stable zero-noise control, independent PiP controls,
  restore/re-entry, and player/float teardown.
- `compare_probe` passes **30/30**: frozen-frame shader comparison changes
  the picture and restores the prior chain; a later capture follows the new
  playback position.

Evidence: `.probe-out/author-mpv-playback.log`,
`.probe-out/author-libmpv-loader.log`,
`.probe-out/author-libmpv-qt-surface.log`,
`.probe-out/author-libmpv-widget.log`,
`.probe-out/author-libmpv-interaction.log`, and
`.probe-out/author-libmpv-compare.log`. The Qt smoke descriptor is a
Standard-capability baseline with protocol/engine schema `0/0`, because
the original release does not implement this project's native status protocol
or prepared-only patch. It does not declare those missing capabilities merely
because the author filter is compiled in.

This supersedes the broad MSYS2 candidate as the selected runtime baseline.
That earlier patched candidate remains a separate experiment; its proofs do
not transfer to the author release. `mpv.exe` is a smoke target, not a Mikai
external-player dependency. No application runtime preference or safe default
was changed. No engine is made `Prepared` or `Active` by these runs, and
complete corresponding source, licence closure, and patched native protocol
acceptance remain required before distribution.

## What is acquired, and what each acquisition does and does not prove

Acquisition is not clearance, and none of the records below is a readiness
claim. Every tree lives outside the repository and outside every tracked source
tree, so nothing can be committed or accidentally vendored.

- **mpv fork core** — licence cleared (GPL-2.0-or-later or LGPL-2.1-or-later per
  build options, see ESC-6), and **built by this repository** from the pinned
  commit. The root CMake still binds only its headers, so this is a separate,
  opt-in build. The built core loads in isolation once the stage closure matches
  the Qt/GNU cohort; the earlier `Win127` failure is explained as a closure
  mismatch in that scenario, not by an identified missing export. The
  corresponding-source, licence-text and relinkability obligations attach to the
  build that actually exists.
- **Stock core** — external and untouched. The application provisions plain mpv
  at run time from `shinchiro/mpv-winbuild-cmake`; M03 does not vendor it.
- **Native SDKs — acquired, isolated.** Official Windows SDK, CUDA, TensorRT,
  ONNX Runtime and DirectML packages were obtained into
  **`F:\ZKS\mikai-native-sdks\`** with no admin rights, no installer, no driver
  change and no system-wide install, replacing the empty system CUDA stub and
  the Conda TensorRT residue the earlier bounded search had rejected. The tree
  holds the pinned CUDA flavour **13.4** (never inferred from the TensorRT
  version) and TensorRT **11.3.0.99**. A standalone MSVC + CUDA compile, link
  and SM 12.0 kernel execution passed against it. Full layout, URLs, digests and
  terms: **`sdk-acquisition.md`**. That is a toolchain and CUDA-kernel result
  only — no inference chain, playback path or framebuffer result is claimed
  from it. A later build drove the in-process TensorRT builder on a real GPU
  through that helper (see *What exists and what does not*), which is a further
  step, not a playback result.
- **Five spatial ONNX graphs — acquired and CPU-reference verified.** Downloaded
  anonymously from `the-database/mpv-AnimeJaNai` at pinned commit
  `d9cbaae5e92a56e71fdf4da5e063ae3e2611e5f7` (tag 3.6.3), digest- and
  size-matched to the manifest. Read once each through `native_inspect`: every
  graph declares rank-4 FP16 input and output with `C=3`, and one **CPU
  reference** run per graph produced concrete `N=1`/`C=3` at both ends — the HD
  declaration's symbolic output batch `N=-1` is **preserved, never clamped**,
  with the concrete `N=1` living only in the separate verified-reference
  profile. This is a CPU execution-provider run: not a GPU/DirectML/TensorRT
  binding, not an engine build, not activation. Hashes, boundary and rights:
  **`spatial-acquisition.md`**.
- **Four RIFE v4.14 graphs — acquired and weight-data traced.** Extracted from
  the pinned `rife-fp16-1.7z` release (published SHA-256 reproduced exactly):
  `rife_v4.14`, `rife_v4.14_lite`, `rife_v4.14_ensemble`,
  `rife_v4.14_lite_ensemble` — the full {Full, Lite} × {ensemble on/off} space.
  Each graph was walked as a protobuf: 128/128 float16 initializers and the
  documented fp32 GridSample island are present in the bytes. The author's own
  checkpoints were then compared tensor by tensor: **128/128 names and shapes
  matched with FP32→FP16 round-to-nearest-even bytes identical** in all four
  comparisons, so the stored weights *are* the author's MIT v4.14 / v4.14.lite
  weights. Exporter identity, the non-weight graph material and whole-graph
  redistribution are the residual findings; the owner has determined they are
  acceptable for this project (ESC-3 — see
  `components.json#components[id=models.rife-v4.14].clearanceBasis`), which
  accepts the residual rather than establishing facts about it. Archive pin,
  extraction, comparison report and rights: **`rife-acquisition.md`**.
  Since acquisition, all four graphs have run a **real CPU reference** (one
  genuine 64×64 PPM pair, the source 11-channel packing, `t = 0.5`) and all four
  were **serialized through the real TensorRT builder API**. The verifier splits
  the matrix: **lite** and **lite ensemble** pass numerically; **full** and
  **ensemble** are **rejected** on `maxAbsoluteError`. A pass covers raw engine
  bytes, bindings and numerics only — `manifestVerified: false`,
  `prepared: false` — so no model is `Ready` or `Active` and M12 is not done.
  See *What exists and what does not*.
- **VideoJaNai 2.1.0 components — acquired as evidence only.** Six assets
  (manifest, packs, TRT runtime/SM120/PTX, RIFE) with independent SHA-256
  recomputation; no component was installed, executed, bundled or used to
  replace a driver. That acquisition located no source/binary grant for the aji
  source or binaries, and that upstream fact still stands; the project records
  its decision to use them, and the shim's upstream terms are **not verified**
  (ESC-1 stays `blocking` for bundle clearance). See
  `videojanai-acquisition.md`.
- **ONNX Runtime 1.24.4 (DirectML) and DirectML 1.15.4** — cleared, MIT /
  Microsoft Software License Terms respectively; both packages carry their
  licence files, and DirectML's `LICENSE.txt` §1(a) expressly permits
  distributing the runtime in Windows/Xbox ML applications. Terms and acquisition
  detail: `sdk-acquisition.md`; component decisions: `manifest/components.json`.
- **TensorRT 11.3.0.99 runtime and the CUDA `cudart` redistributable** —
  `external`: distributable at bundle-assembly time under the SLA's §1.2.1.x
  conditions, with the notice *"This software contains source code provided by
  NVIDIA Corporation."* and NVIDIA's `Acknowledgements.txt` verbatim. The
  package was acquired (anonymous 302 → 200, `application/zip`,
  1 985 279 435 bytes). `trtexec.exe` is **not used at all** on this path: the
  preparation helper builds in-process through the TensorRT builder API and
  launches no external executable. That is what ESC-5's architecture resolution
  rests on, and it holds with room to spare — the developer tool is neither a
  build-time input nor a run-time dependency.
- **Anime4K shaders** — MIT (47 files) and Unlicense (2 files), read directly
  from the files. Out of native scope, so no `components.json` row.

## Pins that matter

| Pin | Value |
|---|---|
| mpv fork | `63d127174d8f70336c82a22408929e2bc301edcb` (signed, 2026-09-17) — current owned patched fork built and Probe-exercised privately; not a root CMake build target or shipped Native bundle |
| inference shim | `738ce3b3020b3950c48ce3122a08f32af531d4fa` — **verified**: all nine key source files hash identically to upstream at this commit. Upstream declares no licence; adopted on the owner's determination (ESC-1) |
| patch series | `patches/0001-content-addressed-cache-and-prepared-only.patch` then `patches/0002-native-snapshot-and-control.patch` applied; `patches/notes/` and `patches/mpv/` not applied by the shim build |
| C ABI | version **8**, 17 exports, 3 of them optional for older callers |
| `AJI_NVINFER` | `nvinfer_11` — the default is `nvinfer_10` and TensorRT 10 is **not** a dependency |
| CUDA architectures | `75-real;80-real;86-real;89-real;90-real;100-real;120-real;120-virtual` |
| CUDA flavour | **13.4** — never inferred from the TensorRT version |
| TensorRT | 11.3.0.99 |
| ONNX Runtime / DirectML | 1.24.4 / 1.15.4 (1.24.x is the DirectML ceiling) |
| Spatial graphs | `the-database/mpv-AnimeJaNai` commit `d9cbaae5…` (tag 3.6.3) — see `spatial-acquisition.md` |
| RIFE graphs | `animejanai-inference` release `models-rife-fp16-1` — see `rife-acquisition.md` |

The CUDA architecture list must always be passed explicitly: the upstream
CMakeLists fallback is guarded by `if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)`
and CMake pre-defines that variable during `project()`, so the fallback never
fires and omitting it yields a single-arch binary.

The upstream build record (`docs/BUILD-WINDOWS.md`) describes a *different*
machine (VS 18 Community, toolset 14.51.36231, CUDA 13.3). It is a record of the
author's environment, not of this one.

## Observations are not pins

`plan.md` §3.2 lists hashes for the stock core, the fork core, `aji.dll` and
`aji_trt.dll`. Those, and everything else in the `observations` block of
`sources.json`, were measured from pre-existing files that **this repository did
not build**. No source→binary reproduction has been proved. They are descriptive
evidence about files on a machine, and must never be used as a release pin,
referenced by a bundle manifest, or treated as a download verification.

They become pins only when this repository builds the artefact itself, the build
is reproducible, and the digest is recorded from that build.

## Tooling

**`mikai_manifest_check`** recomputes the SHA-256 of every file whose digest the
manifests record, and validates that each component's
`sourceIdentity.sourceId` resolves to a source and that `redistribution` is one
of the three allowed values. Exit codes: `0` clean, `1` invalid identity or hash
mismatch, `2` digest inventory or source root not established, `3` a component
is `blocked`. It loads no DLL and proves nothing about runtime behaviour.

It was built through the root CMake entry and executed against the declared
read-only inference and spatial-model roots supplied at the time. Observed:
**exit 3**, with **14 recorded file digests recomputed and matched** and
unestablished inventory for the sources that were not present then (`mpv-fork`,
`mpv-upstream-stock-build`, `tensorrt-11.3-sdk`, `cuda-toolkit`,
`onnxruntime-directml`, `directml`, `rife-v4.14-assets`, `toolchain.msvc`,
`ffmpeg-dev`). It proves the recorded digests of the files that were present
match, and nothing about runtime behaviour or bundle assembly.

That exit 3 is **historical**. A current-tree checker run against all declared
digest inventories now exits **0**: **19** file digests match, including both
author-runtime archives, and **18** observation digests are format-checked
without being treated as pins. The checker was rebuilt after replacing an
invalid `QJsonValue::toStringList()` call with direct JSON-array comparison.
A mismatched architecture list is rejected with exit `1`.
Evidence: `.probe-out/author-runtime-full-manifest-check.log`.
This checks declared source/component identities, not undeclared dependency
source, bundle redistribution readiness, or prepared inference.

The current-tree `ctest -R mikai_tests` run also passes (14.65 s test time);
the live oracle remains disabled. No linter is configured.
Evidence: `.probe-out/author-runtime-ctest.log`.

**`native_inspect`** (M11) is the read-only ONNX inspection CLI. It uses the
official ONNX Runtime C API to read a graph's declared I/O and, with
`--resolve WxH`, run one **CPU reference** at that geometry. It is CPU-only and
touches no engine path; the five-graph observation above is its result. It is
also how the exercised graph was pinned: `--reference-dir` exports one real CPU
reference — input bytes, output bytes and a manifest into a new directory — at
fp16 `1x3x64x64` → `1x3x128x128`, with the declared symbolic output `N=-1`
retained rather than clamped. A reference whose output holds a NaN or an
infinity is refused with `reference.output.nonFinite`.

**`mikai_native_prep`** (M15) is the out-of-band preparation CLI built by
`native/`, described with its invariants and exercised scope in
`build-msvc.md`. It builds a plan **in-process** through the TensorRT builder
API — no `--trtexec`, no child process — for one graph, one shape set and one
named CUDA device, and moves raw plan bytes onto `--out`. **A raw plan is not a
prepared plan**: it has not been verified, published to a manifest or an
artifact store, and it is not `Prepared` or `Active`.

**`mikai_native_verify`** binds a real engine, enqueues it on the real GPU and
compares against the CPU reference. On the corrected planar reference the
spatial HD Performance engine now **verifies** (`maxAbsoluteError`
`0.00610351562`, RMSE `0.000591099329`, `0` outside tolerance, bounds
unchanged); RIFE full and ensemble still reject. The `0.02`/`0.005` bounds are
not to be widened to make a rejection pass, and a verified engine is still
`prepared: false`, not `Prepared`.

**`mikai_bundle`** verifies, describes, lists and installs runtime bundles.

`verify` is the acceptance instrument for a bundle and it loads. Under the same
restricted search the application uses — the module's own directory, the
bundle's declared dependency directories and System32, with the process `PATH`
never read — it loads **every** declared DLL, then refuses the bundle unless
exactly one of them is an mpv module, that module is the one the manifest names,
its reported client API matches the declared one (when one is declared), and,
for a bundle labelled `native`, the core itself accepts `@mikai_native`. That
last check asks the core by name and nothing else: a handle is created, given
`config=no`, asked to take the filter, and destroyed uninitialized, so no window,
no video and no audio ever exist. A core without the filter refuses the option,
which is exactly the incoherent case the manifest would otherwise hide. What a
passing `verify` still does not prove is playback, a prepared engine or a real
frame.

`describe`, `list` and `manifest` load nothing, which is what makes them usable
on a machine with no core present. `manifest` reads the layout a manifest
declares without the payload it names, and it is deliberately not a weaker
`verify`: every check that needs bytes needs bytes.

**The two `bundle/*.json` files are layout examples, not bundles.** No binary is
vendored here, so every file record carries `bytes: 0` and the reserved
sixty-four-zero placeholder digest — well-formed under the schema so the
examples parse, and a value no file has ever hashed to, so an example can never
be mistaken for a measured bundle. `verify` recomputes every digest and
therefore refuses both until M04's real build output is assembled beside a
manifest. Reading their layout:

```text
mikai_bundle manifest native/bundle/native.json
```

The Standard example is the single stock `libmpv-2.dll` the app already
provisions: at the recorded ~120 MB the FFmpeg closure is statically linked
into it, so it has no companion DLLs to declare. The Native example adds the
fork core, the three ABI-8 shim DLLs, and the TensorRT/CUDA and
ONNX Runtime/DirectML closures in `trt/` and `ort/` — the layout that removes
the `AddDllDirectory`-into-an-existing-TensorRT-folder dependency the research
harness needed, and that leaves nothing to Conda, torch, Python or any other
ambient install.

## Root CMake and the optional inspector

The root build is deliberately free of any native-SDK dependency. The relevant
pieces:

- **mpv**: `MIKAI_MPV_HEADERS` is a header-only `find_path` (guarded by a
  `FATAL_ERROR` if `mpv/render_gl.h` is missing). No mpv import library is
  linked; the header-only loader resolves the DLL at run time.
- **`MIKAI_WITH_MODEL_INSPECTION`** (`option`, default **OFF**): when ON it
  `find_path`s `onnxruntime_c_api.h` under **`-DORT_INCLUDE_DIR`** and adds the
  `mikai_model_inspection` static library and the `native_inspect` executable
  (plus `TestNativeCatalog`). This is **real CPU-only ONNX Runtime inference** —
  it is not a GPU/DirectML/TensorRT binding and produces no engine.
- Everything else the Qt app builds (`mikai_native_support`, `mikai_player`,
  `mikai_backend`, probes, tests) needs no native SDK.

## Verification status
The per-suite counts in this section are **historical** isolated-build ledgers.
The latest complete CPU matrices are green: inspection-ON exit `0` in **21.48 s**
and inspection-OFF exit `0` in **15.92 s**
(`.probe-out/native-batch-on-epoch-guard-ctest.log`,
`.probe-out/native-batch-off-epoch-guard-ctest.log`); an earlier settled-source
run (ON `20.55 s` / OFF `15.37 s`) predates the later MinGW-CRT/source updates.
A genuine prepared CAPI first-snapshot regression now passes **all 7 previously
failing fields before output** (state `Prepared`, not `Active`; real CUDA NN
maxAbs `0.0034044981`, RMSE `0.000446848262`) — a **DIRECT CAPI/CUDA** proof, not
a Qt-framebuffer proof (`.probe-out/native-batch-prepared-snapshot-proof.log`).


- **Fresh isolated build with the inspector OFF (default, no native SDK):**
  `mikai` and `mikai_tests` build (exit 0) and the C++ ctest entry passes in
  **14.82 s** (14.83 s wall, hermetic; 24 suites PASS and `LiveOracle` still
  skips by design). A real Qt/OpenGL **640×360** colored moving frame
  (`testsrc2` + motion) rendered through the explicitly loaded module with the
  GL debug logger active: **32 messages, no `ErrorType` message, `glGetError`
  0**. Perf-warning messages remain, so this is not a warning-free run.
- **Current-tree import inspection is clean.** `objdump -p` on `mikai.exe` from
  both the default/OFF and review/ON builds lists only `Qt6Core`, `Qt6Gui`,
  `Qt6Network`, `Qt6OpenGLWidgets`, `Qt6Widgets`, `KERNEL32`, `msvcrt` and
  `SHELL32` — no mpv, ONNX Runtime, CUDA, TensorRT or DirectML DLL. mpv is
  resolved at run time and nothing native is on the link path.
- **Optional inspector ON:** the isolated configuration
  (`-DMIKAI_WITH_MODEL_INSPECTION=ON -DORT_INCLUDE_DIR=…`) builds the app,
  tests and the listed probes/helpers, and its ctest run passes in **15.17 s**
  (25 suites PASS, `LiveOracle` SKIP).
- **The engine builder works in-process; the spatial engine now verifies on the
  corrected reference.** A Release build of the native tools and of the Qt tree
  both succeeded. `native_prep_probe` passes seven behaviours against the real
  in-process helper (existing `--out` refused and unchanged; a corrupt graph
  exits `5` leaving no engine-shaped file; an absent GPU UUID refuses rather than
  falling back to another adapter; `build-started` is emitted before the build
  proceeds; the TF32-off/strongly-typed precision gate holds; cancelling
  mid-build publishes nothing; failure and cancellation leave prior artifacts and
  source bytes unchanged). A real 64×64 build on the selected RTX 5090 exited `0`
  and wrote **1 380 548 bytes** of raw plan, with the source graph's digest
  unchanged. `native_inspect --reference-dir` exports the matching CPU reference
  successfully and refuses a non-finite output with `reference.output.nonFinite`.
  `mikai_native_verify` now **verifies** that same unchanged engine
  (`sha256 65918177…`) against the **corrected planar** reference on the
  unchanged original graph: `maxAbsoluteError` `0.00610351562`, RMSE
  `0.000591099329`, `0` outside tolerance, bounds unchanged
  (`.probe-out/numerical-boundary-spatial-verify.log`, exit `0`). The earlier
  `maxAbsoluteError` `0.02734375` rejection was measured against the malformed
  interleaved tensor and is **historical for this engine/input boundary**. RIFE
  full and ensemble remain rejected (see below). **No engine is published,
  `Prepared` or `Active`; no frame of native inference has been rendered; no
  bundle has been assembled; and `mikai_manifest_check` still loads no DLL.**
- **Two RIFE engines verify numerically; two do not.** All four original graphs
  run a **real CPU reference** against one genuine 64×64 PPM pair (5 451 samples
  differ between the frames) with the source 11-channel packing and `t = 0.5`,
  and all four graphs were **serialized through the real TensorRT builder API**
  outside any playback path. `mikai_native_verify` then splits the matrix against
  the unchanged `0.02`/`0.005` bounds, with no overrides: **RIFE lite passes**
  (`maxAbsoluteError` `0.0122070312`, RMSE `0.000512728931`) and **RIFE lite
  ensemble passes** (`0.00341796875`, `0.000227462237`), while **RIFE full is
  rejected** (`maxAbsoluteError` `0.0561523438`, RMSE `0.00198529105`) and **RIFE
  ensemble is rejected** (exit `9`; `maxAbsoluteError` `0.0358886719`, RMSE
  `0.00104451097`). Every graph is preserved at dtype FP16 with input
  `1x11x64x64` and output `1x3x64x64`, and that packing is grounded in
  `kernels.cu:500-551` (`[RGB_A RGB_B, t = .5, meshX meshY mulW mulH]`).
  A pass covers **raw engine bytes, bindings and numerics only** —
  `manifestVerified: false`, `prepared: false` — so **nothing is `Ready` or
  `Active`**. The RIFE full/ensemble rejections are **unresolved** and the bounds
  are not to be widened to make them pass. M12 is partially satisfied and **not
  done**: the spatial HD Performance engine now verifies, but RIFE full/ensemble
  do not, so no complete prepared recipe and no native processing is claimed.
- **The native-facing test suites are unchanged and hermetic.** The current full
  suite exits `0` in **15.05 s**, with `TestLiveOracle` skipped by design and the
  junction-permission and missing-`MIKAI_NATIVE_ASSETS` suites still skipping.
  Those suites exercise contracts and vocabulary, not a native chain.
- **The pinned mpv fork core builds and its isolated core loads; the bundle
  candidate does not yet verify.** The fork builds here from the pinned source
  (273 steps, Release), and loading that core as an isolated module **passes**
  (exit `0`) once the stage closure matches the Qt/GNU cohort: client and header
  API both `131077`, mpv `v0.41.0-UNKNOWN`, and swap refusal, transport,
  correlation, `pending 0` and error-end-file all `RESULT: PASS`. The earlier
  `Win127` failure is **unresolved as to the missing symbol** — completing the
  stage closure fixed that scenario, but no particular missing export has been
  identified.
  The earlier **`GGML` initializer abort** on the bundle candidate is
  **superseded**: after a single MinGW-runtime deployment fix (optional
  `MIKAI_MINGW_RUNTIME_DIR`, default EMPTY), a **private coherent Native bundle**
  now **verifies exit `0`** — `138` files, `135` DLL hash+PE closure, `134`
  actual isolated `LoadLibraryEx` child loads, mpv client API `131077`, the core
  accepts `@mikai_native`, `aji` reports ABI 8 — and install/verify/list exit `0`
  as `native-0.1.0-dev.1` (descriptor
  `4f3decfb182632ff1c3654a8a021035ca2ff90e6e675683f3f78c6c8237aa3ac`) under
  `AppLocalData/native/runtime`. That `.1` install predates the later CRT/source
  updates. `0.1.0-dev.2` is now installed beside it (`current-native` points at
  `.2`; `.1` remains for rollback). Installed verify exit `0`: 83 files, client
  API `131077`, core accepts `@mikai_native`
  (`.probe-out/native-crt-closure-verify-installed.log`). A later current-build
  `mpv_surface_smoke` explicitly selects the installed `.2` and passes through
  Qt with processing off: inspected `1280x720` framebuffer, all `921600`
  samples non-black, `297305` changed samples, `24` fps, zero decoder/VO drops
  and zero live filters (`.probe-out/native-bundle-dev2-off-surface.log` and
  `.png`). This is not startup discovery, prepared readiness or inference. The
  old `.2` closure that failed `Win32 127` is superseded, not the installed tree.
  This is **private only**:
  the stored runtime choice was not written, no stock-dir change, no public
  third-party package, and the grant is still unknown. The obsolete prepared
  `1ce…`/plugin `f400…` artifact cannot admit the new module and must be rebuilt.
  Separately, a **Native-off GUI surface** through the owned runtime now
  **passes** (exit `0`) after full candidate hash validation: the Native-kind
  candidate is explicitly selected as `0.41.0-native.1`, a real 320×180 HEVC
  24 fps video plays with `contextReady` true, position `0.542 → 2.542` over a
  12 s duration, **921600/921600** non-black samples and **290264/921600** motion,
  **0** decoder and **0** VO drops, and **0** live `vf` chain entries with Native
  off. The inspected `mpv_surface_smoke.png` (1280×720) is a visibly colored
  motion test pattern. On the same candidate, `widget_pixel_probe` passes
  **21/21** with 0 skips, 0 instrumentation-unavailable markers and exit `0`; the
  inspected 640×360 PNG is colored, GL 4.6 with 32 live GL-logger messages,
  0 `ErrorType`, `glGetError` 0 and 30.44% motion. Late events after close remain
  unknown, and reopening at 256×144 15 fps refreshes the metrics and the
  `vf`-absence check. This is **real Qt/OpenGL through the owned native runtime
  with Native off** — it is **not** the native filter, **not** CUDA interop,
  **not** active inference and **not** task completion. No native frame has been
  rendered, no public bundle is shipped, and nothing is `Ready`.
- **Not a full release, but the desktop probes are green again on current-tree
  binaries:** with the session attached, `interaction_probe` is 149/149
  `RESULT: PASS` and `compare_probe` 30/30 `RESULT: PASS` (frozen-frame proof,
  0.000/255 noise floor); `hls_recovery_probe` and `external_media_probe` report
  `RESULT: PASS` (real expired-signature recovery; real picker, HTTP, same-URL
  retry, file URL). `anime4k_menu_probe` was rebuilt on semantic locators (no
  `objectName`/tooltip selectors) and reran as full PASS with 0 failures: all 24
  preset leaves selectable, effect 1.027/255 against a 0.0000 floor, reopen
  restore 0.999/255. The pixel probes still need the interactive desktop
  session; detached they read black (`hasVideo=0`). A fresh-binary
  `widget_pixel_probe` passed 11 requirements at 640×360 with `dpr=2` and 29.60%
  motion (32 GL messages, no `ErrorType`, `glGetError` 0; perf warnings remain).
  That probe accepts a PNG path argument and otherwise runs one fixed lavfi
  source, so it does not cover alternate-media or dialog flows; its only
  observed failure is a refused PNG save (FAIL, exit code 1). `shell_probe` and
  `dialog_probe` are what observed the missing-input FAIL (exit code 1), the
  static/black inconclusive SKIP (exit code 77), the moving-portrait and the
  1 s-EOF cases. `mpv_surface_smoke` also requires inter-frame motion:
  static near-black is SKIP (exit code 77). `headless_pixel_probe` proves lit
  pixels and a real render without a motion requirement; static near-black
  can pass there, while all-black is inconclusive.
  Separately, the M42 metric gate is not closed, which is a
  probe-observability gap and not a missing player-chip feature: the widget
  probe cannot observe raw `estimated-vf-fps`/drop counters through
  `VideoSurface`, so it prints them as unavailable; reported stream FPS can fall
  back to container FPS, and only the headless-owned `MpvHandle` reads the raw
  properties.

Manifest hashes were computed with `sha256sum` and download accessibility with
`curl -I`; the SDK tree's digests and their self-measured status are recorded in
`sdk-acquisition.md`.

## Consequences for the tasks that follow

- **M04's toolchain prerequisite is met** by the independently acquired SDK
  tree, and the shim builds from the staged, patched tree. What M04 does not
  yet have is a **built and recorded artefact**: the patch series identity in
  `sources.json#patchSeries` is still `PENDING`, and the applied series is not
  the full M14 SHA-256 `buildKey` integration.
- **M05** is `blocked`: `components.json#bundles[id=native]` records
  `shippingDecision: "blocked"` and ESC-1/2/3 stay `blocking`; the 2026-10-04
  grant covers this project's own integration only. What M05 lacks is both an
  input — no assembled closure and no per-file digests for a bundle that does not
  exist — and third-party clearance.
- **M15** has a working in-process helper, a passing probe and one raw plan
  produced, but the verifier **rejects** that plan on `maxAbsoluteError`, and
  there is no Qt preparation runner, no GPU work lease and no publication.
- **Every capability surface must degrade to a named `Unavailable`**, never to a
  silent substitute.

### Which tasks the escalations bind

ESC-1, ESC-2 and ESC-3 are `blocking`; ESC-4 and ESC-5 are `RESOLVED`; ESC-6 is
advisory. None of them binds **local build/use** on rights grounds (the
2026-10-04 owner grant covers this project's own integration), but each of
ESC-1/2/3 binds **bundle clearance** for its third-party component. What still
gates those tasks is **build output and capability proof**, plus third-party
redistribution clearance. Both are recorded per component in `components.json`.
Read the `effect` and `residualAccepted` fields there; the table below is a
summary of what remains outstanding, not of what is permitted:

| Area | What is now true | What is still missing |
|---|---|---|
| Shim build (M04) | the staged tree patches and builds locally; ESC-1 stays `blocking` for bundle clearance | patch series identity not recorded in `sources.json#patchSeries`; the applied diff is not the full M14 SHA-256 `buildKey` |
| Fork core (M04) | the pinned fork builds here (273 steps, Release); the isolated core loads in the matching stage closure | the original `Win127` missing symbol on the superseded closure is historical; the installed `0.1.0-dev.2` core loads under restricted search |
| Bundle (M05) | `0.1.0-dev.2` is installed, load-verified and renders moving video through Qt with processing off; `.1` remains for rollback; owner grant 2026-10-04 covers this project's own integration; `bundles[native].shippingDecision: "blocked"`; ESC-1/2/3 `blocking` | no required Standard-bundle production proof; no **public/shipped** bundle or third-party clearance; no prepared readiness or inference proof; the obsolete prepared `1ce…`/`f400…` artifact must be rebuilt for the new module |
| Preparation (M15) | in-process helper built and probed; spatial and all four RIFE raw plans produced; spatial engine verifies on the corrected reference; `0003-managed-prepared-plan.patch` builds in the SDK union; builder publish/reuse/cancel/lease/preserve **EXIT 0** with actual verified artifacts (engine `1ce1e3d9…`, plan `c1747295…`, inference `738ce3b3…`, plugin `sha256:f400b436…`); a prior `717…` wrong-identity artifact was removed from the owned store; `0.1.0-dev.2` is installed, load-verified and plays with processing off, not a preparation or inference proof | RIFE full/ensemble **rejected** on `maxAbsoluteError` (bounds unchanged/unpublished; O5 not run; precision/tactics is a hypothesis); verified engines are still not `Prepared`; SDK prep Describe now works (`.1`, `.probe-out/native-installed-prep-describe-corrected.log`); the obsolete prepared `1ce…`/`f400…` artifact must be rebuilt for the new module; the observed direct-CAPI prepared inference and factory query do not prove an installed-core Qt playback epoch or decoder-to-framebuffer inference |
| RIFE assets (M12) | all four graphs run a real CPU reference; all four serialized on GPU | full and ensemble rejected on `maxAbsoluteError`; lite and lite-ensemble verify but are not `Prepared`/`Ready`; M12 not done |
| Native execution (M15–M32, M43–M45, M48, M49) | rights do not block independent player-side implementation; direct-CAPI prepared inference is observed separately | no complete installed-core Qt prepared recipe or accepted inference framebuffer; CAPI activation is not Qt playback activation |
| Contracts (M08–M10, M13, M14, M33–M42) | unchanged and unaffected | — |
| `trtexec.exe` (ESC-5) | resolved by architecture, and now more strongly: the helper builds **in-process** and never invokes it at all | that resolution would lapse if the design ever shelled out to `trtexec` |

**M50 and M52** depend on a bundle that does not exist yet — a sequencing
consequence of M04/M05 — and, for anything **shipped**, on redistribution
clearance that the private/local determination does not provide.

There is no third route. Reimplementing the ABI-8 shim against its published
header, and substituting a different model, are both off the table: this
project uses **this** inference implementation and **these** weights, or the
capability reports `Unavailable`.
