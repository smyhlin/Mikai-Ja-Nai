# Third-party notices — native runtime components

Generated 2026-10-08 for M03; per-file inventory added 2026-10-04. This file
records what was actually found, with citations. It does **not** grant
permission and is not legal advice. Where a licence is unresolved it says so,
rather than picking the convenient reading.

Nothing has been **vendored**: the repository contains no third-party native
binary. A private bundle `native-0.1.0-dev.2` is however **installed and
running locally** on the owner's machine — 83 files built or acquired outside
this repository — and §11 maps every one of those files. Installed locally is
not shipped: the Native bundle's distribution decision remains `blocked`, and
the three open escalations are unchanged.

## 0. Scope of use

The intended use of this project, as recorded by its rights owner, is
**private, local and non-commercial**: development and personal playback on the
owner's own machine, with no distribution of any artefact to any third party.

This is a statement of intent, not a licence, and it is recorded here because it
changes the analysis of exactly one component below. It is not a workaround, and
it is not read as one.

- **Where there is a licence, the scope narrows what is possible.** For the
  CC BY-NC-SA 4.0 spatial graphs (§7), the scope means local use is *permitted*
  and only redistribution is blocked.
- **Where there is no licence, the scope does nothing.** For the inference shim
  (§2), there is no grant to narrow. Non-commercial intent is not a licence.

A note on citation, since it changes what is being relied on: an earlier
revision of this file cited the CC BY-NC-SA 4.0 NonCommercial definition as
§1(j). In the licence text as located, §1(j) defines *Licensor*;
**NonCommercial is defined in §1(k)**. Corrected below.

---

## 1. mpv (and the AnimeJaNai fork)

| | |
|---|---|
| Upstream | <https://github.com/mpv-player/mpv> |
| Fork | <https://github.com/the-database/mpv> @ `63d127174d8f70336c82a22408929e2bc301edcb` |
| Licence | GPL-2.0-or-later by default; LGPL-2.1-or-later when built `-Dgpl=false` without the GPL-only files |
| Licence texts in the fork | `LICENSE.GPL`, `LICENSE.LGPL`, `Copyright` (there is no single `LICENSE` file) |

The fork's `video/filter/vf_animejanai.c` carries the standard mpv
LGPL-2.1-or-later header. Whether the *whole* core is GPL or LGPL depends on
the meson options used for the build, and that is not derivable from a binary.
The fork's `Copyright` file lists the GPL-only files that `-Dgpl=false`
excludes (`video/out/vo_x11.c`, `video/vdpau.c`, `audio/out/ao_oss.c`, …).

Evidence: [`Copyright` at the pinned commit][mpv-copyright],
[`vf_animejanai.c` at the pinned commit][mpv-filter],
[upstream `Copyright`][mpv-upstream-copyright].

[mpv-copyright]: https://raw.githubusercontent.com/the-database/mpv/63d127174d8f70336c82a22408929e2bc301edcb/Copyright
[mpv-filter]: https://raw.githubusercontent.com/the-database/mpv/63d127174d8f70336c82a22408929e2bc301edcb/video/filter/vf_animejanai.c
[mpv-upstream-copyright]: https://raw.githubusercontent.com/mpv-player/mpv/master/Copyright

**Consequence for a shipped bundle.** A core built by this repository from the
pinned commit is a GPL-family redistribution with real obligations: complete
corresponding source including this repository's patch series, both licence
texts, and relinkability. This is why the plan builds the core rather than
adopting a third party's binary — the build options become knowable and the
corresponding source reproducible.

### Standard core — pinned author-fork developer archive

The Standard in-process Qt/PlayerShell application keeps its QOpenGLWidget
render API. CMake fetches and verifies the pinned API-only developer archive
during configure/package, compiles against its matching headers, and copies
only `libmpv-2.dll` into private local app/dist packages. It uses no `mpv.exe`,
IPC, or app-launch mpv download.

The pinned `the-database/mpv-winbuild` release is
[`2026-10-07-d6d93599d5`](https://github.com/the-database/mpv-winbuild/releases/tag/2026-10-07-d6d93599d5),
tag commit `6ac50ae6b135c5c9b484c37cc47e5368d1f8b613`, build run
[37549675458](https://github.com/the-database/mpv-winbuild/actions/runs/37549675458).
Its source mpv commit is
[`d6d93599d59069b715013a6e77e898056499b905`](https://github.com/the-database/mpv/tree/d6d93599d59069b715013a6e77e898056499b905),
tree `d69b8c428bb057743eef5aa4e33f6c92926fd393`. The API-only archive
[`mpv-dev-x86_64-20261007-git-d6d93599d5.7z`](https://github.com/the-database/mpv-winbuild/releases/download/2026-10-07-d6d93599d5/mpv-dev-x86_64-20261007-git-d6d93599d5.7z)
is 32,830,217 bytes with publisher SHA-256
`077cb75fed47b185428f97224e1d798e2d4c2f4063fd8cda2662b74f2892327c`. It
contains `include/mpv/*`, `libmpv.dll.a` and `libmpv-2.dll`; it contains no
`mpv.exe`. The locally extracted DLL was observed as x64, mpv API 2.5,
122,146,816 bytes, SHA-256
`90de8f89fa1421eaec510e5cfe11de75846b445489864220ec811fd0f08b4649`
(local measurement, not a publisher digest).

This official release DLL does **not** contain this project's Native
status/admission patches and is not a Native bundle. The separate Native fork
build remains based on commit
[`63d127174d8f70336c82a22408929e2bc301edcb`](https://github.com/the-database/mpv/tree/63d127174d8f70336c82a22408929e2bc301edcb),
with its project patch series recorded at `sources.json#patchSeries.mpvForkPatchSeries`.

mpv is GPL-family by default. The exact build options, complete corresponding
source, dependency licence closure, and relinkability for this release binary
have not been established. Accordingly, this is a private/local package only;
third-party redistribution remains blocked until that evidence is established.
The former stock `shinchiro/mpv-winbuild-cmake` app-launch download and its
machine observations are historical only, not an active path.

---

## 2. AnimeJaNai inference shim — **NO LICENCE, BLOCKING**

| | |
|---|---|
| Current official source | <https://github.com/the-database/animejanai-inference> @ v0.17.1, commit `263ff190a2227199a08292bb41508837382eb66d`, API/ABI 9 |
| Historical private Native baseline | v0.9.0, commit `c7f56baf966064a7855db2f82a2f1b0d0c6cfdb3`, ABI 8, with separately recorded local modifications and build |
| Artefacts | Windows dispatcher and backends; Linux runtime is separately released |
| Licence | **no upstream grant located; redistribution blocked** |

Evidence for the unresolved licence status:

- the repository has no `LICENSE`/`COPYING` file —
  [`main/LICENSE` returns HTTP 404][aji-license-404]
- the GitHub API reports `"license": null` for the repository —
  [repo metadata][aji-repo]
- the local reference copy at
  `C:/Users/myhli/Downloads/janai/animejanai-inference-main` has no licence
  file and no per-file licence headers
- the inspected v0.17.1 source ZIP has no entry whose name contains
  `license`, `licence`, `copying`, or `notice`; filename inspection alone
  does not establish that no grant exists anywhere

[aji-license-404]: https://raw.githubusercontent.com/the-database/animejanai-inference/main/LICENSE
[aji-repo]: https://api.github.com/repos/the-database/animejanai-inference

Absent a grant, copyright defaults to all-rights-reserved. **There is
currently no legal basis to vendor, redistribute or ship these binaries, and no
basis to treat their presence in a read-only package on this machine as
clearance.** The fact that the upstream package redistributes them is that
author's act; it is not a permission for this project.

**The recorded private, local, non-commercial scope does not mitigate this.**
There is no licence text whose limits a non-commercial use could stay within,
and no grant to fall inside. Use, modification, build and redistribution are all
equally without basis. This is the one component where the scope in §0 changes
nothing, and that asymmetry is the reason the scope is worth recording.

The only route to a cleared component is a grant from the copyright holder. A
draft request is prepared at [`../README.md`](../README.md).
**It has not been sent, and no reply has been received.** The alternative
"reimplement the ABI-8 shim from its published header" is deliberately *not*
adopted: this project is built around this specific implementation, and
substituting a different one would change the product rather than its
provenance.

The historical v0.9.0 official Windows release asset is `aji-windows-x64.zip` (657,798 bytes,
SHA-256 `22828c41439ac2b165a18294ba3e15257e8df617ab1d7ab6fb0f96e3915c5586`).
The historical baseline comparison against the v0.9.0 tag, performed before
authorized local edits, found the header, configuration parser and dispatcher
byte-identical; the TRT source and CMake were already locally modified. The
current mutable local source additionally changes the dispatcher for optional
ABI-8 capability queries and changes TRT/DirectML backend mapping. Its TRT
changes include Unicode-aware `CreateProcessW` use and wide Win32 file
operations, but the dispatcher still uses ANSI module-path and DLL-loading
calls and other config/model/cache path handling remains narrow. No end-to-end
Unicode-path support is claimed. A local MSVC/CUDA compile/link and descriptive
artifact hashes are recorded in
`sources.json#sources[id=animejanai-inference].localBuildObservation`; runtime
behavior is not established.

The current [v0.17.1 release](https://github.com/the-database/animejanai-inference/releases/tag/v0.17.1)
has an ABI-9 Windows runtime archive `aji-windows-x64.zip` (743,177 bytes,
SHA-256 `9ddf6bb6da0ec52a2da0d9a13e59be99ff97a5c8c2e7156653dd154f6a4e40ad`)
and Linux runtime archive `aji-linux-x64.tar.zst` (281,391 bytes,
SHA-256 `89ca6cc961889eebfa326745d2c7d9b96ebc71700303335f87adf1995f7c7cdc`).
These are separate from the private ABI-8 source and build above; an ABI-8
mpv filter must not be paired with the ABI-9 runtime. Release identity is not
runtime compatibility or operation evidence.

The separate Linux `aji-encode-linux-x64.tar.zst` (80,255,083 bytes,
SHA-256 `d979be62b8b7735b4b579ba7a6b68e3db7f50707b9aeb42c20da8f6ca31093fe`)
is an optional offline CLI, not used for app playback, automatically acquired,
or shipped. Its FFmpeg dependency and redistribution rights require separate
assessment. Neither its publication nor the runtime release clears public
distribution; ESC-1 remains open. Asset sizes and digests come from the release
API; see `sources.json` for the source archive and cohort details.

The official asset and its dispatcher/TensorRT/DirectML dependency closure may
be acquired only into the user's per-machine component store after explicit
user action. This does not grant redistribution rights: no Aji binary or model
graph is included in a public distribution, and there are no startup downloads.
The upstream source has no located licence or upstream grant; the owner's
2026-10-04 grant covers project-owned integration code only.

This blocks the Native bundle. It is escalated, not worked around.
The local source-only extension preserves ABI 8; the patched TRT and DirectML
backend sources map exact RIFE code `4262` (only with ensemble disabled) to
`rife_v4.26_heavy`, and the dispatcher source exposes optional
`aji_api_version()` and `aji_rife_heavy_supported()` queries. The official
downloaded v0.9.0 Aji asset lacks the extension. The local MSVC/CUDA compile
and link of all three DLLs succeeded; observed local artifact sizes and hashes
are in `sources.json#sources[id=animejanai-inference].localBuildObservation`.
This is not an ABI/capability lookup, decoder/filter, backend-load, graph
availability, or inference result, and does not establish Ready/Active or
authorize public distribution.

---

## 3. NVIDIA TensorRT 11.3

| | |
|---|---|
| Version | 11.3.0.99 (CUDA flavour **13.4** — not derivable from the TensorRT version) |
| Acquisition | `TensorRT-Enterprise-11.3.0.99-Windows-amd64-cuda-13.4-Release-external.zip` |
| Verified | HTTP 302 → 200 `application/zip`, `Content-Length: 1985279435`, **no authentication required** |
| Terms | [TensorRT Software License Agreement][trt-sla] + [TensorRT Supplement][trt-sla] |

[trt-sla]: https://docs.nvidia.com/deeplearning/tensorrt/latest/reference/sla.html

The Supplement, section 2: *"The following portions of the SDK are distributable
under the Agreement: the runtime files .so and .dll."*

Conditions that travel with any redistribution (SLA 1.2.1.x):

1. the application must have material additional functionality beyond the
   included SDK portions;
2. the distributable portions shall only be accessed by the application;
3. the notice **"This software contains source code provided by NVIDIA
   Corporation."** shall be included in modifications and derivative works of
   sample source code distributed;
4. unless a developer tool is identified in the Agreement as distributable, it
   is internal-use only;
5. the application's distribution terms must be consistent with the Agreement;
6. the SDK may not be used in any manner that would cause it to become subject
   to an open source software licence (SLA 1.2.5).

The SDK ships `doc/Acknowledgements.txt` (327 505 bytes, 5 717 lines in the
reference package copy as `TensorRT_Acknowledgements.txt`). NVIDIA's own
portions are `SPDX-License-Identifier: LicenseRef-NvidiaProprietary`; the
embedded third-party components carry their own licences (Apache-2.0, BSD-3,
MIT, …). **That acknowledgement file must ship verbatim with any bundle that
carries TensorRT components.**

**Acquisition is not blocked.** The artefact is anonymously fetchable today at
a known size. Its absence from this machine is an *installation* gap, not an
availability or approval gap. Fetching it needs no licence approval; installing
the CUDA toolkit does require owner approval for the installer, which is
outside this task.

### `trtexec.exe` — unresolved

`trtexec.exe` is a developer tool. The Supplement makes the runtime
`.so`/`.dll` distributable; SLA 1.2.1.4 says a developer tool is internal-use
only unless the Agreement identifies it as distributable, and the located
Agreement text does not identify `trtexec.exe`.

The reference package ships it and its notice claims the TensorRT
redistributables come from NVIDIA's downloads "under the same terms". That is
the upstream author's reading, not a grant this project has verified.

The architecture distinction is path-specific. The private prepared-helper
path builds engines in-process through the TensorRT builder API and does not
need `trtexec.exe`. However, the official v0.9.0 configuration path invokes
`trtexec.exe` for the TensorRT first-build. The private helper's design does not
resolve the official-config path: no NVIDIA developer-tool redistribution
clearance has been established. `trtexec.exe` remains outside any public
bundle, and ESC-5 remains unresolved for a path using the upstream config.

---

## 4. NVIDIA CUDA runtime

| | |
|---|---|
| Component | `cudart64_*.dll` |
| Terms | [CUDA EULA][cuda-eula], section 2.6 Attachment A |

[cuda-eula]: https://docs.nvidia.com/cuda/archive/12.2.1/eula/index.html

Attachment A lists the CUDA Runtime for Windows as `cudart.dll`,
`cudart_static.lib`, `cudadevrt.lib`, and explicitly permits *"certain
variations of these files that have version number or architecture specific
information embedded in the file name"* — `cudart64_90.dll` is given as the
example.

The same distribution requirements and the same no-open-source-licence clause
(SLA 1.2.5) apply. The CUDA agreement additionally states at 1.1.6 that the SDK
may be used to develop and test applications released under OSI-approved open
source licences.

**cuDNN and cuBLAS are not required.** The observed engine settings disable
them (`--tacticSources=-CUDNN,-CUBLAS,-CUBLAS_LT`) and the kernels use only
`cudaMalloc`/`cudaMemcpy` and the driver API. The plan's bundle layout mentions
`cudnn, cublas*`; that is broader than the need this project has.

---

## 5. ONNX Runtime (DirectML build) — MIT

| | |
|---|---|
| Package | `Microsoft.ML.OnnxRuntime.DirectML` **1.24.4** |
| Upstream commit | `2d924974ef147392ced8409d36bd6d2e7fcc8a74` (`rel-1.24.4`, from the nuspec) |
| Licence | MIT, © Microsoft Corporation |
| Files | `onnxruntime.dll`, `onnxruntime_providers_shared.dll` |

Evidence: [LICENSE at v1.24.4][ort-license], [nuspec][ort-nuspec]. Obligation:
retain the copyright notice and permission text.

1.24.x is the last DirectML-flavoured ONNX Runtime release; Microsoft moved
DirectML to sustained engineering pending a WinML migration, so do not expect a
newer pin to exist.

[ort-license]: https://raw.githubusercontent.com/microsoft/onnxruntime/v1.24.4/LICENSE
[ort-nuspec]: https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.nuspec

---

## 6. DirectML — **resolved**

| | |
|---|---|
| Package | `Microsoft.AI.DirectML` **1.15.4** ("DirectML Redistributable Package") |
| Package digest | sha256 `4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9`, 202 292 617 bytes |
| Binary licence | `LICENSE.txt` — Microsoft Software License Terms (Microsoft DirectX Machine Learning) |
| Code licence | `LICENSE-CODE.txt` — MIT, © Microsoft Corporation |

The package was downloaded and read. It carries **two** licence files, and the
apparent earlier conflict was a filename mismatch rather than competing
licences:

- `LICENSE.txt` (10 439 bytes, sha256 `a05138e3…`) governs the binary
  `DirectML.dll` and is a **Microsoft Software License Terms** agreement. At
  **section 1(a)** it grants: *"You may copy and distribute the software (i.e.
  make available for third parties) in applications and services you develop in
  the build with Machine Learning tools and frameworks, and/or games that run on
  Windows and Xbox."*
- `LICENSE-CODE.txt` (1 093 bytes, sha256 `903df551…`) covers the code under
  **MIT** — the same licence as the [`microsoft/DirectML` repository][dml-license].

The nuspec's `<license type="file">LICENSE.txt</license>` points at the binary
terms, which is the correct one for a DLL. The reference package's notice
calling the file `DirectML_LICENSE.txt` is a naming inconsistency on that side.

**Cleared**, subject to the terms' own conditions: Windows/Xbox use only, not a
stand-alone offering, Microsoft notices retained (§3(c)), export restrictions
apply (§4).

`DirectML.dll` from this package is byte-identical to the copy in the read-only
reference package — same size (18 527 776) and same sha256 `9c9e6d82…`. That
turns the reference package's DirectML binary from an unattributed blob into a
verified artefact of this pinned package.
The Aji DirectML closure uses ONNX Runtime DirectML **1.24.4** and DirectML
**1.15.4**, with their respective MIT and Microsoft binary terms/notices above.
These runtime packages are provisioned into the user's per-machine component
store only following explicit user action; this notice does not authorize a
public bundle or claim that the backend has loaded or run.

[dml-license]: https://raw.githubusercontent.com/microsoft/DirectML/master/LICENSE

---

## 7. AnimeJaNai upscaler models — CC BY-NC-SA 4.0

| | |
|---|---|
| Files | the five `2x_AnimeJaNai_*.onnx` graphs |
| Upstream | <https://github.com/the-database/mpv-AnimeJaNai> (licence at the repository root) |
| Licence | **Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International** |

Evidence: [upstream LICENSE][mpvajnai-license]. The model files themselves carry
no embedded licence header; the repository-level licence is the only evidence
located.

[mpvajnai-license]: https://raw.githubusercontent.com/the-database/mpv-AnimeJaNai/main/LICENSE

Obligations and limits, quoted from the located text:

- **NonCommercial** — the grant is *"to reproduce and Share the Licensed
  Material, in whole or in part, for NonCommercial purposes only"* (§2(a)(1)),
  and NonCommercial is defined as *"not primarily intended for or directed
  towards commercial advantage or monetary compensation"* (§1(k)).
- **ShareAlike** — *"if You Share Adapted Material You produce"*, the Adapter's
  License *"must be a Creative Commons license with the same License Elements,
  this version or later"* (§3(b)).
- **Attribution** — on sharing, retain creator identification, copyright notice,
  a licence notice, a warranty-disclaimer notice and a URI (§3(a)(1)(A)).
- **No patent or trademark rights** are licensed (§2(b)(2)).
- The licensor **reserves the right to collect royalties** for uses other than
  NonCommercial (§2(b)(3)).
- Rights **terminate automatically** on non-compliance, subject to the 30-day
  cure in §6(b).

**Use and redistribution are different acts, and the recorded scope decides
which one applies.** This is the distinction the previous revision of this file
left unresolved:

- **Local non-commercial use is permitted.** The recorded intended use of this
  project is private, local and non-commercial, which is what §1(k) describes.
  Running these five graphs on the owner's own machine is therefore inside the
  grant. No ShareAlike condition attaches, because §3(b) is conditioned on
  *sharing*, and nothing is shared.
- **Redistribution is not permitted.** Placing the graphs in a shipped bundle is
  a *Share* — §1(l) defines Share to include "reproduction, … distribution,
  dissemination, communication, or importation". That engages §3(a) attribution
  and §3(b) share-alike, and it is outside the recorded scope regardless of how
  non-commercial it would be.

So the blocker on these five files is **redistribution**, not use. The
open question recorded below is about distributing, and it does not need to be
answered for the development this project is actually doing.

Separately, the TensorRT SLA and CUDA SLA each contain a clause (1.2.5) against
using the SDK in a manner that would cause it to become subject to an open
source software licence. Whether a given arrangement of these model weights and
NVIDIA redistributables in one bundle engages that clause is likewise a
rights-owner question. **No incompatibility is asserted here**: CC BY-NC-SA 4.0
licenses the model files, the NVIDIA instruments license the NVIDIA components,
and one does not by itself impose terms on the other.

**Redistribution not cleared; local use permitted.** The NonCommercial question
is no longer open for the use this project actually makes, and the remaining
condition is that these five files stay out of any shipped bundle. If
distribution is ever contemplated, the rights owner must record the
NonCommercial determination *at the time of distribution* and state how the
§3(a) attribution and §3(b) share-alike conditions are discharged. Neither
question blocks development today. M11/M13 are likewise unblocked: the catalog
must carry a per-model `termsNote` so that a user-supplied model is handled
under its own terms.

---

## 8. RIFE v4.14 graphs — **acquired, weight data traced; whole-graph redistribution unresolved**

M12's combination space reaches **four** graphs, because the two suffix axes are
independent: `rife_v4.14.onnx`, `rife_v4.14_lite.onnx`,
`rife_v4.14_ensemble.onnx` and `rife_v4.14_lite_ensemble.onnx`. The names follow
`rife_model_name()` in the shim (code `414` → `rife_v4.14`; a four-digit code
ending in `1` → `_lite`; ensemble appends `_ensemble` on a separate boolean),
cross-referenced by the identical enumeration in [vs-mlrt's `vsmlrt.py`][vsmlrt].
All four are original supported combinations, not a substitute for a missing
asset.

[vsmlrt]: https://raw.githubusercontent.com/AmusementClub/vs-mlrt/master/scripts/vsmlrt.py

| Layer | Project | Licence |
|---|---|---|
| Original weights | [`hzwer/Practical-RIFE`](https://github.com/hzwer/Practical-RIFE) @ pinned commit `de9a989bb7b8a71d94f058297e603633aaa43ad6` | **MIT** — author states the linked model files share the project's MIT licence |
| Research origin | [`hzwer/ECCV2022-RIFE`](https://github.com/hzwer/ECCV2022-RIFE) | MIT, © Megvii Inc. — [LICENSE][rife-license] |
| ONNX export used as conversion input | [`AmusementClub/vs-mlrt`](https://github.com/AmusementClub/vs-mlrt) | GPL-3.0-only — [LICENSE][vsmlrt-license] (covers its *code*) |
| The exact fp16 graphs the shim consumes | [`the-database/animejanai-inference`](https://github.com/the-database/animejanai-inference) @ tag `models-rife-fp16-1`, asset `rife-fp16-1.7z` | see lineage note below; archive pin and per-graph digests in [`../README.md`](../README.md) |

[rife-license]: https://raw.githubusercontent.com/hzwer/ECCV2022-RIFE/main/LICENSE
[vsmlrt-license]: https://raw.githubusercontent.com/AmusementClub/vs-mlrt/master/LICENSE

Practical-RIFE's exact wording — *"The content of these links is under the same
MIT license as this project"* — is an **affirmative grant by the copyright
holder** naming the linked model files specifically. It is accepted here as
licence evidence for the v4.14 and v4.14.lite weights. It is a project-level
statement rather than a per-file notice — a difference in form, not in substance.

**The weight-data hop is closed.** The author's own v4.14 / v4.14.lite
checkpoints were acquired and compared tensor by tensor against all four graphs:
128/128 names and shapes matched with FP32→FP16 round-to-nearest-even bytes
identical in four comparisons, and each ensemble graph's initializer set is
byte-identical to its base graph's. The stored weight bytes **are** the author's
MIT weights. This used no execution — it walks stored weights only.

**What remains open, stated narrowly.** Exporter identity and the non-weight
graph material. The comparison reads initializers, not node semantics: it proves
nothing about which exporter produced the fp16 GridSample fp32 island, the
Transpose-built grids, the cast placement or the ensemble merge math, and no
upstream statement names it. The publishing repository declares no licence of
its own and the asset carries no per-file notice, so the rights basis for
redistributing the files **as artefacts** is not established.

Two things that are **not** claimed:

- A converted graph does **not** automatically inherit the publishing
  repository's licence status. That repository's absent licence governs its
  *source code*; it does not by itself set the rights in the files it publishes.
- GPL-3.0 on vs-mlrt's *code* does **not** automatically extend to its model
  outputs. Whether it does is unsettled by the licence text alone, and is not
  asserted either way.

**Ensemble.** No ensemble weight artefact is published in Practical-RIFE's
links. The ensemble graphs' initializer sets are byte-identical to their base
graphs' and match the same MIT checkpoints; that does not settle the
graph-operator derivation or whole-artefact redistribution.

**Acquisition.** All four graphs were extracted from the pinned
`rife-fp16-1.7z` release (its published SHA-256 reproduced exactly) into
`F:\ZKS\mikai-native-assets\rife\onnx\`, outside the repository and every
tracked tree. Archive pin, extraction, structural check, the comparison report
and the rights position: **`../README.md`**.
Model graphs remain separate from the app distribution and are staged only in
per-machine user-owned graph stores after user action; this does not resolve
ESC-2 or ESC-3 and does not grant public redistribution.

**Status: acquired and weight-data traced, not cleared.** No grant covers
redistributing the graph files, so they stay out of any shipped bundle; M12's
capability surface can be built and unit-tested against fixture inputs. Nothing
was executed, built into an engine, bundled or exposed as Ready.

---

## 9. Anime4K GLSL shaders — MIT and Unlicense

The 49 `Anime4K_*.glsl` files in the repository root `shaders/` are vendored by
this repository and shipped with the Standard build. Reading the files
establishes their licence directly, so no external lookup is needed:

- **47 files** carry an MIT licence header — *"// MIT License / Copyright (c)
  2019-2021 bloc97 / All rights reserved."*
- **2 files** — `Anime4K_AutoDownscalePre_x2.glsl` and
  `Anime4K_AutoDownscalePre_x4.glsl` — carry the **Unlicense** (public domain
  dedication): *"This is free and unencumbered software released into the public
  domain."*

Both permit redistribution. This is a *native-out-of-scope* component — it is
not part of a native bundle and therefore has no row in `components.json` — but
it is recorded here so the shipped artefact set has no unrecorded licence.

---

## 10. Explicitly not adopted

Not shipped, not vendored, not a dependency: `mpvnet.*` and the .NET manager
payload; `AnimeJaNaiManager.exe`; `AnimeJaNaiUpdater.exe`; `7za.exe`;
`yt-dlp.exe`; `portable_config/scripts/*.lua`, `mpv-animejanai.conf`,
`input-animejanai.conf`; the WPF support DLLs; `animejanai/benchmarks/*`;
**AnimeInterp in any form**; and the ambient Conda TensorRT runtime found on
this machine, which is research residue — production must never depend on
Conda, Torch, Python or any ambient install.

---

## 11. Per-file inventory of the installed private bundle

`components.json#installedBundleInventory` maps **every one of the 83 files**
(80 DLLs, 3 EXEs) in the installed private bundle `native-0.1.0-dev.2` to its
source, the licence as actually located, and one of the three decisions. This
section summarises; it does not replace it.

Each of the 83 digests was recomputed from the file on disk and matched. How
each attribution was *established* matters more than the hash:

| Cohort | Files | Attribution basis | Licence as located | Decision |
|---|---|---|---|---|
| mpv fork core | 1 | byte-identical to this repository's own build of the pinned commit | GPL-2.0-or-later | cleared, GPL obligations |
| ABI-8 shims | 2 | byte-identical to this repository's build from the patched pinned source | **none upstream** | **blocked** (ESC-1) |
| ggml / whisper self-build | 17 | byte-identical to this repository's own build | MIT | cleared |
| MSYS2 verbatim cohort | 36 | exact digest match to the pinned package archive | mixed permissive / LGPL / GPL | cleared per package |
| MSYS2 relinked cohort | 13 | byte-identical to this repository's relink + export-set comparison | as owning package | cleared per package |
| Microsoft CRT | 4 | pinned `vc_redist` payload | VC++ Redistributable terms, **text not recovered** | external |
| CUDA runtime | 1 | byte-identical to NVIDIA's published redistributable | CUDA EULA Att. A | external |
| TensorRT | 3 | byte-identical to the pinned `-external` SDK | TensorRT SLA + Supplement | external |
| ONNX Runtime / DirectML | 3 | byte-identical to the pinned NuGet packages | MIT / MS Terms | cleared |
| Project tools | 3 | project-owned code paths | this project's Unlicense grant | cleared |

### What the licence cohort label does *not* say

The 49 `mpv-deps` files have **no single licence**. This is a cohort, not a
licence: it contains permissive (MIT, BSD-3, Apache-2.0, Zlib, ISC), LGPL
(`glib2`, `fribidi`, `graphite2`, `gdk-pixbuf`, `librsvg`, `libthai`,
`libunistring`), **GPL-family and dual terms** (`libfreetype`
GPL-2.0-or-later OR FTL, `libiconv` LGPL-2.1-or-later OR GPL-3.0-or-later,
`libidn2` GPL-2.0-or-later OR LGPL-3.0-or-later, `libintl`
GPL-3.0-or-later AND LGPL-2.1-or-later, `libzstd` BSD-3-Clause OR
GPL-2.0-or-later, `liblzma` 0BSD AND LGPL-2.1 AND GPL-2.0, `libdatrie` LGPL)
and packages declaring only `custom` or a non-SPDX string. Each DLL keeps its
owning package's terms; the per-file `licenceSpdxAsDeclaredByPackage` field
carries the exact string. GPL-family members carry source and relinkability
duties on any distribution.

### Three things this inventory does **not** prove

1. **The 13 relinked DLLs are not behaviourally proven.** Their export sets are
   identical to the prefix originals (measured: cairo 415, fontconfig 246,
   freetype 220, harfbuzz 1415, pango 514, rsvg 55, … zero missing, zero extra)
   and they import none of `libstdc++-6`/`libgcc_s_seh-1`/`libwinpthread-1`. But
   they were linked with `-Wl,--allow-multiple-definition` and a privately
   linked winpthread. That is a symbol-interposition risk, not a build failure.
   Export identity is **not** behavioural equivalence; no run-time equivalence
   test is recorded.

2. **The Microsoft CRT licence text is missing.** The pinned installer was
   downloaded and its payload located as two cabinet containers, but the EULA
   was not recovered from them. The terms are identified by acquisition route
   only. This is recorded as *missing evidence*, not as a grant.

3. **Only the `sm120` TensorRT builder resource ships.** The kernel-architecture
   list in `sources.json#buildConfigurationPins` enumerates ptx plus
   sm75–sm100 and sm120; this bundle carries **only** `sm120`. On a GPU whose
   SM is not sm120, the builder would need a resource that is absent. That is a
   real closure gap in this particular installed instance and is recorded here
   rather than papered over.

### Closure and self-containment

Every static import of all 83 files resolves inside the bundle, to an
`api-ms`/`ext-ms` API set, or to a Windows system library. No bundled binary
imports a third-party DLL the bundle does not ship, and none imports the GNU
runtime trio — so the native closure cannot collide with the Qt cohort. What is
still imported from outside is named per row under
`importsNeedingAnExternalProvider`, and every such name is either a Windows
system library or a driver-supplied one (`nvcuda` via the CUDA driver;
`Qt6Core` for the project-owned `native_inspect.exe`, which deliberately does
not ship Qt).

`ggml_backend_load_all` resolves CPU backends from the executable's directory
and the current working directory, **not** via `AddDllDirectory`. The 14
`ggml-cpu-*.dll` files sitting beside the core is necessary but is **not** by
itself proof the variant set resolves on an arbitrary machine. That remains a
run-time question and is not claimed here.

`libmpv-2.dll` statically imports `vulkan-1.dll` even though the meson build
records `vulkan=disabled`. This is a hard load-time import: the installed GPU
driver supplies the Vulkan loader on the target system, but a missing loader
prevents `libmpv-2.dll` from loading. It is recorded rather than assumed away.

### Wire version versus producer ABI

The bundle manifest's `protocolVersion: 1` and `engineSchemaVersion: 1` are
**correct** and are not a mismatch. The shipped core contains
`mikai.native.status/1`, `mikai.native.admission/1` and
`mikai.native.control/1`, and no `/2` variant — matching the producer in the
effective fork. The producer CAPI snapshot/schema **v2**, the factory query
**v1** and source **ABI 8** are a separate domain (`sources.json#abi`,
`#patchSeries`); they are not the same numbers as the status wire and are not
conflated with it. No version field was changed.

### Escalations unchanged

Nothing in this inventory resolves ESC-1, ESC-2 or ESC-3, and the Native
bundle's shipping decision stays `blocked`. The owner's 2026-10-04 grant covers
this project's own integration code only. `aji.dll` and `aji_trt.dll` are
**present** in the installed private bundle and **blocked** for redistribution —
`inBundle` in `components.json` answers "may this ship?", not "is it here?"; see
`components.json#inBundleSemantics`.

---

## Summary

| Component | Licence as located | Redistribution |
|---|---|---|
| Stock mpv core (Standard) | GPL-2.0-or-later | external — not vendored |
| mpv fork core (Native) | GPL-2.0-or-later **or** LGPL-2.1-or-later, per build options | **cleared**, with GPL-family obligations, once self-built from the pin |
| `aji.dll` / `aji_trt.dll` / `aji_dml.dll` | **none** | **blocked** |
| TensorRT runtime DLLs | NVIDIA TensorRT SLA + Supplement | external — acquired from NVIDIA, bundled under §1.2.1.x |
| `trtexec.exe` | unresolved for upstream-config first-build; private prepared-helper path avoids it | **blocked** (ESC-5; no NVIDIA developer-tool clearance) |
| `cudart64_*.dll` | CUDA EULA Attachment A | external — acquired from the CUDA redistributable |
| `onnxruntime.dll` | MIT | **cleared** |
| `DirectML.dll` | Microsoft Software License Terms (binary) + MIT (code) | **cleared**, on the licence read from the pinned package |
| AnimeJaNai ONNX models | CC BY-NC-SA 4.0 | **redistribution blocked** (ESC-2); local non-commercial use permitted |
| RIFE v4.14 full/lite/ensemble × {ensemble off/on} | MIT upstream — stored weight data traced; whole-graph redistribution unresolved | **blocked** — redistribution, not availability (ESC-3) |
| Anime4K shaders | MIT (47) / Unlicense (2) | cleared — out of native scope, recorded for completeness |
| `mpv-deps` cohort (49 files) | mixed: permissive, LGPL, GPL-family and dual terms, per owning package | cleared **per package**; GPL-family members carry source and relinkability duties |
| ggml / whisper self-build (17 files) | MIT | **cleared** — retain the copyright notice |
| Microsoft CRT (4 files) | VC++ Redistributable terms; **text not recovered** | external — recorded as missing evidence, not as a grant |

**Three rights blockers remain open, and none is paperwork.** ESC-1 (§2) — the
inference shim has no licence of any kind, and the recorded non-commercial
scope does not change that. ESC-2 (§7) blocks only the *redistribution* of five
CC BY-NC-SA model files, not their local use. ESC-3 (§8) blocks the
*redistribution* of the four acquired RIFE graphs on whole-graph provenance and
rights, not their acquisition. The earlier wording of this paragraph said "one
rights blocker"; that was wrong on its own terms and against
`components.json#escalationsSummary`, which has always recorded three. It is
corrected here rather than quietly narrowed.

ESC-1 binds the tasks that build, ship or execute the shim; the stock mpv
loader path, the contract and vocabulary work, and the import of user-supplied
models are untouched by it. ESC-2 blocks only the *redistribution* of five
model files, not their local use. ESC-3 blocks the *redistribution* of the four
acquired RIFE graphs on whole-graph provenance and rights, not their
acquisition: the graphs are on disk and the stored weight data traces to the
author's MIT checkpoints.

The full blocker-to-task mapping is in
[`components.json`](components.json#blockertaskbindings) and in
[`../README.md`](../README.md).
