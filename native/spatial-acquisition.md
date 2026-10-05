# `native/spatial-acquisition.md` — AnimeJaNai spatial ONNX acquisition and provenance

Scope: the **five original spatial ONNX graphs** used as the read-only input to
M11's inspection/inventory work — how they were independently acquired, the
declared boundary and one CPU reference observation for each (§4), and what the
rights position is for exactly those bytes.

> **This is not a readiness claim.** The five graphs are on disk, pinned to an
> upstream commit and byte-verified. Each has since been loaded once by the M11
> inspection CLI and executed as a **CPU reference** at one stage geometry
> ([§4](#4-declared-boundary-and-cpu-reference-observation-m11-instrument)). That
> is a CPU execution-provider run — **not** a GPU/DirectML/TensorRT binding
> result, **not** an engine build, **not** activation. The graphs are **not**
> built into an engine, not activated, and not exposed to the app as Ready.
> Nothing here is on the Qt build path and nothing here is a runtime path.

## 1. Location

```
F:\ZKS\mikai-native-assets\spatial\
  onnx\                     the five graphs, as downloaded from upstream
  _meta\acquisition.json    machine-readable index of every fact below
  _meta\terms\LICENSE.txt   the upstream licence text relied on, verbatim
```

Outside the repository, matching the pattern of
[`native/sdk-acquisition.md`](./sdk-acquisition.md) and
[`native/rife-acquisition.md`](./rife-acquisition.md). Files under this root are
**read-only references**: `NativeModelInventory` copies a model's bytes into the
app-owned `native/models` store; it never writes beside these files, never uses
this directory as a cache, and never depends on it at runtime.

## 2. Independent acquisition

The graphs were downloaded anonymously over TLS from the upstream repository at
a **pinned commit**, not taken from any local package:

| | |
|---|---|
| Release | `the-database/mpv-AnimeJaNai` tag **3.6.3** |
| Commit pinned | **`d9cbaae5e92a56e71fdf4da5e063ae3e2611e5f7`** (`/git/ref/tags/3.6.3`, type `commit`) |
| Path in repo | `BuildMpvUpscale2xAnimeJaNai/mpv-upscale-2x_animejanai/animejanai/onnx/` |
| Mechanism | `raw.githubusercontent.com/<commit>/<path>/<file>`, anonymous, exit 0 for all five |

### Corroboration, not adoption

The hashes recorded in `native/manifest/sources.json`
(`sources[animejanai-onnx-models]`) are **reference observations**, not the
reproduction pin. The pin this record reproduces is the upstream commit above.
All five freshly downloaded files were hashed and every manifest value — digest
**and** byte count — reproduced exactly. The 3.6.3 package already present at
`F:/ZKS/janai/mpv-upscale-2x_animejanai-v3.6.3/` was hashed too and agrees; it is
corroboration only and is **not** a runtime or catalog dependency of this
repository.

### Freshly observed owned SHA-256

| File | Bytes | SHA-256 |
|---|--:|---|
| `2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx` | 1 971 970 | `b308e93c3dda3c4f9f968295b2b7b8d1c0583a711bca53a08d51134d9d1cbd5c` |
| `2x_AnimeJaNai_HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx` | 740 318 | `7b804d87320f37b7269d6e27796046fbd975e95e2aa1f65d5557f52a36282716` |
| `2x_AnimeJaNai_HD_V3.1Sharp1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx` | 1 971 970 | `dde763c8202cfebac5ec030ea6644f3ee339ea35bbda8a76e257405399552f0f` |
| `2x_AnimeJaNai_HD_V3.1Sharp1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx` | 740 318 | `72246eded3aab52f8d1a71613c0a1d900734b358d1820b62eb36f1ed376e1cf4` |
| `2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo.onnx` | 1 212 903 | `ded46591ddfaeb22b3daaf84ad7b36838335aa3efca66acfa36177ce4ab21eb5` |

## 3. Rights

Licence: **Creative Commons Attribution-NonCommercial-ShareAlike 4.0
International**, from the upstream repository's
[`LICENSE`](https://github.com/the-database/mpv-AnimeJaNai/blob/d9cbaae5e92a56e71fdf4da5e063ae3e2611e5f7/LICENSE)
at the pinned commit (`_meta/terms/LICENSE.txt`, SHA-256 `58032a81…c3d847`, git
blob `d60efe46d6b8428805b711f1c135af5e9fac6d25`). The graphs carry no embedded
licence header; this repository-level licence is the only evidence located.

Consequences: NonCommercial limits redistribution to non-commercial purposes;
ShareAlike requires adapted material to carry the same licence elements;
Attribution requires the creator identification, copyright notice, licence
notice, warranty disclaimer and a link to be retained; §2(b)(2) licenses no
patent or trademark rights.

These bytes are kept as **private, non-commercial reference inputs on this
machine only**. This repository does not redistribute, re-host or ship them.

**Licence position (2026-10-04, in-session):** The project's own integration is covered by the owner's unrestricted grant (recorded at `../LICENSE`, The Unlicense); these third-party CC BY-NC-SA 4.0 graphs are not relicensed by it and remain governed by their upstream terms as recorded. All original digests, pins and provenance tables are preserved unchanged.

## 4. Declared boundary and CPU reference observation (M11 instrument)

The five graphs were passed to `native_inspect` — M11's read-only inspection CLI
(`native/tools/native_inspect.cpp`), which loads the official ONNX Runtime C API,
reads each graph's declared I/O, and with `--resolve WxH` runs **one fp16 CPU
reference** at that geometry without touching the app's engine path.

Observed (parent, 2026-10-01) — all five exited 0:

| Graph | declared output batch | CPU reference run | verified profile |
|---|---|---|---|
| `…HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx` | `-1` (symbolic, preserved) | 64×64 → 128×128 | FP16 rank-4 N=1/C=3 → N=1/C=3 |
| `…HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx` | `-1` | 96×64 → 192×128 | FP16 rank-4 N=1/C=3 → N=1/C=3 |
| `…HD_V3.1Sharp1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx` | `-1` | 96×64 → 192×128 | FP16 rank-4 N=1/C=3 → N=1/C=3 |
| `…HD_V3.1Sharp1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx` | `-1` | 96×64 → 192×128 | FP16 rank-4 N=1/C=3 → N=1/C=3 |
| `…SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo.onnx` | `1` | 96×64 → 192×128 | FP16 rank-4 N=1/C=3 → N=1/C=3 |

(file names as in the §2 table above.) In every graph the declared input **and**
output are rank-4 `float16` with `C=3`; the run produced **N=1** at both ends,
and the HD declaration's symbolic output batch **N=-1 is preserved, never
clamped** — the concrete `N=1` lives only in the separate verified-reference
profile. The SD graph declares `N=1` outright.

**What this does and does not prove.** It proves the five graphs load through the
official ONNX Runtime C API, that each declares rank-4 FP16 input and output with
`C=3` (the HD output batch symbolic `-1`, SD `1`), and that one CPU reference
execution at the requested geometry produced concrete `N=1`/`C=3` at both ends —
for HD Balanced at 64×64→128×128 and for the other three HD graphs and SD at
96×64→192×128. It is a **CPU reference only**: not a GPU/DirectML/TensorRT
binding result, not an engine build, and not Native activation. Nothing here was
built, converted, re-exported, modified or added to the app; no model became
`Ready`, and no model is used as a runtime input. **M16's GPU engine-binding
proof remains the gate.**

## 5. What was and was not done

* **Done:** downloaded from upstream at the pinned commit, re-hashed, matched
  against the manifest's reference hashes, licence text stored, facts recorded in
  `_meta/acquisition.json`; one read-only CPU reference run per graph through the
  M11 inspection CLI (§4).
* **Not done:** no build into an engine, no Native activation, no GPU/DirectML/
  TensorRT binding verification, no conversion, no re-export, no modification of
  any source package (which stays read-only).
