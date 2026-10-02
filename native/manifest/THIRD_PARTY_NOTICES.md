# Third-party notices — native runtime components

Generated 2026-10-01 for M03. This file records what was actually found, with
citations. It does **not** grant permission and is not legal advice. Where a
licence is unresolved it says so, rather than picking the convenient reading.

Nothing has been vendored. As of this writing the repository contains no
third-party native binary, and no native bundle ships.

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

### Stock core (Standard bundle)

Provisioned at run time from
[`shinchiro/mpv-winbuild-cmake`](https://github.com/shinchiro/mpv-winbuild-cmake)
by the pre-existing discovery/download path, which this work leaves untouched.
That recipe does not pass `-Dgpl=false`, so it is GPL-2.0-or-later. It is not
vendored here.

---

## 2. AnimeJaNai inference shim — **NO LICENCE, BLOCKING**

| | |
|---|---|
| Source | <https://github.com/the-database/animejanai-inference> @ `738ce3b3020b3950c48ce3122a08f32af531d4fa` |
| Artefacts | `aji.dll`, `aji_trt.dll`, `aji_dml.dll` |
| Licence | **none** |

Evidence that no licence exists:

- the repository has no `LICENSE`/`COPYING` file —
  [`main/LICENSE` returns HTTP 404][aji-license-404]
- the GitHub API reports `"license": null` for the repository —
  [repo metadata][aji-repo]
- the local reference copy at
  `C:/Users/myhli/Downloads/janai/animejanai-inference-main` has no licence
  file and no per-file licence headers

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
draft request is prepared at [`../permission-request.md`](../permission-request.md).
**It has not been sent, and no reply has been received.** The alternative
"reimplement the ABI-8 shim from its published header" is deliberately *not*
adopted: this project is built around this specific implementation, and
substituting a different one would change the product rather than its
provenance.

The commit pin above is **verified, not assumed**: all nine key source files
were hashed locally and again from upstream at that exact commit, and every
digest matched. See `sources.json#sources[id=animejanai-inference]`.

This blocks the Native bundle. It is escalated, not worked around.

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

The planned architecture removes the question rather than answering it: engines
are built in-process through the TensorRT builder API in an out-of-band
preparation helper, so `trtexec.exe` is a build-time convenience and not a
runtime dependency. That is a design consequence to carry into M04/M15.

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
| The exact fp16 graphs the shim consumes | [`the-database/animejanai-inference`](https://github.com/the-database/animejanai-inference) @ tag `models-rife-fp16-1`, asset `rife-fp16-1.7z` | see lineage note below; archive pin and per-graph digests in [`../rife-acquisition.md`](../rife-acquisition.md) |

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
and the rights position: **`../rife-acquisition.md`**.

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

## Summary

| Component | Licence as located | Redistribution |
|---|---|---|
| Stock mpv core (Standard) | GPL-2.0-or-later | external — not vendored |
| mpv fork core (Native) | GPL-2.0-or-later **or** LGPL-2.1-or-later, per build options | **cleared**, with GPL-family obligations, once self-built from the pin |
| `aji.dll` / `aji_trt.dll` / `aji_dml.dll` | **none** | **blocked** |
| TensorRT runtime DLLs | NVIDIA TensorRT SLA + Supplement | external — acquired from NVIDIA, bundled under §1.2.1.x |
| `trtexec.exe` | unresolved | **blocked** (design removes the need) |
| `cudart64_*.dll` | CUDA EULA Attachment A | external — acquired from the CUDA redistributable |
| `onnxruntime.dll` | MIT | **cleared** |
| `DirectML.dll` | Microsoft Software License Terms (binary) + MIT (code) | **cleared**, on the licence read from the pinned package |
| AnimeJaNai ONNX models | CC BY-NC-SA 4.0 | **redistribution blocked**; local non-commercial use permitted |
| RIFE v4.14 full/lite/ensemble × {ensemble off/on} | MIT upstream — stored weight data traced; whole-graph redistribution unresolved | **blocked** — redistribution, not availability |
| Anime4K shaders | MIT (47) / Unlicense (2) | cleared — out of native scope, recorded for completeness |

**One rights blocker remains, and it is not paperwork.** §2 — the inference
shim has no licence of any kind, and the recorded non-commercial scope does not
change that. It binds the tasks that build, ship or execute the shim; the
stock mpv loader path, the contract and vocabulary work, and the import of
user-supplied models are untouched by it. §7 now blocks only the
*redistribution* of five model files, not their local use. §8 blocks the
*redistribution* of the four acquired RIFE graphs on whole-graph provenance and
rights, not their acquisition: the graphs are on disk and the stored weight data
traces to the author's MIT checkpoints.

The full blocker-to-task mapping is in
[`components.json`](components.json#blockertaskbindings) and in
[`../README.md`](../README.md).
