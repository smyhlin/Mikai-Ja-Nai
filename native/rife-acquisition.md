# `native/rife-acquisition.md` — RIFE v4.14 asset acquisition and provenance

Scope of this record: what was **downloaded, byte-verified, extracted and
structurally inspected** for M12 (`rife-v4.14-assets` in
`native/manifest/sources.json`), and what the **rights position actually is**
for those specific bytes.

> **This is not a readiness claim.** All four graphs this combination space
> reaches are on disk, pinned, and their internal structure verified — and
> their stored weight tensors match the author's own v4.14 / v4.14.lite
> checkpoints byte-for-byte after fp16 rounding ([§5](#5-lineage--weight-data-traced-exporter-not)).
> All four graphs have since run a **real CPU reference** (one genuine 64×64 PPM
> pair, 5 451 differing samples, the source 11-channel packing and `t = 0.5`) and
> all four were **serialized through the real TensorRT builder API**, outside any
> playback path. `mikai_native_verify` splits the matrix against the unchanged
> `0.02`/`0.005` bounds, with no overrides: **lite passes** (`maxAbsoluteError`
> `0.0122070312`, RMSE `0.000512728931`) and **lite ensemble passes**
> (`0.00341796875`, `0.000227462237`), while **full is rejected**
> (`maxAbsoluteError` `0.0561523438`, RMSE `0.00198529105`) and **ensemble is
> rejected** (exit `9`; `maxAbsoluteError` `0.0358886719`, RMSE `0.00104451097`).
> A pass covers raw engine bytes, bindings and numerics only —
> `manifestVerified: false`, `prepared: false` — so **no model is `Ready` or
> `Active`** and no bundle is assembled. M12 is **partially satisfied, not done**.
> What remains open as a provenance question is exporter identity and the
> non-weight graph material — those residuals are unchanged.
>
> **Licence basis (2026-10-04, in-session).** The project owner granted unrestricted use of the project's own native integration (recorded at `../LICENSE`, The Unlicense). For third-party artefacts: upstream licence status remains as located. The inference shim source had no located upstream licence - that fact remains recorded; the owner determination is the basis for the project's use/recording of its own integration work as documented in `manifest/components.json` (ESC-1, ESC-3). Third-party models/graphs keep their own upstream terms; no relicensing is implied. This record preserves all original digests, pins, provenance tables and weight-comparison evidence unchanged.

## 1. Location

Root: **`F:\ZKS\mikai-native-assets\rife\`** — outside the repository and
outside every tracked source tree, matching the pattern set by
[`native/sdk-acquisition.md`](./sdk-acquisition.md). Nothing here can be
committed or accidentally vendored.

```
F:\ZKS\mikai-native-assets\rife\
  _downloads\rife-fp16-1.7z   322 142 263 B  the original archive, re-verifiable
  onnx\                       33 543 434 B  the four graphs this combination space reaches
  original-checkpoints\               the author's own v4.14 / v4.14.lite checkpoints
                                      and the weight-comparison report (§5)
  _meta\acquisition.json              machine-readable index of everything below
  _meta\terms\                        the five licence texts relied on, verbatim
```

## 2. The archive

The exact upstream asset pinned in the manifest, taken from the primary URL.

| | |
|---|---|
| URL | `https://github.com/the-database/animejanai-inference/releases/download/models-rife-fp16-1/rife-fp16-1.7z` |
| Release tag | `models-rife-fp16-1` — *"RIFE fp16 models (set 1)"* |
| Response | `200`, `Content-Type: application/x-7z-compressed` |
| Authentication | none — the whole chain is anonymous |
| Bytes | **322 142 263** — GitHub's published `asset.size` and the measured transfer size agree |
| Published SHA-256 | `3fcf3429f3bb661d15c59aa58ed7ba28c0ca7f79e2388f8e0165dd8bbe0648f9` |
| **Measured SHA-256** | `3fcf3429f3bb661d15c59aa58ed7ba28c0ca7f79e2388f8e0165dd8bbe0648f9` — **matches** |

Unlike the vs-mlrt assets in the same manifest (whose GitHub `digest` is
`null`), this release publishes a real digest and the local file reproduces it
exactly. The pin is therefore genuine, not a locally-invented baseline.

The archive holds **47 entries**, matching the release note's own claim of
*"47 models (rife 4.7-4.26 incl. lite/heavy/ensemble variants), 1011 MB -> 507 MB
on disk."*

## 3. Extraction — the reachable name space

No 7-Zip is installed on this machine, but `C:\Windows\system32\tar.exe`
(bsdtar) reads this 7z container, so the four members were named individually
and nothing else was unpacked.

| File | Bytes | Measured SHA-256 |
|---|--:|---|
| `rife_v4.14.onnx` | 10 778 647 | `a64b9cd0fb463d620688394437f35f02e33c0658c39ccce2b9637899fb520cbe` |
| `rife_v4.14_lite.onnx` | 5 986 311 | `948019ffb5e7e54ac6b89a13542da8d100af9b4ade7374551c5f595ac3ff2ce7` |
| `rife_v4.14_ensemble.onnx` | 10 785 406 | `ec64f92e31a1b61c034b4de6f1afc1c2aeaf478b93dbf0a6ce2e69a327385b53` |
| `rife_v4.14_lite_ensemble.onnx` | 5 993 070 | `9486d87ca17a83a1594ac54b254b6fb1be6fa52f5dd11a31e4a602d124ea8b1b` |

**No name here is inferred from a filename** — each is what the
consumer actually asks for. The derivation is public source, read directly:
[`src/aji_trt.cpp:932-946`](https://github.com/the-database/animejanai-inference/blob/main/src/aji_trt.cpp#L932-L946)

```cpp
// rife model code -> file basename: 414 -> rife_v4.14, 4141 -> _lite,
// ensemble appends _ensemble (rife_cuda.py's mapping).
std::string rife_model_name(int code, bool ensemble)
{
    std::string s = std::to_string(code);
    ...
    std::string name = "rife_v" + s.substr(0, 1) + "." + dec;
    if (s.size() == 4 && s.back() == '1')
        name += "_lite";
    if (ensemble)
        name += "_ensemble";
```

This was independently corroborated against the **shipped binaries** rather than
only the repository: the format-string pool of
`animejanai/inference/aji_trt.dll` and `aji_dml.dll` contains, contiguously and
in order, `rife_v`, `_lite`, `_ensemble`, `1x11x%dx%d` and `.onnx`, plus
`invalid rife model code %d`. `1x11x%dx%d` is the `--optShapes=input:…` template
the backend builds engines with.

The two suffix branches are **independent, not exclusive** — `_lite` is keyed on
the code's shape, `_ensemble` on a separate boolean, so they compose:

| code | ensemble | emitted basename |
|---|---|---|
| `414` | no | `rife_v4.14` |
| `414` | yes | `rife_v4.14_ensemble` |
| `4141` | no | `rife_v4.14_lite` |
| `4141` | yes | `rife_v4.14_lite_ensemble` |

That is the full {Full, Lite} × {ensemble on/off} space M12's typed contract
reaches, so **all four graphs are extracted**. `rife_v4.14_lite_ensemble.onnx`
is an original supported combination of these upstream weights, not a
substitute standing in for a missing asset.

## 4. Structural verification (no Python, no ONNX runtime)

Each file was parsed as a protobuf directly (varint/length-delimited field
walk) to read the real graph, not its filename. Findings, identical across all
four unless stated:

- `ir_version 8`, opset `ai.onnx v16`
- exactly **one graph input named `input`** and **one output named `output`**,
  both `elem_type = FLOAT16` — the fp16 typing the backend feeds
- **128/128 initializers are `float16`**; no float32 initializer survives
- **4 `GridSample`** nodes fed by `Transpose`-built grids, with **8 Cast→float32**
  nodes: the documented **fp32 sampling-grid island is actually present** in
  the bytes, which is the release note's central claim
- 4-D dynamic NCHW input/output — the `1x11xH xW` shape is supplied at engine
  build time, not baked in
- conv weights run `block0.conv0.0.0.weight 96x23x3x3` →
  `encode.cnn3.weight 32x8x4x4`; distinct output channels {32,48,64,96,128,192};
  4 flow blocks

The variants are **genuinely different graphs**, which is the check that matters:

| | full | lite | full + ens | lite + ens |
|---|--:|--:|--:|--:|
| nodes | 260 | 260 | **334** | **334** |
| fp16 elements | 5 364 312 | **2 968 152** | 5 364 312 | **2 968 152** |
| distinguishing | — | same topology, ~55% of weights | +`Sub`(6) `Div`(8) `Neg`(3) | same merge math over lite weights |

`lite` sharing the full topology at ~55% of the weight bytes is exactly the
author's *"similar training framework, but lower computational cost model"*;
`ensemble` adding merge ops while keeping the base weights matches an ensemble
of that base. `lite_ensemble` composes both — lite's weight bytes under the
ensemble merge ops — so the bytes themselves corroborate that the two suffixes
are independent axes. Names, bytes and structure agree.

## 5. Lineage — weight data traced, exporter not

**Established, from primary sources:**

1. **Original weights.** `hzwer/Practical-RIFE` at pinned commit
   **`de9a989bb7b8a71d94f058297e603633aaa43ad6`**. Its README at that commit
   carries the grant sentence verbatim under *Trained Model*: *"The content of
   these links is under the same MIT license as this project."*, and its v4.14
   rows link the exact checkpoints:

   | Release | Drive file id | Link |
   |---|---|---|
   | 4.14 (base) | `1BjuEY7CHZv1wzmwXSQP9ZTj0mLWu_4xy` | [Google Drive](https://drive.google.com/file/d/1BjuEY7CHZv1wzmwXSQP9ZTj0mLWu_4xy/view) |
   | 4.14.lite | `1eULia_onOtRXHMAW9VeDL8N2_7z8J1ba` | [Google Drive](https://drive.google.com/file/d/1eULia_onOtRXHMAW9VeDL8N2_7z8J1ba/view) |

   The repository `LICENSE` at the same commit is MIT, `Copyright (c) 2021 hzwer`.
2. **The conversion.** `the-database/animejanai-inference` release note states
   the input was *"vsmlrt v1 (video_player) RIFE models converted to fp16 with
   the GridSample sampling-grid math kept in an fp32 island
   (`tools/convert_rife_fp16.py`)"*. The converter's own docstring says it
   converts *"the vsmlrt RIFE v1 (video_player) ONNX models"*, and its logic
   matches the structure found in §4 — it keeps only the grid-builder chain
   fp32. The conversion step introduces no new weights, only casts and nodes.

### 5.1 Weight data, compared directly

The hop this record previously carried as open — *which* weights the v4.14
vs-mlrt/fp16 graphs hold — was tested directly rather than inferred. The
author's own checkpoints were acquired outside the repository (§1) and compared
tensor by tensor against `ModelProto.graph.initializer` of all four graphs.

Local acquisition (`original-checkpoints/`). **These are local measurements of
the received files, not upstream-published pins:** Google Drive publishes no
digest, so the Drive file ids above are the only upstream identity for these
checkpoints.

| Checkpoint | Zip bytes | Zip SHA-256 (local) | Member |
|---|--:|---|---|
| `RIFEv4.14.zip` | 19 979 922 | `077f200018a81aae067dbbeaefef855f37f7e40b1655ed28e2cde728fdee0514` | `train_log/flownet.pkl` (21 508 207 B) |
| `RIFEv4.14.lite.zip` | 11 068 470 | `b6fe29b1d8032287826ce14886b7da3f8cb8362344eaf7eed29532d4b48e266f` | `train_log/flownet.pkl` (11 923 567 B) |

The inspection was **inert**: the ZIP, torch-pickle and ONNX walks are
hand-written field walks. Pickle opcodes were parsed as inert data; **no
referenced callable, module, Python runtime or torch runtime was executed.**

Observed (parent, 2026-10-01):

- Each checkpoint is a torch ZIP whose `flownet.pkl` object carries **128
  float32 tensors** — 5 364 312 elements (base) / 2 968 152 (lite) — plus the
  `_metadata` dict (152 entries) that `Module.state_dict()` attaches. That
  attribute is reported as an attribute and is **not** counted as a tensor slot,
  so no key is silently dropped. All 128 keys carry the single leading `module.`
  prefix the shipped `train_log/RIFE_HDv3.py` `Model.load_model` strips
  (`k.replace("module.", "")`); nothing else was renamed.
- On the graph side only `graph.initializer` was decoded: 128 float16 tensors,
  5 364 312 / 2 968 152 elements, initializer-set digests
  **`2d45e2fe0201af75ceda7e363fab853506553272e05744265b1bf8baaaafc5f3`**
  (base and ensemble) and
  **`3f086c6223572fc6854f4fa9ba8ce7dc9191edd85bf455208c807943394c2fe8`**
  (lite and `lite_ensemble`).
- **Four comparisons, all exact:** base→`rife_v4.14` and base→
  `rife_v4.14_ensemble`, lite→`rife_v4.14_lite` and lite→
  `rife_v4.14_lite_ensemble`. Each matched **128/128** names and shapes with
  FP32→FP16 round-to-nearest-even bytes identical — zero mismatched, zero shape
  mismatched, zero missing on either side, zero unmatched graph initializers.
  The base and ensemble initializer sets are identical. This does not prove
  the derivation of their different graph operators.

So the stored weight tensors in all four graphs **are** the author's v4.14 /
v4.14.lite weights rendered in fp16 — established by comparison, not by an
inference about which tool exported them. Machine-readable result:
`original-checkpoints/weight-comparison.json` (`schemaVersion` 1), written by
the data-only static inspector `original-checkpoints/compare-weights.mjs`
(exit 0).

### 5.2 Still open

- **Exporter identity and non-weight graph material.** The comparison reads
  only `ModelProto.graph.initializer`; nodes are skipped by length and never
  interpreted. It proves nothing about which exporter produced the graph
  structure — the fp16 GridSample fp32 island, the Transpose-built grids, the
  cast placement, the ensemble merge math, or the fp16 conversion path. No
  upstream statement has been located naming the exporter of these specific
  files; the vs-mlrt release assets still publish `digest: null` and carry no
  per-file licence, and the publishing repository declares no licence of its
  own.
- **Redistribution rights.** Byte correspondence between checkpoint tensors and
  graph initializers is evidence about weight data only. It says nothing about
  who may redistribute the graph artefacts, and the tool's own verdict records
  exactly that — `licenseReady: false`: *"Not asserted. This tool only compares
  stored weight bytes; it cannot and does not establish exporter identity, graph
  provenance, or any redistribution right."*

This is **not** a claim of legal incompatibility, and the direct match is not a
grant. `_meta/acquisition.json` records the exact comparison and remaining
exporter/whole-graph rights limits.

## 6. Rights position

| Question | Answer |
|---|---|
| Documented grant on the **original** v4.14 weights | **MIT**, `Copyright (c) 2021 hzwer`, granted by the author's README statement + repository LICENSE |
| Whether that grant reaches **these converted graphs** | **Weight data: established (§5.1). Graph artefacts: not.** Every stored weight tensor in all four graphs matches the author's own MIT-licensed v4.14 / v4.14.lite checkpoints after fp16 rounding, so those weight bytes are the author's MIT weights. The files *as artefacts* additionally carry exporter-produced structure with no per-file notice, and §5.2's exporter hop is unclosed, so the grant cannot be asserted over the file as a whole |
| Redistribution approval | **Cleared per owner determination** (ESC-3, `components.json#components[id=models.rife-v4.14].clearanceBasis`): on the weight-data provenance already established, the rights owner has determined the residual whole-graph questions are acceptable for this project. The residuals below are accepted, not answered. |
| Redistribution of *these converted graphs* | **Owner-determined as acceptable.** The weight data traces to the author (§5.1); the files as artefacts still carry exporter-produced material with no per-file notice, and the owner has accepted that residual for this project. No upstream grant covers the file as a whole, and none is claimed. |
| Ensemble specifically | No ensemble weight artefact exists in Practical-RIFE's published links. Each ensemble graph's initializer set is byte-identical to its base graph's and matches the same checkpoint (§5.1); this does not establish its graph-operator derivation or a separately published checkpoint |

Private scope and provenance are separate axes and the first does not settle the
second. The weight-data hop is closed by direct comparison (§5.1) — those
weight bytes are the author's MIT weights — but the files as whole artefacts
still carry exporter-produced material whose upstream chain is unnamed, and no
upstream grant covers redistributing them. Provenance of the weight bytes is
established; the residual whole-graph facts remain unknown, and the project
owner's determination (ESC-3) is what supplies the basis for this project rather
than any located upstream grant.

Verbatim licence texts are preserved in `_meta/terms/` with their own digests
(`Practical-RIFE-LICENSE.txt`, `Practical-RIFE-README-model-grant.txt`,
`ECCV2022-RIFE-LICENSE.txt`, `vs-rife-LICENSE.txt`, `vs-mlrt-LICENSE.txt`).

Note the asymmetry worth being precise about: the **weights** are MIT by the
author's statement; the **vs-mlrt code** is GPL-3.0-only. No vs-mlrt code is
shipped here — only the ONNX *output* of its export is consumed, and whether a
code licence extends to exported model files is not settled by the licence text
and is not asserted here.

## 7. What is still not established

To move these assets from *acquired* to *Ready*, in order:

1. **Exporter identity and the non-weight graph material** (§5.2) — an upstream
   statement naming the exporter of these specific v4.14 graphs, or manifest
   route B (convert the author's own checkpoints with this project's toolchain),
   which would make the graph's construction this project's own act.
2. **Engine-build and runtime proof**, which is M24/M44's work, not this
   record's. A weight comparison is not an execution. What has since run: all
   four graphs on a **real CPU reference** (one genuine 64×64 PPM pair, the source
   11-channel packing, `t = 0.5`), and all four **serialized through the real
   TensorRT builder API** outside any playback path. Two of those engines — RIFE
   lite and lite ensemble — **verify numerically** against the reference; the full
   and ensemble engines are **rejected** on `maxAbsoluteError`. A verification is
   still not readiness: each passing engine reports `manifestVerified: false`
   and `prepared: false`, so no model is `Ready` or `Active` and no bundle is
   assembled.

## 8. Reproduction

```sh
# in _downloads/
curl -sSL -o rife-fp16-1.7z \
  "https://github.com/the-database/animejanai-inference/releases/download/models-rife-fp16-1/rife-fp16-1.7z"
sha256sum rife-fp16-1.7z     # 3fcf3429f3bb661d15c59aa58ed7ba28c0ca7f79e2388f8e0165dd8bbe0648f9
tar -xf rife-fp16-1.7z -C ../onnx \
  rife_v4.14.onnx rife_v4.14_lite.onnx \
  rife_v4.14_ensemble.onnx rife_v4.14_lite_ensemble.onnx
```

The author's checkpoints are downloaded from the README's Drive links (file ids
in §5.1) into `original-checkpoints/`. Because Drive publishes no digest, the
zip hashes in §5.1 are **local measurements of the received files**, not
upstream pins; the only upstream identity is the Drive file id. The comparison
itself is reproduced by the data-only inspector sitting beside them (no torch,
no Python, no ONNX Runtime; `--out` selects the report file):

```sh
cd original-checkpoints/
bun compare-weights.mjs --out weight-comparison.json   # exit 0 = every pair exact
```

`_meta/acquisition.json` still carries per-file digests, sizes and graph
structure for the four graphs. Its `provenanceLinkage` lineage text is the
pre-comparison phrasing and is refreshed by the parent integration step.

## 9. Separate acquisition: AnimeJaNai 3.7.0 RIFE component archive

This section records the Windows `component-rife.7z` asset from the official
AnimeJaNai 3.7.0 release. It is a separate acquisition from the historical
RIFE v4.14 archive and does not supersede or alter the v4.14 provenance above.

| | |
|---|---|
| Release | [AnimeJaNai 3.7.0](https://github.com/the-database/mpv-AnimeJaNai/releases/tag/3.7.0) |
| Asset | [`component-rife.7z`](https://github.com/the-database/mpv-AnimeJaNai/releases/download/3.7.0/component-rife.7z) |
| Published size | **462,444,035 bytes** |
| Published SHA-256 | `d41de2b56972cb3c446d9830bd5c5e7257e6e099ef0e7a2d5770ec01a9bf7251` |
| Locally verified download | `F:/ZKS/mikai-native-assets/rife/_downloads/component-rife-3.7.0/component-rife.7z` |
| Extracted inventory | **47 ONNX models**, `extracted/animejanai/rife/`, **506,592,464 bytes total** |
| Per-file integrity pins | `rifeModelPins()` in `src/player/native/models/NativeRife.cpp`; the pinned size and SHA-256 for each model match the archive extraction |

The user-initiated store verifies the archive's published size and SHA-256 and
verifies each extracted graph's size and SHA-256 before publish. Stored
inventory destination is `<nativeRoot>/components/rife/3.7.0`. This is a
per-machine inventory/store only: its presence does **not** enable RIFE
inference and is not a `Ready` or `Active` claim.

This acquisition does not resolve third-party rights. The legal/redistribution
position remains unresolved under ESC-3: no clearance to redistribute the
archive or its models is claimed here. The v4.14 acquisition and its weight
provenance above remain distinct and unchanged.
## 10. Current local component and plan observations

The official AnimeJaNai 3.7.0 `component-rife.7z` archive was acquired,
verified, and imported with metadata. All 47 graph names and metadata hashes
were compared with the archive. The compiled v4.18 ensemble hash pin alone was malformed at
55 characters and was corrected to the exact observed full SHA-256 digest
`69c46eb0cf3c1874ca5278758a8e7c4df1b70d0feaa682ce895459b7a6fa9716`
(10,785,406 bytes); the other 46 names, sizes and hashes matched the acquired
archive. The full-package inference folder is handled by the official
hash-dispatcher without querying; custom graphs require observed ABI 8.

A current plan-only smoke prepared `prepareAnimeJanaiPlan` for all 47 graphs,
including RIFE v4.26 heavy, with exact backend/model code/ensemble flag and
verified graph records. The filename boundary for `rifeModelName` was fixed.
Two-digit ABI model codes 47/48/49 decode correctly; 407 decodes incorrectly
as 4.07. No engine build was requested by this smoke. A temporary catalog
lifetime issue that crashed a heavy test was fixed. Fresh spatial and RIFE v4.14
lite raw TensorRT engines were built and loaded through Qt/libmpv. Spatial
attempt a44 observed raw build and 640× output/render logs but failed CRC
filename filtering and an early raw-output read of 320 before configured-slot
Core property reconfiguration. Corrected spatial a46 (artifact 367, 66.47
seconds) passed for the selected model's fresh raw build (15.7249 seconds,
2.98863 MiB) and render/control run; its raw engine SHA-256 is
`0a3fd65a1de1abe5507a996ee571abfebef8eda3294046a195b91b4648c759c9`
and full trace `.probe-out/natural-spatial-a46/qt-flow-proof.log`.
its nonblack changed/moving output and controls passed, with 73 postfilter
frames/3 seconds and zero decoder/VO drops. This does not establish all-47
inference, numerical fidelity, private Ready/Active, or FPS improvement.

Artifact 349's initial RIFE probe failed because its request-bound scan searched
the spatial `plan.modelDir`/wrong model identity and used `time-pos` transitions
instead of rendered-frame counts. The corrected RIFE run (artifact 366, 88.63
seconds, stage 7) passed for fresh selected v4.14 lite. Its graph SHA-256 is
`948019ffb5e7e54ac6b89a13542da8d100af9b4ade7374551c5f595ac3ff2ce7`. The
request-bound raw engine built in 38.932 seconds (9.12264 MiB; 9,565,780 bytes),
SHA-256
`e1a4421a3634df8fedaf9d45f1ecc5120aa371e41beb26612ffd7fdfa98dee22`; retained
under `.probe-out/natural-rife-a45/native/animejanai/models/rife/`, with full
trace `.probe-out/natural-rife-a45/qt-flow-proof.log`. The selected-engine
directory began empty; the build footer matched exact ONNX and
`saveEngine`, and no prebuilt engine participated. Stats reported RIFE active at
`2/1`, `NewVideoFPS48`, padding/crop `320x180→320x192→320x180`, with actual
320x180 NVDEC output.
Successful `MPV_RENDER_UPDATE_FRAME` surface updates over three seconds were
73 baseline versus 145 processed at equal resolution; the 1.5x threshold
passed. The processed image was nonblack, changed, and moving; pause/seek,
volume/mute, audio/subtitle cycle, Off restoring hwdec, source reopen and
shutdown-surface checks passed. Engine-to-active-model identity remains an
inference because Core has no engine-ID event. This run does not establish
numerical fidelity, all-47-model inference, private Ready/Active, or an FPS
performance improvement.

These component-archive observations do not alter the historical v4.14
provenance or resolve ESC-1/2/3. The imported graph inventory and a plan-only
result are not numerical verification, readiness, Active processing, or
redistribution clearance.
