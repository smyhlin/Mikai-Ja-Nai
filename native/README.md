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
execution needs a bundle and the ABI-8 shim, and neither exists here (ESC-1).

**The mpv AnimeJaNai fork is not built by this repository.** Its source pin is
recorded below; the build that would produce `core.fork-mpv` belongs to M04,
which is blocked by ESC-1, not by its toolchain.

## Contents

| Path | What it is |
|---|---|
| `manifest/sources.json` | Pinned source identities, ABI contract, build-configuration pins, and a clearly separated block of local measurements |
| `manifest/components.json` | Every artefact a shipped bundle would contain, each with a licence and an explicit `cleared` / `blocked` / `external` decision |
| `manifest/THIRD_PARTY_NOTICES.md` | The same findings as prose, with citations, and the reasoning behind each decision |
| `manifest/videojanai-release.json` | Release-asset inventory for the VideoJaNai 2.1.0 components |
| `tools/manifest_check.cpp` | `mikai_manifest_check` — recomputes recorded digests and validates component/source/rights identity |
| `tools/native_inspect.cpp` | `native_inspect` (M11) — read-only ONNX inspection CLI over the official ONNX Runtime C API |
| `tools/bundle_tool.cpp` | `mikai_bundle` — validate, describe, list and install runtime bundles; loads no module |
| `bundle/manifest.schema.json` | The runtime-bundle manifest schema (M05) |
| `permission-request.md` | A drafted, **unsent** request to the inference shim's copyright holder for the grant that would clear ESC-1 |
| `sdk-acquisition.md` | How the MSVC/CUDA/TensorRT/ONNX Runtime/DirectML/Windows SDK trees were obtained, and what was proven with them |
| `spatial-acquisition.md` | The five spatial ONNX graphs: source pin, hashes, licence, and the CPU reference observation |
| `rife-acquisition.md` | The four RIFE v4.14 graphs: archive pin, extraction, structural check, weight-data comparison, rights position |
| `videojanai-acquisition.md` | VideoJaNai 2.1.0 component archives: digests, roles and terms evidence |

Not yet here, owned by later tasks: `CMakeLists.txt`, `patches/`,
`build-msvc.md`, `tools/mikai_native_prep` (M04); `bundle/` so far carries only
the manifest schema (M05).

## Scope of use: private, local, non-commercial

The intended use of this project, as recorded by its rights owner, is
**private, local and non-commercial**: development and personal playback on the
owner's own machine, with no distribution of any artefact to any third party.
That is a fact about this project's plans, **not a licence**, and it changes
what each instrument does and does not permit. It is recorded here because two
of the findings below turn on the difference between *using* a component and
*redistributing* it.

- **CC BY-NC-SA 4.0 — the spatial upscaler weights.** Under the located text,
  the recorded use is *inside* the grant: §2(a)(1) licenses reproduction and
  sharing "for NonCommercial purposes only", and §1(k) defines NonCommercial as
  "not primarily intended for or directed towards commercial advantage or
  monetary compensation". Running the weights on the owner's own machine is
  therefore permitted use, and it engages no ShareAlike condition, because
  nothing is shared. **Redistribution is a different act and stays
  unauthorized.** Putting the five graphs into a shipped artefact would be a
  Share, engaging the §3(a) attribution and §3(b) share-alike conditions, and
  it is outside the recorded scope in any case.
- **The inference shim has no grant of any kind.** Non-commercial intent is
  irrelevant to it: there is no licence text whose limits a non-commercial use
  could stay within, and no grant to fall inside. Private local use,
  modification, building and redistribution of `aji.dll`, `aji_trt.dll` and
  `aji_dml.dll` are all equally without basis. Nothing in this project's scope
  cures that, and no waiver or private approval exists; only the holder's grant
  does. See `permission-request.md`.

## Status: the rights gate does not close

M03 blocks M05 and every distribution step. **The gate does not close, and the
blockers are real, not paperwork.**

- **ESC-1 — the inference shim has no licence at all.** `the-database/animejanai-inference`
  (the source of `aji.dll`, `aji_trt.dll`, `aji_dml.dll`) has no `LICENSE` file,
  no SPDX identifier, no per-file headers, and GitHub reports `license: null`.
  Absent a grant, copyright defaults to all-rights-reserved. It blocks **all
  use, not only redistribution**. The committed pin
  `738ce3b3020b3950c48ce3122a08f32af531d4fa` is verified byte-for-byte against
  upstream, but the pin records identity only: it carries **no owner grant**,
  and the draft in `permission-request.md` remains **unsent**. No substitute
  implementation is proposed.
- **ESC-2 — the spatial ONNX weights are CC BY-NC-SA 4.0.** *Using* them
  locally is within the located grant; **redistribution is not permitted**, so
  the five graphs stay out of any shipped bundle. No incompatibility with the
  NVIDIA instruments is asserted: the two licences govern different components.
- **ESC-3 — the RIFE v4.14 graphs: assets acquired, whole-graph redistribution
  unresolved.** All four graphs their combination space reaches are on disk,
  pinned and structurally verified, and the stored weight tensors match the
  author's own MIT-licensed v4.14 / v4.14.lite checkpoints byte-for-byte after
  fp16 rounding. What remains open is exporter identity, the non-weight graph
  material, and the rights basis for redistributing the files as artefacts —
  the graphs carry no per-file notice. See `rife-acquisition.md` §5–§7.

No binary has been vendored.

## What is acquired, and what each acquisition does and does not prove

Acquisition is not clearance, and none of the records below is a readiness
claim. Every tree lives outside the repository and outside every tracked source
tree, so nothing can be committed or accidentally vendored.

- **mpv fork core** — licence cleared (GPL-2.0-or-later or LGPL-2.1-or-later per
  build options, see ESC-6), **but not built by this repository**. The root
  CMake binds only its headers; the corresponding-source, licence-text and
  relinkability obligations attach to whatever build actually exists.
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
  only — no inference chain, engine, playback path or framebuffer result is
  claimed from it, and it does not move the shim past ESC-1.
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
  redistribution remain unresolved. Archive pin, extraction, comparison report
  and rights: **`rife-acquisition.md`**.
- **VideoJaNai 2.1.0 components — acquired as evidence only.** Six assets
  (manifest, packs, TRT runtime/SM120/PTX, RIFE) with independent SHA-256
  recomputation; no component was installed, executed, bundled or used to
  replace a driver. No source/binary grant was located for the aji source or
  binaries, so ESC-1 is unchanged. See `videojanai-acquisition.md`.
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
  1 985 279 435 bytes); `trtexec.exe` is not located as distributable (ESC-5).
- **Anime4K shaders** — MIT (47 files) and Unlicense (2 files), read directly
  from the files. Out of native scope, so no `components.json` row.

## Pins that matter

| Pin | Value |
|---|---|
| mpv fork | `63d127174d8f70336c82a22408929e2bc301edcb` (signed, 2026-09-17) — **not built by this repository** |
| inference shim | `738ce3b3020b3950c48ce3122a08f32af531d4fa` — **verified**: all nine key source files hash identically to upstream at this commit; **no owner grant**, request unsent |
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
**exit 3** (components `blocked` — the recorded outcome, not a checker failure),
**14 recorded file digests recomputed and matched**, and unestablished inventory
for the sources that were not present then (`mpv-fork`,
`mpv-upstream-stock-build`, `tensorrt-11.3-sdk`, `cuda-toolkit`,
`onnxruntime-directml`, `directml`, `rife-v4.14-assets`, `toolchain.msvc`,
`ffmpeg-dev`). It proves the recorded digests of the files that were present
match, and nothing about runtime behaviour or bundle assembly. The sources since
acquired (SDKs, spatial, RIFE, VideoJaNai) change those inventory rows only once
the manifest pins record them — a parent integration step, not made in this file.

**`native_inspect`** (M11) is the read-only ONNX inspection CLI. It uses the
official ONNX Runtime C API to read a graph's declared I/O and, with
`--resolve WxH`, run one **CPU reference** at that geometry. It is CPU-only and
touches no engine path; the five-graph observation above is its result.

**`mikai_bundle`** validates, describes, lists and installs runtime bundles. No
subcommand loads a module or starts a core, so a passing `verify` says only that
the manifest matches the bytes — loadability and filter registration stay the
loader's business.

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

- **Fresh build with the inspector OFF (no native SDK):** the `mikai`
  application configures and builds, and the C++ ctest suite passes (**14.55 s**
  wall, hermetic; `LiveOracle` still skips by design). A Qt/OpenGL frame at
  640×360 rendered with **all 25 680 sampled pixels lit**.
- **The produced `mikai` executable imports no mpv, ONNX Runtime, CUDA,
  TensorRT or DirectML DLL** — mpv is resolved at run time, and nothing native
  is on the link path.
- **Optional inspector ON:** the configuration
  (`-DMIKAI_WITH_MODEL_INSPECTION=ON -DORT_INCLUDE_DIR=…`) builds the inspector
  and its test matrix passes (**25.48 s**).
- **No Native execution was run, and no runtime behaviour of any inference
  component has been observed:** the shim is not built, because building it is
  exactly the act ESC-1 withholds, and `mikai_manifest_check` loads no DLL.
  Manifest syntax inspection is not integrity proof.
- **Not a full release.** The desktop interaction probe currently has **5
  failures plus a late access violation**, and the Compare probe's stale chooser
  checks are **being repaired**; those are active work, not settled results.

Manifest hashes were computed with `sha256sum` and download accessibility with
`curl -I`; the SDK tree's digests and their self-measured status are recorded in
`sdk-acquisition.md`.

## Consequences for the tasks that follow

- **M04's toolchain prerequisite is met** by the independently acquired SDK
  tree (`-DAJI_TRT_ROOT=<root> -DAJI_NVINFER=nvinfer_11` against the recorded
  pins). What blocks M04 is **not** the toolchain: it is ESC-1. Building the
  inference shim from unlicensed source is exactly the act that needs a grant,
  so the toolchain being ready does not move the shim one step closer. The patch
  series identity belongs in `sources.json#patchSeries`, still `PENDING`.
- **M05** inherits `bundle.shippingDecision: "blocked"` for the Native bundle.
  The Standard bundle is unblocked.
- **Every capability surface must degrade to a named `Unavailable`**, never to a
  silent substitute.

### Which tasks the rights blockers actually bind

A component is not a task: a blocker binds the tasks whose **own acceptance**
needs the inference source, the built shim binaries, or a redistributed
third-party artefact. Dependency ordering — M05 preceding M06, and so on — is a
separate planning fact and is not a rights question. The mapping is recorded in
`manifest/components.json`; as corrected here:

| Blocker | Binds directly | Does not bind |
|---|---|---|
| **ESC-1** — no licence for the inference shim | **M04** (builds the shim — toolchain ready, grant not); **M05** (would ship `aji.dll` / `aji_trt.dll` / `aji_dml.dll`); every task whose acceptance executes a native chain — **M15–M32** — and the native acceptance runs **M43, M44, M45, M48, M49** | **M06, M07** (loader and discovery over the *stock* core); **M08, M09, M10, M13, M14** (contracts and typed vocabulary); **M11** (imports *user-supplied* models under their own terms); the presentation and boundary tasks **M33–M42** |
| **ESC-2** — CC BY-NC-SA spatial weights | **M05** only, and only because the bundle would carry the five graphs | Local non-commercial use of those weights, which the located terms permit; **M11/M13**, which handle user-supplied models under their own terms |
| **ESC-3** — RIFE graphs: whole-graph redistribution unresolved | **M05**, if the bundle would carry the RIFE graphs; **M24/M44** need the engine-build and runtime proof, which is a capability gate, not a rights one | Everything else; the graphs are acquired and the weight data traces to the author's MIT checkpoints |
| **ESC-5** — `trtexec.exe` | Nothing, **by design**: the planned engine build is in-process through the TensorRT builder API, so no task needs to ship it | — |

**M50 and M52 are not independently rights-blocked.** They are gated on the
*bundle* they must install: M50's acceptance covers a licensed Native bundle
that cannot exist while ESC-1 stands, and M52 walks that same acceptance list.
That is a consequence of M04 and M05, reported as such rather than as a
separate finding.

There is no third route. Reimplementing the ABI-8 shim against its published
header, and substituting a different model, are both off the table: this
project uses **this** inference implementation and **these** weights, or the
capability reports `Unavailable`.
