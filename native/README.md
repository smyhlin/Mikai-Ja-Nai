# `native/` — Native Enhancement Provenance, Manifests & Component Store

This directory contains the provenance manifests, license notices, standalone CLI verification tools, patch series, and optional MSVC + CUDA + TensorRT build configuration for the **AnimeJaNai** and **Prepared Native** video enhancement tracks.

---

## 1. Architectural Overview: Two Enhancement Tracks

The application supports two distinct neural enhancement contracts alongside the built-in **Anime4K** GLSL shaders:

1. **Standard AnimeJaNai current official runtime (v0.17.1, API/ABI 9):** The Qt app's manager/configuration and runtime planning live in `src/app/dialogs/AnimeJanaiManagerPanel.cpp` and `src/player/native/components/AnimeJanaiConfig.{hpp,cpp}` / `AnimeJanaiRuntime.cpp`. It uses `vf_animejanai` in the pinned mpv runtimes alongside CUDA 13/TensorRT 11 dependencies. The `mpv-fork` source entry still identifies ABI-8 commit `63d127174d8f70336c82a22408929e2bc301edcb`, not the source identity for the current runtimes. The separate Windows DEV archive record identifies mpv commit `b4f9f5bfed18d5ce610af28024986410dd9bdaae`; the Linux archive is pinned by `cmake/MikaiLinuxMpv.cmake` and `packaging/provenance/linux-runtime.json`, but its build/source correspondence is not established. The updated Windows DEV archive contains ABI-9 host entrypoint name markers (`aji_ingest`, `aji_infer_seq`, `aji_temporal_radius`, `aji_temporal_reset`, `aji_rife_before_upscale`, `aji_pre_resize`, `aji_resize`, `aji_infer_rife`); these strings are marker evidence only and do not establish that entrypoints are exported or callable, executed behavior, hardware operation or inference. The Linux runtime is separately pinned, but matching ABI-9 markers have not been recorded for its binary. The official Linux and Windows runtime archives are distinct release assets; archive identity alone does not establish source correspondence, hardware operation or inference. The historical official v0.9.0 ABI-8 pair and separately patched private MSVC AJI ABI-8 build are not substitutes for this runtime. The separate Linux `aji-encode` archive (~80 MB) is an optional offline tool, not a playback dependency; it is not automatically downloaded or adopted, and its FFmpeg dependency and redistribution rights remain unresolved. User-defined model/profile setup is described in [Standard AnimeJaNai Custom Models & Profiles](#2-standard-animejanai-custom-models--profiles).
2. **Prepared Native Bundle Track (`mikai_native_support`, `NativePlaybackController`, `NativeBuilder`):**
   - Uses a versioned, manifest-verified bundle under `<nativeRoot>/runtime/<bundle-id>/` (`manifest.json` schema `mikai.native.bundle/1`) together with out-of-process preparation tools (`mikai_native_prep`, `mikai_native_verify`, `native_inspect`) that build and numerically verify TensorRT plans against an ONNX Runtime CPU reference before admitting them into playback.

---

## 2. Standard AnimeJaNai Custom Models & Profiles

The **Standard** AnimeJaNai integration is managed by the Qt application in
`src/player/native/components/AnimeJanaiConfig.{hpp,cpp}`,
`src/player/native/components/AnimeJanaiRuntime.cpp`, and
`src/app/dialogs/AnimeJanaiManagerPanel.{hpp,cpp}`. It is separate from the
optional MSVC shim build described in [`build-msvc.md`](./build-msvc.md):
the manager is part of the normal MinGW/Qt application build and does not
require recompiling that shim. For the GUI workflow, see
[AnimeJaNai Custom Models & Profiles](../docs/user-guide.md#optional-video-processing-and-components);
this section records the storage and runtime boundaries.

### Catalog, storage, and staging

The manager configuration is schema v3 at
`<nativeRoot>/animejanai/animejanai_manager.conf`. It stores Standard/Sharp
variant choices, logging and TensorRT settings, the optional external-model
directory, and custom slots 1–10. `.pconf` profiles and complete `.conf`
manager imports/exports retain schema-v3 slot keys, with
`mikai_chain_steps_version=1` for typed ordered ONNX/Anime4K steps. Legacy model
keys migrate on read. Profile edits and
manager settings are deferred until **Save**; custom model import, external
registration, and removal are immediate.

Catalogue observation is owned by
`src/player/native/components/AnimeJanaiCatalogWatcher.{hpp,cpp}`, a
QtCore-only `QObject` that emits `catalogChanged()` for filesystem activity
coalesced over 250 ms. It watches the app-owned custom model
directory (graphs and owned sidecars), the saved external directory or the
manager's unsaved override, the manager configuration, the external registry,
and every registered pin path. Directory observation is direct-child only:
nested subfolders and private Prepared Native bundle directories are not
searched. Missing, created, and recreated directories are re-armed, and atomic
replacements or in-place edits that preserve byte count and modification time
still signal. The observer compares metadata only (existence, byte count,
modification time, native metadata-change time, and directory entry names); it
never hashes, imports, registers, runs inference, compiles an engine, or
benchmarks. Consumers re-enumerate the existing catalogue APIs and refresh
already-open lists in place.

On Windows, bounded metadata reads and change-time queries share a read lock
with `commitMetadataFile`'s exclusive publication lock. Qt's atomic
replacement rejects open destination handles, even with delete sharing; closing
a reader before parsing alone does not prevent a concurrent worker commit from
racing it. Hashing, graph copying, JSON parsing and callbacks stay outside this
lock. POSIX retains its ordinary open-reader/atomic-rename behavior. Failures
caused by external processes remain explicit errors, not automatic retries.

User-imported ONNX files are copied into
`<nativeRoot>/animejanai/models/custom/`. External candidates remain in their
original directory; their explicit registrations (including the recorded
digest) are stored in the app-owned
`<nativeRoot>/animejanai/models/external-models.json`. A registered external
graph is borrowed: removing its registration does not copy, remove, or modify
the external file, and the file returns to the unregistered candidate list.
Importing a model or registering it does not grant Mikai any right to
redistribute that model. Users remain responsible for the applicable model and
weight licenses; files are not thereby vendored or licensed for redistribution.

A graph written directly into the app-owned custom directory without an
integrity sidecar is listed only by
`listUnregisteredOwnedAnimeJanaiModels()` as an unregistered candidate. It is
metadata-only and never executable: profiles, the player's spatial-model menu,
Compare, and Benchmark cannot use it yet. Selecting that candidate and using
the manager's **Import .onnx Model…** action registers it in place:
`registerOwnedCustomAnimeJanaiModel` validates and digests the graph on a worker
and publishes the `.onnx.source.json` record without renaming, moving, or
rewriting the pasted graph. An unregistered external candidate uses the same
action and `registerExternalCustomAnimeJanaiModel`; a raw paste is not silently
promoted to a preset. Unregistered candidates are not removable through the
manager's removal action.

When a profile references an imported or registered graph, runtime preparation
checks its recorded digest, stages it under the app-owned
`<nativeRoot>/animejanai/models/spatial/<sha256>.onnx` with its
`.onnx.source.json` provenance for model-only profiles. Mixed profiles instead
use content-addressed per-stage model directories, configs and stats under
`<nativeRoot>/animejanai/`. Consecutive ONNX steps share an AJI filter; each
Anime4K step becomes a real ordered libplacebo filter with its own input resize
and 1×/2×/4× output scale. Owned shader programs are staged atomically.
Software NV12 negotiation before Anime4K handles CUDA inputs and subsequent
AJI uploads; this route entails transfers and an 8-bit boundary, not guaranteed
zero-copy or HDR preservation. Shader-only profiles require no AJI closure.
Source-based activation uses original dimensions/FPS, never transformed output
or filename guesses. Stage cache paths remain stable across preparations;
process-local keyed leases prevent overlapping app-managed builds and are
released at terminal outcomes. Missing, altered, or otherwise unsupported model
references fail preparation rather than silently substituting another graph.
Enumeration is metadata-only; model hashing and import/registration I/O are
performed away from the GUI thread. This model staging/config path belongs to
the Standard AJI integration, not the Prepared Native bundle store at
`<nativeRoot>/runtime/<bundle-id>/`.

### Compatibility and configuration semantics

Imported-file validation checks that the file is a plausible ONNX protobuf
graph; it does **not** inspect tensors/operators or prove compatibility with
AJI. Filename-derived scale and architecture fields are hints only. A graph
must satisfy the AJI/backend input and operator requirements to run; arbitrary
ONNX files are not promised to work. External-weight packages are not imported
as multi-file bundles.

The manager supports custom profile IDs `builtin_slot:<1..10>` and imported model catalog IDs `spatial:custom:<id>`. A profile has ordered chains with resolution and FPS activation bounds. Chains
contain ordered model steps and optional pre-upscale height/percentage
resizing, and may include RIFE settings (model, interpolation ratio, scene
threshold, ensemble, and before/after-upscale position). Settings expose temporal
frame count (Auto, 1, or odd 3–15), scene threshold, upscale enable/disable and
temporal-fix controls. Sharp selects the corresponding HD model variant; it does
not replace the SD branch. Official v0.17.1 source maps RIFE code 4262 to
`rife_v4.26`, not `rife_v4.26_heavy`; this source mapping does not prove model
availability or inference. TensorRT configuration supports Static (match the
current video resolution), Static ONNX (fixed graph input shape), and Dynamic
(minimum/optimal/maximum shapes), plus builder optimization levels 0–5 and raw
`trtexec` arguments. The built-in Static/Dynamic shape helpers emit the TensorRT
input name `input`; selecting Dynamic does not inspect the ONNX graph to
discover a tensor name. Models with different input names need compatible raw
shape arguments, or Static ONNX when the graph's fixed input shape is
appropriate. These settings affect TensorRT engine preparation, not the
selected backend: backend choice remains a player/runtime choice. Logging and
TensorRT flags are not a claim of successful inference or a GPU performance
result.

### Schema-v3 field reference

`AnimeJanaiConfigStore` writes UTF-8 INI. The manager serializes only custom
slots 1–10, in numeric order; `.pconf` exports one unnumbered `[slot]` section
and import assigns it to the editor's selected slot. `C` and `M` below are
positive chain and model-step numbers, also serialized in numeric order.

| Section / prefix | Keys | Meaning |
|---|---|---|
| `[global]` | `config_version`, `logging` | Version `3`; logging is `yes` or `no`. |
| `[global]` | `external_models_dir` | Optional candidate-browsing directory; registrations remain in the separate app-owned registry. |
| `[global]` | `trt_engine_settings` | Global raw builder flags; an omitted default uses the application's default settings. |
| `[global]` | `quality_preset`, `balanced_preset`, `performance_preset` | `sharp` selects each built-in HD variant; omitted values use Standard. |
| `[slot_N]` / `[slot]` | `profile_name`, `description` | Profile identity and optional description. |
| `chain_C_` | `min_resolution`, `max_resolution`, `min_fps`, `max_fps` | Resolution/FPS activation bounds. |
| `chain_C_model_M_` | `name`, `resize_height_before_upscale`, `resize_factor_before_upscale` | Locally resolvable model name, height in pixels (`0` disables), and percentage (`100` leaves size unchanged). |
| `chain_C_` | `rife`, `rife_model` | Interpolation switch and AJI model code, for example `426` for RIFE 4.26. |
| `chain_C_` | `rife_factor_numerator`, `rife_factor_denominator`, `rife_scene_detect_threshold` | Interpolation ratio and scene threshold. |
| `chain_C_` | `rife_ensemble`, `rife_before_upscale` | `yes`/`no` ensemble and interpolation-order controls. |
| `chain_C_` | `upscale` | `yes`/`no` controls whether ONNX model steps are kept in the chain; when `no`, runtime preparation removes ONNX model steps, including a configured temporal-fix model, but leaves RIFE enabled if configured. |
| `chain_C_` | `temporal_fix_model`, `temporal_fix_scene_threshold`, `temporal_fix_before_upscale` | Optional temporal-fix model name, scene threshold (`0.0`–`1.0`, default `0.150`), and `yes`/`no` ordering before upscale. |
| `chain_C_model_M_` / ONNX `chain_C_step_S_` | `frames`, `temporal_scene_threshold` | ONNX model temporal window (`0` = detect from its input shape, `1` = single frame, or odd `3`–`15`) and scene threshold (`0.0`–`1.0`, default `0.150`). |

The manager store does not persist a backend selector. Runtime preparation
emits `backend=TensorRT` or `backend=DirectML` in the generated plan INI from
the selected backend. Editing a `backend` key in a manager import does not
change the player's backend selection. Generated plan INIs also replace model
references with staged SHA-256 stems; they are derived runtime artifacts, not
the editable source of the profile.

### Retired Model-Graph Experiments

The experimental FP8, 2:4 sparse, fusion, SpaceToDepth/RepConv folding,
one-channel luma, and RIFE 4.25 coordinate-specialization tools and dedicated
probe have been removed. No experimental graph tools, Python optimization
runtime or dedicated optimization probe remain; only production correctness
fixes were retained from this work. Historical measurements, hashes, methods
and limitations are in
[`bench_results.md`](../bench_results.md#rtx-5090--retired-model-graph-experiments-2026-10-18).
FP8 selectively helped throughput but also regressed a model. Sparse variants
gave small benefits with major output drift. Fusion helped one custom chain;
cubic pre-resize did not. Existing SPAN graphs had no eligible fold, new
SpaceToDepth/RepConv and luma designs lacked trained weights, and RIFE
specialization gains were marginal with overlapping ranges.
These are experiment results, not shipped capabilities or general speed/quality
claims.

The retained production builder diagnostic recognizes AJI's explicit
background-engine-build failure and includes a bounded `.engine.build.log`
error summary rather than waiting for the ten-minute timeout. Ordinary mpv
error-level chatter remains diagnostic-only. Only an authoritative builder
tail is frozen: source-error logs arriving after `END_FILE` remain available,
so the original file/open error is not replaced by generic chatter.

The existing `benchmark_probe --diagnostic-log-smoke <local-video>` command,
run from an owned test deployment, checks benign build chatter and a real
source failure without model inference or engine compilation.

### Distinction from Prepared Native

Standard AJI profiles configure the official AnimeJaNai ABI-9 filter/runtime (v0.17.1);
they are not Prepared Native recipes. `native_inspect` inspects ONNX inputs and
outputs and can run a CPU reference inference; `mikai_native_prep` builds a raw
TensorRT plan out of process; `mikai_native_verify` checks native plan
bindings/numerics; and the bundle tooling verifies a Prepared Native manifest.
Those inspection, preparation, verification, and bundle-admission steps are
distinct from AJI's profile configuration and model staging. Import validation
or successful AJI preparation must not be described as Prepared Native
verification, and neither path's documentation implies unobserved GPU inference
or engine performance.

## 3. Pinned Release Manifests (`native/manifest/`)

Release archives and model graphs are pinned by SHA-256 where indicated; source commits and trees use their recorded revision identities and do not by themselves prove binary correspondence. User-imported graphs have separate local digest records; they are not added to these release manifests:

| Manifest File | Schema / Purpose |
|---|---|
| `native/manifest/sources.json` | (`mikai.native.sources/1`) Pinned upstream identities and release asset digests, including `mpv-fork` (ABI-8 source commit `63d127174d8f70336c82a22408929e2bc301edcb`), Windows `mpv-author-winbuild` DEV archive (`2026-10-24`; separate source commit `b4f9f5bfed18d5ce610af28024986410dd9bdaae`), and `animejanai-inference` (official `v0.17.1`, historical `v0.9.0`); TensorRT SDK (`11.3.0.99` / CUDA `13.4`), ONNX Runtime DirectML (`1.24.4`), DirectML (`1.15.4`), 5 spatial ONNX models, and RIFE `v4.14` checkpoints. The Linux mpv archive pin is recorded in `cmake/MikaiLinuxMpv.cmake` and `packaging/provenance/linux-runtime.json`. |
| `native/manifest/components.json` | (`mikai.native.components/1`) Per-component licensing, provenance, and redistribution status (`cleared`, `external`, or `blocked` under `ESC-1..6`). |
| `native/manifest/animejanai-release.json` | Pinned Windows x64 (`component-trt-*.7z`, `component-rife.7z`) and Linux x64 (`component-trt-*-linux-x64.7z`, `component-rife-linux-x64.7z`) asset inventory and file mappings for [`the-database/mpv-AnimeJaNai` `3.7.0`](https://github.com/the-database/mpv-AnimeJaNai/releases/tag/3.7.0). |
| `native/manifest/videojanai-release.json` | Negative-control record for `VideoJaNai 2.1.0` (TensorRT `11.0.0.114` / CUDA `13.2.75`), documenting why cross-release TensorRT engines and builder resources are binary-incompatible with TensorRT `11.3.0.99`. |
| `native/manifest/THIRD_PARTY_NOTICES.md` | Full third-party license notices and per-file attribution for `mpv`, TensorRT, CUDA, DirectML, ONNX Runtime, Anime4K, RIFE, and 7-Zip. |

### Pinned `3.7.0` Component Packs (`NativeComponentCatalog.cpp`)

| Pack Name | Windows x64 Asset & SHA-256 | Linux x64 Asset & SHA-256 |
|---|---|---|
| `trt-runtime` | `component-trt-runtime.7z` (`186,822,775` B)<br>`282cb9b178db241f61f7aedf1ef214af56c2a342428253d04a8d8916d6729373` | `component-trt-runtime-linux-x64.7z` (`~251 MiB`)<br>`2d3a5a20008d752d3cd287fedd5f774baa6853068921bbd9b2add96742a7406b` |
| `trt-sm75` | `component-trt-sm75.7z` (`90,359,224` B)<br>`4bc46f2fd2e9aad1b90666e634334197a28348f24f9a1bdb0d07fadc59b3bc4f` | `component-trt-sm75-linux-x64.7z` (`~86.9 MiB`)<br>`ccaf69de0d99940d56553b8f31de4e1aeda6b9ce3f55d5509b56d4a2e07f4cdd` |
| `trt-sm80` | `component-trt-sm80.7z` (`160,207,044` B)<br>`5bfe7a9288e866baa8111c851634ec077572130527ebe8844a74109e6a0e5393` | `component-trt-sm80-linux-x64.7z` (`~154 MiB`)<br>`9ecdc3ed796f0509dbdc2b8420440bf7961e1a5b82b6aa51d64ddbd12e554258` |
| `trt-sm86` | `component-trt-sm86.7z` (`149,799,926` B)<br>`bdbdc7b92cc3143f76045263a08e506af4594e717763d118ac65404b7ba0b23a` | `component-trt-sm86-linux-x64.7z` (`~144 MiB`)<br>`a4d8b40e4974dd504d8b3a37ece656f6d29dfdda86b76065b70fa74f0aa33bf2` |
| `trt-sm89` | `component-trt-sm89.7z` (`158,554,965` B)<br>`34bd51be1cb1581db3e494d1b30732c926cd9ac1a340efabdf0f9148911fdc87` | `component-trt-sm89-linux-x64.7z` (`~152 MiB`)<br>`cd683e8afd41cf315fe6dc488a1c97dbcb5b5b7e5795893ece4079129d677871` |
| `trt-sm90` | `component-trt-sm90.7z` (`420,452,077` B)<br>`b884e983b8b0d61ecebf9990fbed6a3fa13023b5cf9b4c2920933777e5f5ceb5` | `component-trt-sm90-linux-x64.7z` (`~406 MiB`)<br>`3054e26228be0b2413af0d6d99aa7cc621178c4f902aed8a3bfc6160c57f7b9d` |
| `trt-sm100` | `component-trt-sm100.7z` (`251,528,952` B)<br>`98a4ab1a906123ba61db70cc470c13595b60cf94acde6625c53ad5556a58f447` | `component-trt-sm100-linux-x64.7z` (`~241 MiB`)<br>`71beeb07e3c1e15d7b8c7a27455a931f1c92a453d0d2fe6d6eb5052168270e9f` |
| `trt-sm120` | `component-trt-sm120.7z` (`229,417,183` B)<br>`954821ef65824b0c766998db7d2ae49c68041dac7f78328152ec41f674378c15` | `component-trt-sm120-linux-x64.7z` (`~220 MiB`)<br>`f3f52f355cbe9d7768a16ec2e974681ce56847aaed2f988150fd23f9c8074d2e` |
| `trt-ptx` | `component-trt-ptx.7z` (`208,391,718` B)<br>`7bd93d7f610ec31124b4ed677395e86337a0fbe9279d6315d7005d64f6dd398e` | `component-trt-ptx-linux-x64.7z` (`~199 MiB`)<br>`c1061e7b11cb865c4c1f7d1eea4bce7357f45e073b74bbf10916b3a72faba0f3` |
| `rife-3.7.0` | `component-rife.7z` (`462,444,035` B)<br>`d41de2b56972cb3c446d9830bd5c5e7257e6e099ef0e7a2d5770ec01a9bf7251` | `component-rife-linux-x64.7z` (`~441 MiB`)<br>`265797d56fb1376434658e6adf70ea5ee85b8ac4c51231fdd9e744d35462e136` |
| `aji-v0.17.1` | `aji-windows-x64.zip` (`743,177` B)<br>`9ddf6bb6da0ec52a2da0d9a13e59be99ff97a5c8c2e7156653dd154f6a4e40ad` | `aji-linux-x64.tar.zst` (`281,391` B)<br>`89ca6cc961889eebfa326745d2c7d9b96ebc71700303335f87adf1995f7c7cdc` |

### Component store ownership, pack membership and cleanup

`nativeRoot()` resolves `MIKAI_NATIVE_ROOT`, else an executable-adjacent
`native/` when that directory accepts a write, else `AppLocalDataLocation/native`
(typically `~/.local/share/Mikai/Mikai Wrapper/native` on Linux). The write probe
is a real create-and-remove, because an AppImage mount reports writable modes on
a read-only squashfs and a packaged prefix is root-owned. Path trust is anchored
at the resolved store's parent, so a data-directory store passes the same
ownership and reparse-point checks as an adjacent one.

Installed packs live in `components/tensorrt/<release>/` with a `component.json`
record naming each pack and the byte count and SHA-256 of every file it owns.
`storedPack()` answers existence and size; `verifyPack()` re-hashes and is what a
preset must consult before claiming a component is usable. A pack's declared
`files` list is therefore the store's contract, not a hint.

On Linux, `trt-runtime` lists the SONAME files the pinned 3.7.0 archive actually
contains (`libcudart.so.13`, `libnvinfer.so.11`, `libnvinfer_plugin.so.11`,
`libnvonnxparser.so.11`, `CUDA_LICENSE.txt`). It does **not** list `trtexec`:
the AnimeJaNai provisioning step copies `trtexec` once from the extracted runtime
archive (or from the store when the archive was not re-extracted) into the staged
AJI runtime, and recording it as a pack member made that second copy fail against
`QIODevice::NewOnly` with "trtexec could not be created". The Windows catalogue
has never listed `trtexec.exe` for the same reason: the prepared-helper path
builds its own.

Starting the application prunes abandoned work in this store:
`pruneNativeStore()` removes `scratch/` entries and `*.previous` backups older
than a day, plus `aji-stage-*` trees and `*.partial` copies older than one hour. Those are the only ages involved — a stage younger than an hour may belong
to another window's live install, so it is left alone. Installed components,
imported models, manager configuration and benchmarks are never touched. This is
what makes deleting the folder after an update unnecessary; it is not a repair
for a component that fails `verifyPack()`, which is re-acquired through Settings →
Components.

---

## 4. Rights & Redistribution Boundaries (`ESC-1..6`)

- Optional native/runtime bundles and acquired model graphs are not vendored in this repository or included in `cmake/dist.cmake` output. The distribution does include base third-party binaries such as the pinned `libmpv` and Qt/runtime files; see [`packaging/licenses/NOTICE.txt`](../packaging/licenses/NOTICE.txt) and [`packaging/third-party.json`](../packaging/third-party.json) for their obligations and publication blockers.
- `cmake/dist.cmake` strips any `components`, `assets`, `models`, or `records` directories from `dist-mikai/native/` and verifies their absence before completing. This does not mean that the application distribution contains no third-party binaries.
- **Escalation summary (`native/manifest/components.json`):**
  - `ESC-1`: Upstream `the-database/animejanai-inference` carries no repository license file (`license: null`). Local user-initiated acquisition into `<nativeRoot>/components/` is supported; public redistribution of compiled `aji*` binaries remains `blocked`.
  - `ESC-2`: The 5 spatial ONNX models (`models-2x_AnimeJaNai-1`) are licensed under **CC BY-NC-SA 4.0** (local non-commercial use permitted; bundle redistribution `blocked`).
  - `ESC-3`: RIFE `v4.14` weight tensors trace byte-for-byte (128/128 FP32→FP16 tensors) to `hzwer/Practical-RIFE` (MIT), while the ONNX exporter wrapper metadata is unattributed; graphs are acquired per-machine into `<nativeRoot>/components/rife/3.7.0`.
  - `ESC-4` *(Resolved)*: DirectML (`Microsoft.AI.DirectML 1.15.4`) and ONNX Runtime DirectML (`1.24.4`) licenses are verified (`LICENSE.txt` + `LICENSE-CODE.txt`).
  - `ESC-5`: `trtexec` is an NVIDIA SDK developer tool; the Prepared Native helper (`mikai_native_prep`) uses the in-process `nvinfer1` C++ builder API without invoking `trtexec`, while the official AnimeJaNai config path extracts `trtexec` only into the user's local component store.
  - `ESC-6`: Tracks GPL/LGPL complete-corresponding-source obligations for the base `mpv` runtime. The pinned Windows DEV and Linux bundle archives are binary releases, not corresponding source. Windows run 37965200722 configure logs are retained privately and establish a GPL-enabled non-LGPL libmpv build with static GPLv3 FFmpeg; exact dependency revisions/source-cache, all patch bytes and complete license closure remain unresolved. The Qt 6.12.0 Windows payload and compiler-runtime DLLs are inventoried by deployed path/hash/version. The official Qt source archive is also retained at `../release-artifacts/corresponding-source/qt-everywhere-src-6.12.0.tar.xz`, but its own manifest says complete corresponding source is false; SDK module/build mapping, modifications/scripts, distributor-controlled source delivery and Qt replacement/relink requirements remain open. See [`packaging/provenance/windows-runtime.json`](../packaging/provenance/windows-runtime.json), [`packaging/licenses/NOTICE.txt`](../packaging/licenses/NOTICE.txt) and [`packaging/third-party.json`](../packaging/third-party.json). This escalation remains open.

---

## 5. Verification & Maintenance Tools

- **`mikai_manifest_check`** (`native/tools/manifest_check.cpp`): Checks `native/manifest/sources.json` and `native/manifest/components.json` structure, identities, git SHA pins, ABI inventories and rights records. Recomputing source-file digests requires explicit read-only `--source-root id=directory` arguments; observation hashes are format-checked, never recomputed.
- **`mikai_bundle`** (`native/tools/bundle_tool.cpp`): Generates, inspects, and verifies `<nativeRoot>/runtime/<bundle>/manifest.json` against on-disk PE/ELF binaries.
- **`native_inspect`** (`native/tools/native_inspect.cpp`, enabled when `MIKAI_WITH_MODEL_INSPECTION=ON`): Inspects ONNX model inputs/outputs and executes CPU reference inference via ONNX Runtime.
- **Standalone MSVC + CUDA Build (`native/build-msvc.md`):** Instructions for compiling `aji`, `aji_trt`, `aji_dml`, `mikai_native_prep`, and `mikai_native_verify` with MSVC, CUDA 13.4, and TensorRT 11.3.0.99.

### Manifest checker exit codes

| Exit | Meaning |
|---|---|
| `0` | Source-file integrity and component identity checks passed; not bundle-loadability or inference proof. |
| `1` | Invalid identity/schema, digest mismatch, or cross-manifest inconsistency. |
| `2` | Required digest inventory or source root was not established. |
| `3` | A component is marked redistribution-blocked. |

Invalid data takes precedence over blocked rights, which take precedence over
incomplete integrity evidence. The current manifests contain blocked
components, so the basic command in [`AGENTS.md`](../AGENTS.md#out-of-band-cli-tools--manifest-verification)
returns `3`; without source roots it also reports unestablished file integrity.
This is an explicit refusal, not a packaging clearance. User-local acquisition
and functional tests do not remove redistribution blockers.
