# mikai_janai — Native AnimeJaNai integration: implementation tasks

Companion to [`plan.md`](./plan.md). `plan.md` carries the architecture,
contracts, decisions and source evidence; this file is the **sole execution,
status and acceptance record**. A task that contradicts `plan.md` is a defect in
one of them, not a choice.

---

## 0. How to read this file

- **Every implementation checkbox stays unchecked until its acceptance is
  observed.** A scaffold, a worker handoff, a compile or a green hermetic suite
  is not acceptance. `Status` lines record only what was actually run.
- **`Depends`** lists genuine implementation prerequisites. Pure contracts,
  planning, ownership logic, the locally owned native implementation and the
  Standard runtime path do not depend on redistribution clearance or a finished
  Native bundle; shipping a bundle does.
- **PROPOSED** marks a file, target, CLI, settings group or probe that does not
  exist yet. Everything else in a `Files` mention is an existing path cited by
  real symbol names. Reference trees outside this repo
  (`C:/Users/myhli/Downloads/janai/…`, `F:/ZKS/janai/…`) are **read-only**.
- **Blocking prerequisites are facts.** A missing SDK, licence, asset or
  hardware capability is a stop — never a silent pass, never a placeholder
  presented as a feature.
- Verification names the **actual** command. Anything PROPOSED does not exist
  until its own task lands.
- Stable anchors precede every heading so `plan.md`'s links cannot drift:
  `#m01` … `#m52` for the 52 tasks, `#ws-a` … `#ws-h` for the eight
  workstreams, `#catalog`, `#catalog-m`, `#catalog-c`, `#catalog-r` and
  `#catalog-n` for the acceptance catalog, `#c01` … `#c22` for the C ledger, and
  `#dag`, `#dag-interfaces`, `#dag-waves`, `#dag-critical-path`, `#workstreams`,
  `#batches` and `#status` for §1.

---

## 1. Workstreams, batches, dependencies and the critical path

<a id="dag"></a>

This section is the **single authoritative schedule**: the eight cohesive
workstreams, the next runnable batches, the shared interfaces, the dependency
depth and the critical path. `plan.md` links here and never restates a second
copy.

### 1.1 Eight workstreams

<a id="workstreams"></a>

The 52 implementation packages `M01`–`M52` are grouped into eight **cohesive
jobs**. A workstream is a unit of ownership, not a spawn schedule: it has one
integration owner, one interface/output, one **primary file area** and one merge
gate. Tasks inside a workstream share its owner and merge wave; they are not one
job per field, per milestone or per acceptance checkbox.

**File-owner contract.** Within a merge wave at most **3–4 workers** run
concurrently, each with an **exact file lease** agreed for that batch. The table
below names **primary file areas**, not exclusive ownership; two workstreams that
touch the same area are sequenced or given one integration owner before
fan-out. The merge-wave parent owns the final source gate: one build, the CPU
matrix, a representative actual-changed-path check and a full integration run at
the major boundary. Workers never build, lint, test or format mid-flight; the
parent runs those once after all slices land. Shared prerequisites — in
particular shared protocol/header changes — have **one owner** agreed before
fan-out. GPU-bound tasks (M43, M44, M45, M48, M49) run **serially and
exclusively** even where no `Depends` edge orders them.

| WS | Workstream | Tasks | Owner interface / output | Primary file area | Merge gate |
| --- | --- | --- | --- | --- | --- |
| **A** | Runtime packaging and ABI | M01–M10 | `MpvApi` + `Bundle` + typed model/recipe contracts (contracts co-owned with B) | `src/player/MpvApi.*`, `src/player/native/NativeBundle.*`, `native/bundle/`, `native/tools/bundle_tool.cpp`, `native/tools/manifest_check.cpp` | one build + full CPU matrix + representative changed-path + full integration |
| **B** | Models, ordered preparation and cache | M11–M18 | `NativeCatalog`, `NativeRecipe`, `NativePreparationPlan`, `NativeManifest`, `NativeBuilder` | `src/player/native/Native{Types,ModelInspect,Catalog,Recipe,Preparation,PreparationPlan,Manifest,Builder,Rife,FrameReference}.*` | prepared-artifact identities and numerics verified |
| **C** | Inference, metadata and filters | M19–M25 | filter-local `GET_META` snapshot, epochs, execution paths | `native/patches/`, `src/player/native/Native{Status,Capabilities,DirectML}.*` | filter snapshot + spatial/RIFE/combined execution |
| **D** | Activation, source, PiP and lifetime | M26–M30 | `NativeController` requested/effective state machine | `src/player/native/NativeController.*`, `src/player/PlayerDialog.*` | acknowledged epoch + lifetime release |
| **E** | Player UI, shaders and settings | M31–M38 | menu/prefs/shortcuts/HUD/i18n on the existing surfaces | `src/player/{CompareWindow,PlayerShell,Anime4K}.*`, `src/app/dialogs/*`, `translations/*` | UI, geometry and language acceptance |
| **F** | Media recovery and trust | M39–M40 | failure classification + trust-boundary guards (cross-cutting; changes assigned to the owning source files, no independent writer) | `src/backend/*`; failure-classification path in `src/player/native/*` co-owned with C/D | negative matrices |
| **G** | End-to-end correctness, fidelity and performance | M41–M49 | probes and measured proof | `src/probes/*`, `tests/cpp/*` | measured framebuffer/cadence/perf evidence |
| **H** | Distribution, docs and release | M50–M52 | coherent bundle, install/upgrade/rollback, docs (packaging docs/manifests kept separate from A's loader) | `native/manifest/*`, `native/*.md`, `readme.md`, `AGENTS.md`, `docs.md` | full release matrix |

Dependency edges are **only** the `Depends` lines in §2. Workstream membership
implies no ordering between its tasks.

### 1.2 Immediate priority — integrate the existing working components

<a id="batches"></a>

The user confirmed that Mikai, mpv-Janai and inference are already implemented
and working. The current pass connects them; it does not reimplement inference,
replace working engines or rebuild the native runtime as a prerequisite.

1. **Finish the integration seams.** Consolidate the landed player/settings
   changes: accepted Save applies the request to the owning player; explicit
   Prepare reaches the existing preparation path; backend and indexed-frame
   callbacks agree; source epochs discard stale work; processing remedies remain
   separate from media retry. Complete the remaining player language wiring.
2. **Build the application once.** Use the existing Qt build and installed
   working Native bundle. Resolve compiler errors at these interfaces, not by
   redesigning either component.
3. **Run one end-to-end integration scenario.** Open real local media, exercise
   the settings/preparation/activation path through the existing runtime and
   observe the actual player output and status. Check pause/source ownership
   and the changed failure actions. Run the project's tests after the integrated
   changes; no per-edit probe loop.
4. **Fix observed integration failures only.** Keep the working mpv/inference
   implementations and compatible engines. A component change needs a concrete
   integration failure showing why it is necessary.
5. **Record the result.** Distinguish landed source, a successful build and
   exercised behavior. Update the affected documentation without presenting an
   integration smoke as full release acceptance.

**Separate backlog, not integration prerequisites:** a new Standard bundle,
native SDK reproducibility, expanded provenance/rights work, independent
numerical investigation, path-hardening acceptance, exhaustive GPU/lifecycle
stress, performance benchmarking and clean-machine distribution. Their original
M/C/R/N entries remain below; none is silently marked complete or deleted.
Third-party redistribution gates still apply to distribution, not to this
private integration pass.

### 1.3 Shared interfaces (agreed once; every task builds against these names)

<a id="dag-interfaces"></a>

| Interface | Location (PROPOSED unless noted) | Owner task |
| --- | --- | --- |
| `mplayer::MpvApi` / `MpvApiInstance` / `MpvApiError` — the single process-wide `mpv_handle*` function table, module path, pinned `HMODULE`, recorded capabilities. Serves stock and native alike, so it is deliberately **not** named `MpvRuntime` and **not** nested under `native/` | `src/player/MpvApi.hpp/.cpp` | M07 |
| `mplayer::native::Bundle` — validated, coherent standard-or-native runtime bundle descriptor | `src/player/native/NativeBundle.hpp/.cpp` | M05/M06 |
| `mplayer::native::ModelRole` {Spatial, Temporal}, `Backend` {TensorRT, DirectML}, `Availability` {Ready, Preparing, Unavailable} + mandatory factual reason | `src/player/native/NativeTypes.hpp` | M10 |
| `mplayer::native::Recipe` — ordered spatial stages, optional RIFE stage, backend. Stages name models only; prepared state joins later on the **build key**, with **engineSha256** as the separate post-serialize integrity value | `src/player/native/NativeRecipe.hpp/.cpp` | M10 |
| `mplayer::native::PreparationPlan` — resolved source hw/fps/CSP/decoder-device/SM, per-stage I/O, source→output geometry, blockers | `src/player/native/NativePreparationPlan.hpp/.cpp` | M13 |
| `GraphContentIdentity` (role + content hash + actual declared I/O) and `EngineBuildIdentity` (device/precision/TF32/build options) + `engineSha256`; `ArtifactManifest` and the ownership boundary | `src/player/native/NativeManifest.hpp/.cpp` | M14 |
| `mplayer::native::StatusSnapshot` — {version, revision, typed state} from the filter-local `GET_META` route, covering the Probe / Prepared / Active states and the reported device identity | `src/player/native/NativeStatus.hpp/.cpp` | M19/M20 |
| `mplayer::native::Controller` — per-`PlayerDialog` requested/effective state, epoch protocol, process-wide native work lease | `src/player/native/NativeController.hpp/.cpp` | M21/M26 |
| `mplayer::shell::kNativeSettingsGroup` (`player/native`, versioned sub-key) | `src/player/native/NativePrefs.hpp/.cpp` | M34 |
| `mplayer::shell::isReservedNativeShortcut()` + `player/native/shortcuts` group | `src/player/native/NativeShortcuts.hpp` | M36 |

### 1.4 Dependency depth

<a id="dag-waves"></a>

The task graph is acyclic; the longest chain from a root is **25 levels**. The
per-task `Depends` lines in §2 are authoritative — the lists below are a compact
dependency aid, not a second DAG and not a merge order.

- **Independent roots:** M01 (baseline), M02 (fixtures), M03 (pins), M06 (bundle
  contracts), M10 (typed contracts).
- **Independent of native production:** pure contracts, Standard playback,
  owned-cache logic, CPU inspection/planning/ownership and the consumer/UI
  slices.
- **Gated by native production:** M04 (native-source build), M07 (loader cutover
  touches every call site), M15/M16 (real preparation + binding/numerical
  inspection).
- **Off the critical path but not deferrable:** M12 depends on M10 for its typed
  surface and can proceed independently of M04/M05.

Task IDs are labels, not an ordering: a task depends only on its own `Depends`
line and the shared interfaces in §1.3. There is **no numeric milestone merge
order**. The batches in §1.2 set the runnable order; no two tasks in the same
batch share a file.

### 1.5 Critical path

<a id="dag-critical-path"></a>

The longest implementation chain is **25 tasks**, after separating independent
contracts and Standard runtime work from Native bundle production:

```
M10 → M14 → M15 → M16 → M17 → M18 → M19 → M20 → M21 → M22 → M23
    → M26 → M33 → M34 → M36 → M38 → M41 → M42 → M43 → M44 → M45
    → M49 → M50 → M51 → M52
```

M15 also requires M04's real preparation helper, M11's inspected models and
M13's resolved geometry. The separate provenance/build/bundle branch is
`M03 → M04 → M05`; it gates Native artifacts, runtime proof and distribution,
not development of the Standard loader or pure recipe/manifest contracts. M05
and M08 join the release path at M50, alongside M49. M52 is the sole terminal
task and its prerequisite closure includes every other task.

Branch edges that leave and rejoin the line: `M09 → M19` (ABI/capability into
the status surface), `M12 → M17` (RIFE capability into combined recipes), `M24`
beside M25/M26, `M28 → M29 → M30` (PiP and teardown), `M31 → M32 → M43` (Compare
plan locking and Anime4K composition into the framebuffer proof),
`M35/M39/M40 → M41`, `M46/M47/M48 → M49`, and `M12/M25` feeding M24/M48.

### 1.6 Canonical status and current blockers

<a id="status"></a>

**Current local integration snapshot (2026-10-09; milestones remain open).**
Latest reports supersede older dated status for the observations below; they do
not complete any M-task, mark private Native `Ready`/`Active`, or clear
redistribution. Current release API versions remain mpv
`2026-10-07-d6d93599d5`, AnimeJaNai `3.7.0`, Aji `v0.9.0`. The exact API
archive and locally observed DLL hashes, upstream component query, archive
membership, model inventory correction, explicit custom Aji import, and
plan-only run are recorded in `native/manifest/sources.json` and
`native/README.md`.

- **GUI evidence (artifact 343):** English/Ukrainian access to all 47 RIFE and
  five spatial selections passed; live catalog Save/refusal passed. Verify
  success preserved missing selected DirectML and inactive corrupt TensorRT
  states; selection survived refresh; Review Yes was actually clicked; backend
  persistence passed.
- **Earlier default component download attempt:** **failed**, before creating a DirectML record,
  with `The verified runtime archive listing could not be read`. The root cause
  was the existing 7zr fallback's `.7z`-only support; Aji setup needs ZIP and
  NuGet. An official 7-Zip 26.03 x64 installer and verified 7zr bootstrap were
  acquired; bootstrap extracts the installer payload. The helper removes the
  bootstrap and installer after success and retains `7z.exe`, `7z.dll` and other
  extracted payload in the app tools cache, not `dist`; exact license-file
  contents were not inspected. Bootstrap SHA-256
  `ad4c82fadcbdf93c03b4fc440f300509c7d60c5c2f4d183e35d9d70d6957037d`;
  installer SHA-256
  `0859c524b8a63551848f0c246abddcb1d0b7b656b0fbfe879f8d85e61a9e6edd`.
  Artifact 351 initially rejected a valid Windows archive member path as unsafe.
  After separator normalization, artifact 357 completed actual DirectML
  download/install/verification after Review Yes; selected DirectML remained
  selected and inactive corrupt TensorRT was ignored. The ABI-8 install-record
  SHA-256 is
  `24443bc9d5f8624149d7b3b1f91dd851ad7fbc5323fa2836d3380bbe268ebd10`.
  Artifact 357's overall GUI probe failed four false shortcut/hidden-tab
  assertions. After the QTabWidget page-ancestry and preset-locator corrections,
  artifact 361 passed the complete current GUI/settings gate with zero failures
  in 13.68 seconds. Native and Presets shortcuts opened their actual pages;
  Ctrl+Alt+Shift+F12 glyph and clear-inside checks passed; Save/Cancel passed at
  760x780, DPR 2; English/Ukrainian access to all 47 RIFE and five spatial
  choices, live language Save/refusal, selected-missing DirectML versus inactive
  corrupt TensorRT Verify, and Review Yes followed by default DirectML
  download/install/verify passed. The install record remained ABI 8 with four
  DirectML DLLs and SHA-256
  `24443bc9d5f8624149d7b3b1f91dd851ad7fbc5323fa2836d3380bbe268ebd10`.
  `dist` stage 6 was rebuilt; application/Core remained identical to stage 5.
- **Runtime/UI:** packaged `mikai.exe` window launch and normal shutdown were
  observed. A genuine 320×180 H.264 24 fps, two-audio-track/subtitle fixture
  played through Wasapi/multitrack before the first Aji request; it is not
  processed-playback evidence and implies no FPS claim. An earlier settings
  shortcut 2DIP probe overran; artifact 361 later passed its reported shortcut
  visibility/clear-inside checks and Save/Cancel geometry at 760x780, DPR 2.
- **Plan-only:** all 47 verified graph choices resolved exact
  `prepareAnimeJanaiPlan` inputs, including v4.26 heavy. No engine build was
  requested. ABI-code notes and the fixed catalog lifetime defect are recorded
  in the acquisition record.
- **Fresh spatial attempt (artifact 340):** TensorRT 11.3.0 `[b99]` built the
  fresh engine in 18.527 seconds (3.01755 MiB) for graph SHA-256
  `b308e93c3dda3c4f9f968295b2b7b8d1c0583a711bca53a08d51134d9d1cbd5c`.
  Engine SHA-256:
  `a122bc35c163740a3eedb1221c72e289c296682594bc580ac5faa5676a6f74a8`;
  engine/logs are under
  `.probe-out/natural-spatial-a43/native/animejanai/models/spatial/`.
  The overall spatial probe was marked FAIL after a 600-second wait expected
  incidental status `filter accepted`; actual status was `filter retained
  (inference not observed)`. The status gate is now corrected, but this run does
  not become PASS retroactively. The shim/TRT backend and engine loaded, slot
  2001 configured (`320x180→640x360`, depth 3), CUDA-graph chain captured and
  `vo=libmpv` reported 640×360 CUDA NV12. This is build/configuration evidence,
  not pixel correctness, processed playback, or Ready/Active.
- **Previous spatial attempt (a44):** a fresh raw engine build and 640×
  output/render logs were observed, but the probe failed CRC filename filtering
  and an early raw-output read of 320 before configured-slot Core property
  reconfiguration. The corrected spatial run superseded these false gates.
- **Fresh spatial run (artifact 367, a46):** FULL PASS in 66.47 seconds on stage
  7. The selected-engine directory began empty; the raw engine built in 15.7249
  seconds (2.98863 MiB). The exact spatial HD Balanced
  graph SHA-256 was
  `b308e93c3dda3c4f9f968295b2b7b8d1c0583a711bca53a08d51134d9d1cbd5c`.
  Its fresh engine was 3,133,804 bytes, SHA-256
  `0a3fd65a1de1abe5507a996ee571abfebef8eda3294046a195b91b4648c759c9`;
  the PASSED raw build log tied the exact `--onnx` graph to the `--saveEngine`
  output and actual engine CRC, with all request-bound components present;
  full Qt/render/control/shutdown trace:
  `.probe-out/natural-spatial-a46/qt-flow-proof.log`.
  Core video-out params were 640x360 from 320x180 NVDEC input, configured
  spatial slot 2001 GPU 320→640, followed by four successful render flags.
  Postfilter count was 73 rendered frames/3 seconds (24 fps unchanged), raw
  estimate 24, decoder/VO drops zero. The nonblack, changed/moving OpenGL frame
  and owned PlayerDialog capture passed; Space pause, Right seek, volume 37/
  mute, audio/subtitle cycle, Off restoring hwdec, source reopen and shutdown
  surface checks passed. This passes the described selected spatial playback
  run, not all-47 inference, numerical fidelity, private Ready/Active, or an
  FPS improvement claim.
- **Initial RIFE attempt (artifact 349):** the v4.14 lite engine built and
  loaded; the probe failed because its request-bound scan searched spatial
  `plan.modelDir`/wrong model identity and `time-pos` transitions were not
  presentation-frame evidence. This failure was corrected in the fresh run.
- **Fresh RIFE run (artifact 366):** FULL PASS in 88.63 seconds on stage 7.
  The selected-engine directory began empty. The exact v4.14 lite graph was
  5,986,311 bytes (SHA-256
  `948019ffb5e7e54ac6b89a13542da8d100af9b4ade7374551c5f595ac3ff2ce7`),
  model code 4141. Qt worker → vf → local custom ABI-8 → `trtexec` produced a fresh
  request-bound engine in 38.932 seconds (9.12264 MiB; 9,565,780 bytes, SHA-256
  `e1a4421a3634df8fedaf9d45f1ecc5120aa371e41beb26612ffd7fdfa98dee22`); the
  PASSED footer matched the exact ONNX and `saveEngine`, all request-bound
  components were present, and no prebuilt engine was used; full trace:
  `.probe-out/natural-rife-a45/qt-flow-proof.log`. RIFE reported
  active `2/1`, `NewVideoFPS48`, padding to 320x192 then crop to 320x180, with
  actual NVDEC output 320x180. Successful `MPV_RENDER_UPDATE_FRAME` outputs
  over 3 seconds were 73 baseline versus 145 processed at equal resolution;
  the 1.5x threshold passed. `time-pos` events were 70 vs 72 and were not used.
  The nonblack/changed/moving processed image passed (SHA-256
  `91ede513739640d2dacb86e3dbdfa48cabb8c4ecb6d99e194344b9f6fac40111`);
  pause/seek, volume 37/mute, audio/subtitle cycling, Off restoring hwdec,
  source reopen, and shutdown-surface checks passed; decoder/VO drops were 0.
  Engine-to-active-model identity remains inferred because Core has no
  engine-ID event. This is not numerical-fidelity/all-47 proof, private
  Ready/Active, or FPS improvement evidence.
- **Still pending:** numerical model fidelity, all-47-model inference, private
  Ready/Active, and ESC-1/2/3 clearance. The described RIFE and spatial selected-
  model runs passed their fresh build/render/control checks; neither establishes
  other models, private Native capability, or an FPS improvement. The latest
  union build had zero errors/warnings, CTest passed in 13.78 seconds, Ukrainian
  translations were 999 finished/0 unfinished, and `dist` was rebuilt at stage
  7. Artifact 361 passed the current GUI/settings gate. Do not upgrade broad
  milestones based on the two selected-model runs.

**Latest complete CPU matrices (2026-10-04).** The newest full `ctest` is green:
inspection-ON exit `0` in **21.48 s** and inspection-OFF exit `0` in **15.92 s**
(`.probe-out/native-batch-on-epoch-guard-ctest.log`,
`.probe-out/native-batch-off-epoch-guard-ctest.log`), with ON/OFF app/tests/
framebuffer builds exit `0`. An earlier settled-source run (ON `20.55 s` / OFF
`15.37 s`) and the geometry PASS en/uk × DPR1/DPR2 at minimum `560x600` predate
the later MinGW-CRT/source updates. See §1.7 for the full current-wave evidence.
Isolated pre-cutover ledgers (24/25 suites, 175/0/1, 149/149, 23/23 HLS, 21/21
external) are **historical** and superseded wherever a later run exists. Each
task's own `Status` line is the authority for that task's scope; this summary
never upgrades a partial slice.

**Verified independent slices:** loader/bundle contracts (M06/M07), typed
contracts (M10), CPU inspection/planning/ownership (M11/M13/M14), status consumer
and UI contracts (M20/M33–M40), SDK acquisition (C03/C10), and original RIFE
asset acquisition and weight lineage (C11). Each is proven only to the scope its
own `Status` line states.

**Root blockers (honest, current) — 1–2 resolved this wave, 3 open:**

1. **Initial native snapshot — RESOLVED.** The genuine prepared CAPI first-
   snapshot regression now shows **all 7 previously failing fields PASS before
   output**, state `Prepared` (not `Active`), with a real CUDA NN measurement
   (maxAbs `0.0034044981`, RMSE `0.000446848262`) —
   `.probe-out/native-batch-prepared-snapshot-proof.log`. This is a **DIRECT
   CAPI/CUDA** proof, **not** a Qt-framebuffer proof. (The earlier exit-`7` run
   `.probe-out/native-final-prepared-capi.log` is historical.)
2. **Curated RIFE pin→content mapping — RESOLVED (models).** Curated RIFE
   requests now resolve the four exact pinned original graphs to owned
   content-addressed records (role, fp16 `N1C11→C3`, store integrity, genuine
   CPU-reference boundary); ordered all-stage leases publish no partial plan,
   with **no aliases, no substitution and no graph rewriting**. This is model
   *compatibility*, not prepared NN.
3. **RIFE numerics (M12/M16).** The four original temporal graphs import, are
   CPU-referenced and serialize; lite and lite-ensemble verify, but **full**
   (`maxAbs 0.0385742188`, RMSE `0.00104668487`) and **ensemble**
   (`maxAbs 0.0244140625`, RMSE `0.000785205742`) exceed the **untouched** `0.02`
   max / `0.005` RMSE bounds. O5 was not run; no threshold is widened and no
   `Ready` is claimed.

**DirectML (M25/M48)** is an **honest refusal**: the legacy `SHARED` path
imports/blits exact RGBA corner pixels, but the required `SHARED|SHARED_NTHANDLE`
registration fails `wglDXRegisterObjectNV 0xC007006E` →
`CAPABILITY_UNAVAILABLE`, exit `2`, `directMlMayBeEnabled:false`, **no CPU
fallback**. No DirectML RIFE is claimed.

**Native Off is the untouched default.** No filter installed, no engine built,
no device guessed.

---

### 1.7 Current-wave observed evidence (2026-10-04)

Each milestone `Status` below references this section instead of restating it.

- **CPU matrices (latest complete):** inspection-ON `ctest` exit `0` in
  **21.48 s**, inspection-OFF exit `0` in **15.92 s**
  (`.probe-out/native-batch-on-epoch-guard-ctest.log`,
  `.probe-out/native-batch-off-epoch-guard-ctest.log`); ON/OFF app/tests/
  framebuffer builds exit `0`; strict SDK Release build of `0001`+`0002`+`0003`
  exit `0` in 29.492 s (`.probe-out/native-batch-sdk-build.log`).
- **Genuine prepared CAPI snapshot regression: now 0.** The first-snapshot
  regression that previously failed 7 fields now passes **all 7 before output**;
  state `Prepared`, not `Active`; real CUDA NN measurement maxAbs
  `0.0034044981`, RMSE `0.000446848262` — a **DIRECT CAPI/CUDA** proof, **not** a
  Qt-framebuffer proof (`.probe-out/native-batch-prepared-snapshot-proof.log`).
- **Models:** curated RIFE requests resolve the four exact pinned original graphs
  to owned content-addressed records (role, fp16 `N1C11→C3`, store integrity,
  genuine CPU-reference boundary); ordered all-stage leases publish no partial
  plan; no alias, substitution or graph rewriting.
- **Producer:** `native/patches/0003` + staged `aji_dispatch` `create_prepared`
  now publishes a real backend snapshot; ABI8 / snapshot2 / schema2 / query1
  unchanged.
- **Controller/UI:** real `NativePlanLock`, last-wins request queue, and the
  existing status signal publishes a queued reason without changing
  effective/`Active`; `Off` needs a successful async remove plus observed
  absence; timeout/cancel is never `Removed`; the stop barrier is true only with
  no native work or observed `Suspended`/`Removed`; the Compare caption uses the
  live queue.
- **Stock UI probes (stock core, not native):** UK RIFE settings
  Save/Cancel/store/reload/`3:2`/order/threshold/min `560x600` exit `0`
  (`.probe-out/native-batch-stock-rife-settings-uk.log`); external media `21/21`
  and HLS `23/23` exit `0` (`.probe-out/native-batch-stock-external-media.log`,
  `.probe-out/native-batch-stock-hls-recovery.log`); Compare real moving-source
  smoke exit `0` with noise `0.000`, shader delta `0.770`, seek delta `7.339`
  (`/255`) (`.probe-out/native-batch-compare-motion-caption.log`).
- **Bundle:** `0.1.0-dev.2` is installed as `native-0.1.0-dev.2` under
  `AppLocalData/native/runtime`. `current-native` points at it;
  `native-0.1.0-dev.1` remains for rollback. Core `8d03aff4…` (149970714
  bytes). Installed `mikai_bundle verify` exit `0`: 83 files, 79
  restricted-search dependency loads, client API `131077`, the core accepts
  `@mikai_native`, aji ABI 8
  (`.probe-out/native-crt-closure-verify-installed.log`). A later current-build
  `mpv_surface_smoke` explicitly selects this installed bundle and passes:
  real `1280x720` Qt framebuffer, `921600/921600` non-black samples,
  `297305/921600` motion, `24` fps, zero decoder/VO drops, and zero live
  filters (`.probe-out/native-bundle-dev2-off-surface.log`; inspected PNG
  beside it). This is Native-core playback with processing **off**, not
  startup discovery, prepared readiness or inference. The stored runtime
  choice was not written; an empty choice remains Standard.
- **Runtime:** a foreign MinGW3 copied into the Qt executable dirs is
  incompatible; CMake now accepts a selected runtime only when byte-identical to
  the configured Qt cohort (named `FATAL_ERROR` on mismatch). The old private
  `.2` closure under the correct Qt cohort **fails to load**
  (`mpv-deps/avcodec-63.dll`, missing procedure `Win32 127`)
  (`.probe-out/native-batch-qt-old-closure-verify.log`).
- **Not accepted:** mandatory CUDA Qt-framebuffer N1, real RIFE cadence/cuts/EOF/
  A-V, native-active Compare/PiP/source/100-cycle lifecycle and the full
  performance matrix; full/ensemble RIFE numerics still exceed the unchanged
  `0.02`; DirectML is the required `SHARED_NTHANDLE` refusal; public
  distribution/licence/clean-install/release remain blocked by upstream grants.
  **No milestone is completed from CPU/CAPI/stock proofs.**

## 2. Task list

Each task retains its purpose, observable acceptance and significant numerical
requirements. Full historical evidence lives in `.probe-out/`; the canonical
status summary is §1.6.

<a id="ws-a"></a>

### Workstream A — Runtime packaging and ABI (M01–M10)

<a id="m01"></a>

### M01 — Fresh independent baseline; retire obsolete incidental assertions

- **Purpose:** record pre-change truth on a freshly configured tree; **delete**
  (never re-pin) assertions that test wording, object names or incidental
  defaults in `TestAnime4K`, `InteractionProbe`, `Anime4KMenuProbe`; fix the
  harness shader-deploy gap (today `shaders/` is copied only as a `POST_BUILD`
  step on `mikai`, so a fresh build has no shaders beside `mikai_tests.exe`).
- **Acceptance:** a clean configure yields `ctest --test-dir build-cmake -R
  mikai_tests` exit 0 with no assertion that would fail on a reworded label, a
  renamed object or a changed incidental default; every surviving assertion
  names an observable behaviour; a `TestLiveOracle` **SKIP is never reported as
  a pass**; if deleting an assertion exposes a real regression, re-express it as
  a behaviour test rather than restoring the old one.
- **Depends:** none. **Status:** verified; see §1.6.

<a id="m02"></a>

### M02 — Hermetic real-media and local HTTP/HLS fixtures

- **Purpose:** real decodable media (320×180 and 1920×1080; 8-bit and 10-bit
  HEVC; 23.976 and 24 fps; one clip with a hard cut, a saturated segment and a
  near-black segment; one non-multiple-of-32 height; one portrait) plus a
  `QTcpServer` fixture serving a plain file, an adaptive HLS master plus
  variants, a `/retry.mp4` that 404s once then serves the same bytes, and one
  variant whose signed query string has already expired. No CDN, no key.
- **Acceptance:** the fixture serves every case with no outbound network;
  `external_media_probe` passes against a fixture URL; the expired signed
  variant produces a **visible failure followed by a visible recovery**, never a
  silent success. Recorded: `external_media_probe` 21/21, `hls_recovery_probe`
  23/23, zero FAIL.
- **Depends:** none. **Status:** verified.

<a id="m03"></a>

### M03 — Pin native sources, provenance and redistribution decisions

- **Purpose:** record the pin manifests: mpv fork commit
  `63d127174d8f70336c82a22408929e2bc301edcb` with the touched upstream files;
  the inference source identity; C ABI 8 and its symbols; `AJI_NVINFER` default
  `nvinfer_10` with the build here set to `nvinfer_11` for TensorRT 11.3 (the
  CUDA minor is **never** inferred from the TensorRT version); the CUDA arch
  list `75-real 80-real 86-real 89-real 90-real 100-real 120-real 120-virtual`;
  observed package hashes labelled descriptive, not release pins; and a
  per-component licence + `cleared`/`blocked`/`external` decision.
- **Acceptance:** `native/manifest/components.json` lists every binary that will
  exist in a shipped bundle with a source identity, a licence and an explicit
  decision. **Any component marked `blocked` makes M50 and M52 unachievable and
  must be escalated, not worked around.**
- **Depends:** none. **Status:** pins recorded; third-party clearance absent —
  the owner grant of 2026-10-04 covers only this project's own integration, the
  shim's upstream terms are unverified and the permission request is unsent.

<a id="m04"></a>

### M04 — Reproducible optional native SDK builds

- **Purpose:** a second, **optional** CMake project (MSVC + nvcc; C, C++17,
  CUDA) beside the MinGW/Qt default. Vendor pinned sources as a patch set in
  `native/patches/`. First patches: filter-local `GET_META`; a `preparedOnly`
  refusal; a strong content-addressed engine cache replacing `aji_trt.cpp`'s
  weak key and stopping `clean_stale_engines` from deleting foreign files from
  `model_dir`; native plan suspend/resume. Add the explicit preparation helper
  CLI as its own target — an out-of-band `QProcess` child, never in the
  decoder/render/frame path. **No neural architecture is rewritten.**
- **Acceptance:** two consecutive clean builds produce byte-identical
  `aji.dll`/`aji_trt.dll` (or a recorded, explained difference); with
  `MIKAI_BUILD_NATIVE=OFF` the default MinGW/Qt build is bit-for-bit unaffected.
- **Depends:** M03. **Status:** fixed-directory clean-build reproducibility is
  recorded for the earlier `0001` stage in `native/build-msvc.md`, including
  the explained cross-directory nvcc variance. The current strict SDK union
  `0001`+`0002`+`0003` builds exit 0; that build log alone does not establish
  repeat-build identity of the current union or bit-for-bit OFF invariance.

<a id="m05"></a>

### M05 — Assemble coherent versioned native runtime bundles

- **Purpose:** a bundle is **one coherent unit** — exactly one mpv `DLLMODULE`
  (standard **or** native, never mixed), its self-contained dependency closure,
  the native filter and inference DLLs, and `manifest.json` recording kind,
  module path, mpv API version, native protocol version, engine-schema version,
  per-file hashes and bundle version. Production must not depend on Conda,
  torch, Python or any ambient install. Produce a **standard** and a **native**
  bundle.
- **Acceptance:** `bundle_tool verify` passes only when every declared file is
  present, hash-matching and loadable by restricted-search `LoadLibraryEx`; a
  closure reaching outside its own directory is rejected; a bundle labelled
  `native` that cannot register `mikai_native` is rejected as incoherent.
- **Depends:** M03, M04. **Status:** Native bundle verification passes in place
  as `native-0.1.0-dev.2` (83 files, client API `131077`, core accepts
  `@mikai_native`; `.1` and `0.1.0-dev` kept). Producing and verifying the
  required Standard bundle remains unestablished; bundle-tool code and the
  layout example are not that proof. Native-core processing-off playback also
  passes (§1.7), but prepared inference is separate and unproved. Public
  shipment remains blocked on third-party clearance. Evidence:
  `.probe-out/native-crt-closure-verify-installed.log` and
  `.probe-out/native-bundle-dev2-off-surface.log`.

<a id="m06"></a>

### M06 — Validated runtime discovery and atomic installation

- **Purpose:** bundle-aware discovery alongside the untouched stock path. One
  app-owned layout used by every later task:
  `AppLocalDataLocation/native/{runtime,models,artifacts,scratch,logs}`.
  Validate each candidate against its manifest; install **atomically** — extract
  into a temp directory on the same volume, verify, then flip a `current`
  pointer. Never write into a user, shared or installed model directory.
- **Acceptance:** with no bundle present, discovery reports "none" with a
  factual reason and the app still runs; a corrupt bundle (truncated DLL, hash
  mismatch, missing dependency) is rejected **by name** and the previous good
  bundle is still selected; an interrupted install leaves no half-installed
  bundle visible; two same-kind bundles, one invalid → the valid one wins
  silently.
- **Depends:** none. **Status:** synthetic discovery and atomic install
  verified; installed `native-0.1.0-dev.2` `bundle_tool verify` exit `0`
  (§1.7). Startup selection of that bundle is still unproved.

<a id="m07"></a>

### M07 — Cut over every client/render caller to the explicit MpvApi loader

- **Purpose:** one process-wide `MpvApi` holds the selected `DLLMODULE`, loaded
  with absolute `LoadLibraryExW` under `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` plus
  an explicit validated dependency directory — **no global `PATH` mutation**.
  It resolves every needed entry point once and hands out a function table; the
  module is pinned for the process life, past all handles. Many core handles are
  expected and legal (PiP runs one per `VideoSurface`); no hot module swap ever.
  Convert `MpvHandle`, `MpvRenderContext`, `VideoSurface::ensureEngine()` and
  `apiVersion()`. Remove every load-time import: delete the `MPV_LIBRARY` link
  and `find_library` block, replace `MPV_RUNTIME_DIR` with a **headers-only**
  `MPV_INCLUDE_DIR`, and update every documented configure command. In the same
  cutover add `mpv_command_async`/`mpv_get_property_async` and **fix** the
  latent `observe()` mismatch (it asks `MPV_FORMAT_NODE_ARRAY` while its
  dispatcher handles only DOUBLE/FLAG/STRING; nothing reads it today).
- **Acceptance:** `dumpbin /imports` shows **no** `libmpv-2.dll` import in
  `mikai.exe`, `mikai_tests.exe` or any probe; the app opens media and renders a
  non-black frame using only the explicitly loaded module; two cores (main +
  PiP float) coexist from **one** module; a bundle-selection change persists and
  requires a restart, and says so. Async: a deliberately slow native configure
  does not stall the Qt event loop; the HUD keeps updating from the coalesced
  bank; an async reply for a superseded epoch is discarded; no event-owned node
  is freed by the client.
- **Depends:** M01, M06. **Status:** Standard module path verified — `objdump`
  shows zero libmpv imports, `mpv_loader_probe` passes (client API `131077`
  against header `131077`, live swap refused by name, async replies correlate
  one-to-one, refused load delivers its end-file error event). The installed
  Native `.2` module also renders moving video through the real Qt framebuffer
  with processing off (§1.7). Native two-core coexistence and slow-configure
  responsiveness are not established by this single-surface run.

<a id="m08"></a>

### M08 — Wire startup runtime selection, recovery and restart preferences

- **Purpose:** choose the bundle from preferences **after** `QApplication` and
  the application/organization names (so `QStandardPaths` resolves) and
  **before** any `VideoSurface::ensureEngine()`. Record kind, version, path, API
  and capabilities. Make `runtimeRequested` actually provision: the `TitlePage`
  handler runs provisioning then calls `startPlayer()` again instead of merely
  re-showing the dialog. A selection change persists and requires a restart, and
  the UI says so. Preserve Standard provisioning; do not replace the stock
  runtime tree to deliver a Native bundle.
- **Acceptance:** starting with the native bundle selected loads it; starting
  with the standard selected loads that; first play with no bundle produces one
  clear log line plus a provisioning attempt, then either recovers or reports a
  named failure — never a silent black player; a failed provisioning attempt
  leaves the app usable and retryable.
- **Depends:** M07. **Status:** Standard selection and named Native fallback
  verified; a real Native bundle load is still unproved.

<a id="m09"></a>

### M09 — Negotiate native ABI, protocol and runtime capabilities

- **Purpose:** define the capability and probe protocol here, so M19/M22
  implement it and M13 consumes it: `mpv_client_api_version()`; whether
  `vf-metadata/mikai_native` resolves under `MPV_FORMAT_NODE`; the native
  protocol and engine-schema versions; which backends are present. The protocol
  must cover an explicit **PROBE-ONLY PASSTHROUGH** state: the filter attaches as
  a pure pass-through, performs **no** engine configure/deserialization/build/NN/
  RIFE, and reports from the filter thread the real `AVHWFramesContext` device
  UUID or D3D adapter LUID plus decode geometry and CSP; its status is `Probe`,
  **never** `Active`. `NativeOff` by default installs no filter at all. Add one
  **additive** structured snapshot export on `aji.dll` (not libmpv, not
  `MpvApi`) with the protocol version negotiated explicitly. **No legacy log-tail
  parsing anywhere.**
- **Acceptance:** a bundle whose core predates the filter reports
  `NativeUnavailable(reason: "core has no mikai_native filter")` and the app
  plays normally with native off; a newer or older protocol reports both
  versions and does not half-work; the declared capability set explicitly
  includes `probe_passthrough`, `CUdeviceUUID` and `D3DAdapterLUID`, and an
  absent capability is reported absent rather than approximated.
- **Depends:** M07. **Status:** partial — the additive ABI-8 snapshot v2 export
  is observed (protocol/schema `2/2`; real-context decoder UUID
  `GPU-f76e436f-c519-8f5a-95f7-ce9481b54b5b`; resize-only `64x64 -> 96x96`;
  ticket 1 completed; `drain=0 suspend=0 resume=0`) yet the native state stays
  `Unavailable`, `effective` empty. The 684-byte query POD is frozen and the
  factory query is **REAL** on the 5090 (`backend=1` TensorRT, spatial+rife
  roles, SM 12.0, runtime `11.3.0`, `cuDriverVersion 13040`). This is a
  **DIRECT-COMPUTE factory-only** proof — not decoder/NN/N1/`Prepared`/`Active`.

<a id="m10"></a>

### M10 — Define native model, recipe, preference and availability contracts

- **Purpose:** the typed vocabulary. `ModelRole {Spatial, Temporal}`;
  `Backend {TensorRT, DirectML}`; `Availability {Ready, Preparing, Unavailable}`
  with a mandatory factual reason when not Ready. `ModelRecord {id, label,
  bytesSha256, terms, dtype, role, backends, actualProfile}` — stable,
  content-addressed, in a catalogue **separate from Anime4K**. `SpatialStage
  {modelId, preResize}`; `TemporalStage {version, full|lite|ensemble,
  scdetThreshold, order(BeforeSpatial|AfterSpatial), factorNum, factorDen}`;
  `Recipe` in three shapes (spatial-only, RIFE-only, combined). **Stages carry
  no prepared-artifact ids** — the prepared side is a join on the M14 build key,
  with `engineSha256` as the separate post-serialize value. Record requested,
  effective and reason as three separate fields from the outset.
- **Acceptance:** a recipe round-trips through its serialised form unchanged; a
  recipe naming an unavailable model or backend yields `Unavailable` **with the
  reason** and never a silent fallback; ordering is preserved exactly; a
  `combined` recipe cannot be expressed with an ambiguous order.
- **Depends:** none. **Status:** verified independent contracts; real RIFE
  settings store/reload preserves exact requests. No Native execution claimed.

<a id="ws-b"></a>

### Workstream B — Models, ordered preparation and cache (M11–M18)

<a id="m11"></a>

### M11 — Compatible ONNX import and independent model inventory

- **Purpose:** import a user `.onnx` and validate it by **actual engine
  inspection** — never by filename prefix and never through an architecture
  whitelist. Check the exact input count and output count (refuse anything but
  1×3 in / 1×3 out, naming the actual counts), the declared dtypes and the
  **real graph geometry** (1:1 or a supported scale learned from the actual
  engine, not a `2x_`/`4x_` filename). **Eligibility: FP16 NCHW only** — the
  bridge expects FP16 I/O, so FP32 and BF16 graphs are recorded in the inventory
  as **unsupported with an explicit reason**, never converted, re-exported or
  auto-promoted. A fixed graph must be rank 4, N=1, C=3, NCHW on both sides; a
  dynamic graph declares the same rank/N/C with dynamic H/W. A declared symbolic
  output batch stays symbolic in the declared metadata and is eligible only
  after a **real CPU reference run at the requested geometry** proves actual
  N=1/C=3. The spatial relation must be a **uniform positive integer scale**
  (identity/1× included). Dynamic H/W resolve from the **actual stage geometry**;
  no `dyn-HW` filename form and no filename-based classification. No Python.
- **Acceptance:** importing
  `2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo.onnx`
  succeeds with a **learned** factor; a two-input model is rejected with the
  actual counts; two same-named files in different folders yield two distinct
  IDs; a truncated file is rejected **without** writing anything into the
  inventory; an FP32 and a BF16 graph are recorded unsupported with an explicit
  reason, no converter run; a rank-5 or C≠3 graph, or a non-integer/non-uniform
  scale, is refused naming the actual declared shape; a dynamic-HW graph resolves
  geometry per stage without a different identity.
- **Depends:** M10. **Status:** inspection boundaries verified (fp32/bf16
  unsupported, truncated/corrupt, zero/multi-IO, rank, batch, channel); original
  HD owned import exits `0` (`modelId user:532fe8…`, declared output `N=-1`
  preserved, CPU run `N=1` at `320x180 -> 640x360` scale 2, `eligible (verified
  reference)`, `published:true`) — inspection only, not an engine and not
  prepared.

<a id="m12"></a>

### M12 — Provision original Janai RIFE variants and capabilities

- **Purpose:** the full RIFE capability surface — v4.14 full, lite, ensemble;
  rational `factorNum`/`factorDen`; the SCDetect threshold; the ordering choice
  (interpolate-then-upscale vs upscale-then-interpolate) with the per-model
  pre-resize RIFE-first ordering implies. The user's recipe is honoured exactly:
  no silent substitution of another chain or model, no silent skip of RIFE. Turn
  the informational "Planned: RIFE is not implemented yet" leaf into a real
  capability subtree; RealESRGAN and "other models" stay placeholders.
- **Acceptance:** each variant, factor, threshold and ordering is selectable and
  round-trips; a variant whose assets are absent reports `Unavailable` with the
  missing-asset reason and the recipe fails loudly — readiness is *the entire
  selected recipe, or a visible Original plus a failure*.
- **Depends:** M10. **Status:** assets acquired and weight-verified (128/128
  stored weight tensors match the author's MIT checkpoints after FP16 rounding);
  all four graphs run a real CPU reference and serialize on GPU. `mikai_native_verify`
  passes lite and lite-ensemble and **rejects full** (`maxAbs 0.0385742188`,
  RMSE `0.00104668487`) and **ensemble** (`maxAbs 0.0244140625`, RMSE
  `0.000785205742`) against the **unchanged** `0.02`/`0.005` bounds. M12 is
  **partial, not done**: the curated RIFE requests now resolve the four exact
  pinned original graphs to owned content-addressed records (§1.7), while the
  full/ensemble numerics still exceed the unchanged bounds. Artifact 349 later
  built and loaded a v4.14 lite raw TensorRT engine through Qt/libmpv, but failed
  overall and did not establish pixel correctness, output cadence, or Ready.
  No model is `Ready`
  or `Active`. The separate Native RIFE controls exist, but the old
  `rife/planned` leaf remains in `Anime4K.cpp`; replacing that leaf as requested
  in this task's Purpose is still unmet.

<a id="m13"></a>

### M13 — Resolve source-aware preparation plans and geometry

- **Purpose:** given an actual source (w, h, fps, CSP, matrix/range/siting,
  decoder device) produce a `PreparationPlan`: which stages apply, each stage's
  resolved input/output geometry, the source→output geometry and any blockers.
  The source geometry is learned from the real source and the real engine; until
  known, `sourceGeometry`/`outputGeometry` are an optional unknown, **not a
  fabricated 1×1 placeholder**. Handle orientation, crop and CSP exactly as the
  native reference does. Do **not** adopt the native "missing model → skip"
  behaviour for a user-selected recipe: a requested recipe is all-or-nothing.
- **Acceptance:** a 1080p23.976 10-bit BT.1886 source yields a plan derived from
  the real inspected graph, not a filename; a source outside every chain window
  yields a blocked plan with a named reason, not a fallback; two sources with
  identical geometry share one prepared artifact set; a portrait or cropped
  source produces a plan whose geometry matches the reference behaviour.
- **Depends:** M10. **Status:** resolver verified (51 prep-plan slots incl
  rotation-to-portrait, portrait/crop temporal padding, non-zero-origin crop,
  full-range, 10-bit/23.976/portrait, odd-dimension asymmetric pad, two-stage
  threading, 320x180 SD with real window refusal). Actual inspected-profile
  adapter execution and live source/device reports need M19/M22 and are never
  guessed.

<a id="m14"></a>

### M14 — Content-addressed engine manifests and ownership boundaries

- **Purpose:** **two distinct identities, never conflated.**
  `GraphContentIdentity` = role + graph content hash + the graph's *actual*
  declared I/O (device-independent). `EngineBuildIdentity` = GPU UUID + SM,
  precision/TF32 flags, plugin, runtime, core version, source ABI, CUDA/driver
  version and build options, plus `engineBytesSha256` over the produced engine
  file. Store manifests beside the artifacts in an **app-owned** cache. Never
  use a user, shared or installed model directory as a writable cache. Never
  deserialize an engine before its manifest validates. Detect an in-place source
  change and invalidate. **Two keys:** the pre-build key (what to compile and
  where to look) and the post-serialize `engineBytesSha256`.
- **Acceptance:** the same graph imported twice yields **one**
  `GraphContentIdentity` and two `EngineBuildIdentity` values when the machines
  differ; two same-basename models in different folders get different
  identities; a changed driver invalidates; a truncated engine invalidates; a
  sentinel dropped into a model directory survives a full prepare/activate/
  cleanup cycle byte-for-byte.
- **Depends:** M10. **Status:** identities and ownership verified (declared-
  profile/build-key agreement, collision refusal, explicit-root confinement with
  a model-directory sentinel, read-side redaction, manifest reparse checks,
  hostile manifest/store/runtime/LUID slot matrix). Complete prepare/activate/
  cleanup acceptance needs real producers.

<a id="m15"></a>

### M15 — Explicit cancellable native engine preparation

- **Purpose:** preparation is an **explicit user action**, runnable while the
  original plays. The controller keeps the picture on `Original` and the state
  at `Preparing` — never `Active`. `Ready` requires **all** assets, all engines,
  the resolved per-stage geometry and all capabilities. Cancellation is real:
  terminate the child and discard the partial artifact. One process-wide lease
  serialises GPU build against GPU inference. **No `trtexec` invocation,
  background build or log tail ever appears in a frame, configure, render or
  paint path.**
- **Acceptance:** Prepare starts visible progress; cancel stops it and leaves no
  usable-looking partial artifact; a failed build lands in `Failed(reason)`,
  keeps Original visible and marks nothing Ready; a second Prepare press while
  one is running does not start a second build; preparation while the video
  plays does not stutter or drop frames (quantified in M49).
- **Depends:** M04, M11, M13, M14. **Status:** standalone raw-engine producer
  verified — the Release in-process helper builds through the TensorRT builder
  API (no `--trtexec`, no child process) on the named CUDA UUID and passes **all
  seven** behaviours (existing-output preservation, corrupt-ONNX failure,
  absent-device refusal, `build-started` before the build, TF32-off/strongly-
  typed precision gate, mid-build cancellation publishing nothing, unchanged
  prior artifacts/source bytes). `native/patches/0003` + staged `aji_dispatch`
  `create_prepared` now publishes a real backend snapshot, and the genuine
  prepared CAPI first-snapshot regression is now **0** (§1.7); bundle
  `0.1.0-dev.2` is installed, load-verified and plays through Qt with processing
  off; it is not a preparation or inference proof. Full Qt orchestration, lease
  and publication remain open.

<a id="m16"></a>

### M16 — Inspect built engines; prove bindings and numerical behaviour

- **Purpose:** after a build, inspect the produced engine and **prove** rather
  than assume: every input and output binding's count, dtype and shape matches
  the plan (**validate all bindings, not just the last one**); the geometry
  factor is the one the plan expects; and a CPU-reference numerical comparison
  on a real frame. Record the data path honestly: CUDA↔Qt OpenGL interop performs
  device copies and a sync, and the RIFE scene metric is a **D2H scalar copy
  plus a CPU stream sync per pair**. A full-frame CPU conversion/readback is
  prohibited in the steady-state GPU path; a user-requested frame capture is
  allowed and separate.
- **Acceptance:** a correct engine passes with matching bindings, dtypes and
  geometry; a mismatched or stale engine fails loudly, naming the actual
  bindings; a corrupted engine never deserializes.
- **Depends:** M15. **Status:** the spatial HD Performance engine **verifies**
  on the corrected planar reference (same unchanged engine/graph: `maxAbsoluteError`
  `0.00610351562`, RMSE `0.000591099329`, `0` outside tolerance; the earlier
  `0.02734375` rejection was measured against a malformed interleaved tensor and
  is historical for this boundary). RIFE lite/lite-ensemble verify; **full** and
  **ensemble** are still rejected on `maxAbsoluteError`; the optimisation
  diagnostic ruled provider optimisation out (RMSE `8.5556432276e-06`). **No
  threshold is widened.** Every passing engine is `manifestVerified:false` /
  `prepared:false` — no engine is `Ready`.

<a id="m17"></a>

### M17 — Prepare complete ordered spatial and temporal recipes

- **Purpose:** take a `Recipe` — spatial-only, RIFE-only or combined — and drive
  it to a fully prepared, manifest-validated, readiness-published state. Apply
  stages in order: per-stage pre-resize in chain order; RIFE before or after
  spatial exactly as the recipe says; RIFE's own working resolution when a
  pre-resize applies. The curated Quality/Balanced/Performance entries are
  honest reference recipes assembled from the shipped models — **not** the native
  slot numbers `1001/1002/1003`.
- **Acceptance:** a combined RIFE-first recipe with per-model pre-resize
  prepares a chain whose every stage geometry matches the plan, published
  all-or-nothing; a recipe whose second stage cannot be prepared ends in
  `Failed` naming that stage, keeps Original visible and activates **no** partial
  chain.
- **Depends:** M12, M15, M16. **Status:** blocked — requires real fully verified
  spatial/temporal engines (numerical acceptance + prepared-plan identity
  unmet); the RIFE assets are acquired and weight-verified, not missing.

<a id="m18"></a>

### M18 — Replace weak-cache and playback-triggered build paths

- **Purpose:** native side — replace the weak cache key with M14's identity;
  make `clean_stale_engines` never touch anything it does not own; refuse to
  build during playback (`preparedOnly`); retire the configure-time build and
  `trtexec` from every playback path. Client side — the native filter is
  inserted only with a manifest-validated artifact set, or not at all; an absent
  filter is an honest state meaning "no native processing", never a silent
  passthrough presented as active.
- **Acceptance:** starting playback with a missing engine produces a named
  `PreparingRequired` state — not a minutes-long freeze and not a silent
  passthrough; playing with a prepared engine writes nothing to the cache; a
  process monitor watching a full session sees **no** `trtexec` child; the
  user's model directory is byte-identical before and after.
- **Depends:** M09, M17. **Status:** substantial implementation present:
  strict staging applies the prepared-only/cache patch; the managed prepared
  API validates digests before deserialize and bypasses legacy build/cache
  paths; assembly proves the whole artifact set before admission. This is not
  proof of an installed-core playback session: missing-engine refusal,
  cache-write silence, absence of a build child, and model-directory
  byte-preservation still need session evidence. Full M14 identity and M17
  prepared-recipe integration remain open.

<a id="ws-c"></a>

### Workstream C — Inference, metadata and filters (M19–M25)

<a id="m19"></a>

### M19 — Expose authoritative filter-local native metadata snapshots

- **Purpose:** serve a structured snapshot under label **`mikai_native`**, read
  via `vf-metadata/mikai_native` with `MPV_FORMAT_NODE`. Contents: protocol
  version, schema version, monotonic revision, and a typed state document
  carrying **requested plan, effective plan**, engine identities in use,
  per-stage geometry, decoder device, cadence counters and the last error.
  Probe-only passthrough is part of this contract (real device UUID/LUID plus
  geometry and CSP, status `Probe`, **never** `Active`; any context/device/
  cadence change invalidates the plan and re-probes). Two layers: the flat
  `mp_tags` layer carries `protocol`, `schema`, `revision` and the typed status;
  the snapshot itself is **one valid JSON string** in a single tag. **Node
  ownership is exact:** an async `mpv_event_property` node is borrowed — decode
  and deep-copy the typed values synchronously inside the dispatch, and never
  call `mpv_free_node_contents` on it. The free-node API is reserved for
  libmpv-allocated owned results, always through the selected `MpvApi` under
  RAII.
- **Acceptance:** reading `vf-metadata/mikai_native` returns a well-formed
  document whose revision strictly increases on each real change and is
  unchanged on a re-read with nothing changed; reading it on a core without the
  filter returns the property error — not a crash, not an empty success; the
  snapshot distinguishes requested from effective even for a 1:1 spatial chain.
- **Depends:** M09, M18. **Status:** partial producer proof. The fork source +
  DLL build is **actual PASS**, and the real non-copy NVDEC/CUDA **Probe** smoke
  passes **14/14** on actual decoded CUDA frames (320×180 NV12, 8-bit, 24 fps,
  decoder UUID `f76e436f-c519-8f5a-95f7-ce9481b54b5b`, SM 12.0): typed
  source/device facts, unchanged frozen revision, seek/source-reload epoch
  replacement and metadata removal. A pure `Probe`, never `Active`, with no
  engine or cadence counters. The `0003`+staged `aji_dispatch` `create_prepared` producer now publishes a
  real backend snapshot (§1.7); managed prepared-plan attachment and
  authoritative requested/effective/engine/cadence/error snapshots still require
  controller integration.

<a id="m20"></a>

### M20 — Consume versioned native status through the selected MpvApi

- **Purpose:** poll the metadata property on a **bounded 500 ms timer**, only
  while native is `Active`, `Preparing` or `Switching`; never inside `paintGL`,
  never per frame. Skip work when the revision is unchanged — polling is the
  mechanism, because notifications are not assumed. Parse into
  `StatusSnapshot {version, revision, typed state}` and emit a Qt signal only on
  a genuine change; unsubscribe on teardown. **The poll request itself is
  asynchronous** (`mpv_get_property_async`) tagged with the current epoch; a
  late reply for a superseded epoch is dropped. Ownership follows M19 exactly.
- **Acceptance:** the status a viewer sees is the filter's own report, not an
  echo of what was requested — a plan that failed to activate shows the old
  effective plan plus the reason; with the filter absent, status is `Unavailable`
  with a reason; polling does not measurably change frame pacing and does not
  measurably block the GUI thread (quantified in M49).
- **Depends:** M07, M19. **Status:** consumer slice implemented and **PASS**
  (previously-red `TestNativeStatus` cases now pass: malformed probe identity
  refused by `native.status.probe.identity`; sensitive tag refused without echo;
  detach-clears-view, named-refusal halt, preparing-availability and non-Active
  plan refusal slots pass). Authoritative Native snapshots remain blocked on M19;
  the existing status signal publishes a queued reason without changing
  effective/`Active` (§1.7).

<a id="m21"></a>

### M21 — Acknowledged filter epochs, drain, suspend and resume

- **Purpose:** plan changes are transactional and **acknowledged**. One
  controller per session owns requested vs effective plus a process-wide native
  work lease. Every request carries a revision; completions from obsolete
  revisions are dropped. The sequence: pause → drain pending inference tickets →
  set the named filter plan → apply a decoder-compatibility override if required
  → re-anchor the clock keeping the same PTS → **wait for the ack** → then
  restore play/pause, audio, subs and speed.
- **Acceptance:** a plan change mid-playback does not jump the position, does not
  mute, does not change speed and preserves the audio and subtitle selection;
  two rapid changes leave exactly the second one effective; a failed change
  leaves the previous effective plan intact and reports why.
- **Depends:** M19, M20. **Status:** partial transport/diagnostic evidence only
  — the ABI v2 real-context probe observed `drain=0 suspend=0 resume=0` as real
  operations and a successful post-resume real-frame completion, but no
  authoritative Active-epoch acknowledgement is proved.

<a id="m22"></a>

### M22 — Integrate named native filters with decoder and source compatibility

- **Purpose:** insert the named filter with correct mpv escaping —
  `@mikai_native:animejanai=key=value:...` with `%`-length quoting over the
  **UTF-8 byte count**, or better an mpv **command node** instead of a shell
  string. The filter has **two modes**: PROBE-ONLY PASSTHROUGH (attached only
  after Prepare/Enable, passes Original frames unchanged, no engine configure/
  deserialization/build/NN/RIFE, reports the real device UUID or adapter LUID
  plus decode geometry and CSP from the filter thread) and READY NN (attached
  only after every stage is prepared). Playback and the UI stay on Original in
  probe and preparation. **No CUDA calls or hwctx pointers cross the Qt thread.**
  If playback is paused and no frame is available, the source probe may request
  one frame at the existing PTS through the existing async source coordinator
  while preserving pause; if the source is not seekable it waits for the next
  real frame or reports `Unknown` — it never guesses. Fixed composition:
  `decode → native vf (RIFE in the recipe's order) → mpv colorscale → Anime4K
  shader hook stages`. Decoder compatibility: use the **actual** CUDA context/
  device the decoder reports, never global device 0. Preserve the HTTP headers
  and every existing mpv/vo option verbatim. A software-decoded source may need
  one GPU upload — label that honestly, do not claim NVDEC for a CPU decode, and
  do not put a GPU→CPU→GPU round trip in the steady-state inference path.
- **Acceptance:** PROBE-ONLY PASSTHROUGH attaches on demand, passes frames
  unchanged, publishes `Probe` (never `Active`) and yields a real device UUID or
  adapter LUID that a subsequent build key is computed from — preparation never
  assumes device 0; paused-with-no-frame either yields one frame at the existing
  PTS without unpausing, or reports `Unknown` on a non-seekable source; any
  context/device change re-probes; the chain mpv reports contains `mikai_native`
  in the right position relative to `colorscale` and the Anime4K hook stages;
  sources whose paths contain a drive colon, a comma or a percent all open;
  `hwdec` and the HTTP headers survive; a software-decoded source is labelled.
- **Depends:** M21. **Status:** blocked on the M19/M21 native producer and the
  coherent module.

<a id="m23"></a>

### M23 — Run generic spatial-only native model chains

- **Purpose:** run an ordered spatial chain end to end — multi-stage 2× chains
  and the 1×3 dynamic-HW compact, with per-stage pre-resize, confirming the
  output geometry is the learned one. This is the **mandatory full CUDA
  deliverable** and must work for any generic-compatible imported model, not
  only the shipped five.
- **Acceptance:** a 1080p source through a two-stage 2× chain reaches the Qt
  framebuffer at 3840×2160 with visibly correct output; the dyn-HW model handles
  a non-multiple-of-32 source without a wrong-size crash or a silently wrong
  scale; a deliberately broken stage fails loudly.
- **Depends:** M22. **Status:** execution code and callers present — managed
  ordered spatial inference, per-stage geometry and Qt admission are wired.
  No current native-active Qt framebuffer run proves the required two-stage,
  dynamic-HW or broken-stage behavior. A whole manifest-validated prepared
  recipe and installed-core integration remain open; lack of a run is not
  absence of the execution code.

<a id="m24"></a>

### M24 — Run original RIFE-only and combined native recipes

- **Purpose:** run RIFE alone, RIFE-before-spatial and spatial-before-RIFE, each
  with its rational factor, SCDetect threshold, full/lite/ensemble variant and
  optional pre-resize. A scene change returns `AJI_SCENE` and must emit a
  duplicate of frame `a`, as the reference pipeline does. Record that scene
  detection costs a D2H scalar copy and a CPU stream sync per pair.
- **Acceptance:** output frame rate is the rational factor applied to the source
  rate and the timeline stays rational — no drift and no rounding to a fixed
  multiplier; at a hard cut there is no blended ghost frame; frame counts and
  PTS are monotonic across a seek and across EOF.
- **Depends:** M23, M17. **Status:** blocked on numerically verified, fully
  prepared engines; the RIFE assets are acquired and weight-verified, not
  missing. This task is full scope — not deferred, shrunk or turned back into a
  placeholder.

<a id="m25"></a>

### M25 — Integrate and capability-gate the native DirectML spatial path

- **Purpose:** DirectML is a **conditional capability track**, not a promise.
  Integrate backend selection into the recipe model and gate the offering on a
  **real** provider plus Qt GPU-interop proof. The CUDA↔OpenGL proof does **not**
  license a claim that WGL can import D3D11. **No DirectML RIFE**: if the backend
  cannot do temporal work, say so and offer spatial-only there. No Vulkan
  requirement, and no hidden fallback to CUDA behind a DirectML label.
- **Acceptance:** on a machine without the required D3D11/Qt interop, DirectML
  rows are visible, disabled and carry a reason naming what is missing; on a
  machine with it, a spatial chain runs and the status names the provider and the
  device actually used; nothing anywhere claims DirectML RIFE.
- **Depends:** M23. **Status:** blocked — the probe is **actual** (attached,
  `3.3.0 NVIDIA 616.92`): the legacy `SHARED` D3D11 texture path creates/registers
  the texture and blits the Qt framebuffer with exact RGBA corner pixels
  (`d3dTextureImported:true`, `framebufferBlit:true`, PNG written). The
  **required** `SHARED|SHARED_NTHANDLE` contract is **not registrable**:
  `wglDXRegisterObjectNV` fails `0xC007006E`, so
  `ajiTextureContractRegistrable:false`, `directMlMayBeEnabled:false`,
  `result: CAPABILITY_UNAVAILABLE`, exit `2`, no CPU fallback. An honest
  refusal, not an NN proof.

<a id="ws-d"></a>

### Workstream D — Activation, source, PiP and lifetime (M26–M30)

<a id="m26"></a>

### M26 — Transactional requested-to-effective recipe activation

- **Purpose:** the controller exposes `requestRecipe()` — recording the
  **requested** value, persisting the tick even when the choice is unavailable,
  and publishing `effective` plus a reason as separate fields — and drives the
  M21 transaction. Native Off is the default and means **no `vf` inserted at
  all**; toggling on/off live bypasses slot 0, and a deactivated plan is
  reported as deactivated, never as active-with-nothing. **Admission is a
  distinct state:** Native Off installs no filter; Prepare/Enable admits a probe
  filter (M22) to obtain the device identity and geometry; only once every stage
  is prepared does the controller switch transactionally to READY NN. Turning
  Native Off removes the probe filter **after its ack**. **Apply is
  asynchronous** (`mpv_command_async` tagged by handle, source id and epoch). A
  command reply being accepted is **not** activation — activation completes only
  once M19's `GET_META` reports the new epoch and the effective plan is
  published.
- **Acceptance:** selecting an unavailable model leaves the request persisted
  and the effective plan unchanged, with the reason visible; turning Native off
  mid-playback removes the filter and leaves position, pause, volume and speed
  intact; turning it on mid-playback activates without a reload; a command reply
  arriving before the new epoch is published does not flip the UI to Active and
  the GUI stays responsive throughout; the Probe → Prepared → Active sequence is
  visible and ordered (Probe never reports Active, Prepared never activates, and
  Native Off removes the probe filter only after the removal is acknowledged).
- **Depends:** M22, M23. **Status:** blocked — activation acceptance requires
  post-output native epoch snapshots; command success or pure state logic is
  insufficient. Real `NativePlanLock` and a last-wins request queue exist; `Off`
  needs a successful async remove plus observed absence; timeout/cancel is never
  `Removed` (§1.7).

<a id="m27"></a>

### M27 — Handle source, geometry, cadence and discontinuity changes

- **Purpose:** a new episode, a quality switch, a dub/sub variant and an
  orientation change all invalidate the current plan's geometry; on each, cancel
  stale source jobs, re-derive the plan from the new source, and either keep,
  re-prepare, or fall back to Original with a reason. **Reconfiguration is not
  only about geometry:** a change in the decoder CUcontext / CUdevice UUID, the
  D3D11 adapter LUID, or the cadence invalidates the **active plan** and triggers
  a **fresh probe for the new epoch** — a plan is never carried across a device
  change. **Invalidating the active plan is not invalidating the persistent
  artifact:** re-probing produces new build inputs compared against M14's build
  key; if **every** input matches, the already-built, already-verified engine is
  **reused** (no rebuild). What is never shared is a live execution context or a
  captured CUDA graph. Cache reuse is keyed on **source geometry, not URL
  identity**. A discontinuity (seek, episode cut, stream recovery) must not leave
  a half-applied epoch.
- **Acceptance:** switching 1080p → 480p mid-episode re-derives the plan; an
  unprepared new plan continues on Original with a visible reason rather than
  stalling or silently mis-scaling; a device/context/adapter-LUID or cadence
  change invalidates the active plan and re-probes per epoch, and the probe
  reports the new identity before any preparation; when the re-probe resolves
  inputs that all match M14's build key the verified engine is reused with **no**
  rebuild (a rebuild on an unchanged input set is a failure of this task); a seek
  during activation does not leave a stale epoch; returning to a previously
  prepared geometry reuses the artifact without rebuilding.
- **Depends:** M26. **Status:** blocked — requires an actually active verified
  recipe and native snapshots (the queue/lock transport exists — §1.7).

<a id="m28"></a>

### M28 — Preserve playback state across native activation and media replacement

- **Purpose:** the existing resume machinery already handles position, pause,
  volume and speed across a stream swap; extend it so a **native activation** and
  a **media replacement** land on the same path. Mute, speed, volume, subtitle
  and audio-track selection must all survive. Do not disturb `keepaspect` or the
  volume-100 option. A resume after a media replacement must wait for the new
  duration, exactly as `restoreResumePoint()` already does.
- **Acceptance:** activating native mid-playback keeps position within one frame,
  keeps mute/speed/volume and the selected tracks; replacing the episode
  preserves the resume point and the paused state; a speed of 2× is not silently
  reset to 1×.
- **Depends:** M27. **Status:** blocked — requires the M26/M27 real producer;
  plain-player regressions are independently reachable (§1.7).

<a id="m29"></a>

### M29 — Preserve native processing through pooled PiP handoffs

- **Purpose:** both surfaces run the **same effective `Recipe`** and the same
  Anime4K chain — no PiP bypass, no second "safe" plan. There is one `MpvApi`
  module and **two core handles**, each with its own native epoch, CUDA context
  and output pool; no execution context is shared across cores or threads. One
  surface plays and infers at a time: entering PiP suspends and drains the parked
  core's filter, releasing TRT intermediates while keeping the GL widget and the
  last displayed frame alive; leaving restores the hand-back of float position,
  pause, mute, volume, speed, fullscreen state **and the actual native plan**. Do
  not reuse the historical "two live `QOpenGLWidget`s must never coexist" claim
  as a licence to bypass native in PiP.
- **Acceptance:** entering PiP shows the same native processing on the float; the
  parked core is suspended (proved by resource/handle observation, not by
  assertion) and no second inference lease is taken while the float plays;
  leaving PiP resumes at the float's position with mute, volume, speed,
  fullscreen and the native plan intact; a core whose plan failed to prepare
  shows Original on **both** surfaces with the same reason.
- **Depends:** M28. **Status:** blocked — requires a real native recipe,
  parked-resource acknowledgement and the one-module loader; plain PiP is
  independent (§1.7).

<a id="m30"></a>

### M30 — Release parked native resources on close and ordered shutdown

- **Purpose:** closing must stop the parked float's stream, cancel its native
  jobs, drain its tickets, detach callbacks and release its native resources —
  the hidden `pipPool_` source risk is handled at the source-risk level, without
  claiming a reproduced leak. Widget-pool lifetime stays owned by application
  shutdown ordering. **Teardown order is fixed**, and each step waits for the
  previous one: **pause → drain/suspend/cancel ACK while both the core and the
  render context are still alive → detach callbacks → free the GL render context
  with its context current → terminate the core.** The ACK is asynchronous — no
  Qt-side GPU wait and no unsafe destruction of a mid-flight worker. Repeated
  open/close cycles must not accumulate engine, core or CUDA resources.
- **Acceptance:** 100 open/close cycles with PiP entered and left each time show
  flat engine, core and CUDA memory after the first cycle and no growth trend;
  after close, no native job, ticket or callback fires; shutdown exits cleanly
  with the same shape; close completes only after the drain/suspend ack and never
  hangs waiting on a GPU.
- **Depends:** M29. **Status:** blocked — requires the real native producer;
  plain teardown regressions remain independent (§1.7).

<a id="ws-e"></a>

### Workstream E — Player UI, shaders and settings (M31–M38)

<a id="m31"></a>

### M31 — Preserve exact-frame shader Compare with native plan locking

- **Purpose:** shader Compare keeps its existing meaning over a **fixed native
  processed frame**. Freeze the native epoch and take a native-plan lock for the
  whole capture-and-repaint sequence; restore the currently applied shader on
  **every** path including cancel and failure. Native Off → the base is the
  unprocessed frame; Native On → the base is clearly labelled as Janai output
  with the current recipe and RIFE state. Native changes are queued until the
  window closes or the capture is cancelled, with a clear reason. **No native
  model swap, no reseek and no native A/B slot** is used as the A/B mechanism —
  the A/B axis remains the shader catalogue, whose ids are unchanged. The
  existing paused/frozen Compare noise-floor semantics must **pass**; do not
  disable or re-pin those tests.
- **Acceptance:** with native active, both sides of Compare render the same
  frozen source frame with the same native epoch; the base label names the native
  output and the recipe; closing Compare leaves the previously applied shader and
  the active native plan exactly as they were; a native request made while
  Compare is open is queued, and applying it afterwards is visible.
- **Depends:** M28. **Status:** blocked on the native plan-lock. A **proven code
  defect** was found and fixed: on a failed grab the capture's pause-of-playing
  was not restored (minimum fix: resume iff the grab failed and it was playing,
  `QPointer`-guarded); the fix is compiled and the current smoke **PASSES**
  (`.probe-out/native-current-compare-scoped-restore.log`, exit `0`): pixels
  noise `0.000` / single shader `0.199/255`, live side A/B QMenu picks leave the
  `glslProperty` + chain id, stored OFF untouched + close restored, a real
  failure (seek churn, noFrame) restores playing/polling/chain, a no-surface
  capture refuses by name. The destroyed-mid-grab guard is **SKIP** (no second
  surface available), so M45 stays open; the old static 61/61 is not the current
  count. The moving-source run also records noise `0.000`, shader delta `0.770`,
  seek delta `7.339` (`/255`) (`.probe-out/native-batch-compare-motion-caption.log`);
  a **native-active** frame is **not** proven, and the static fixture is only
  correct for N1, not two-position Compare (§1.7).

<a id="m32"></a>

### M32 — Prove native and Anime4K composition without changing shader identities

- **Purpose:** prove the fixed composition `decode → native vf → mpv colorscale
  → Anime4K shader hook stages` behaves, without changing a single shader
  identity, catalogue id, chain order or file. **No mandatory renderer rewrite.**
  Where a gate depends on the ratio of the actual post-native image to the
  canvas, measure that ratio rather than assuming every Anime4K upscale "does
  nothing" once native is on. Preserve the existing control-pair methodology:
  freeze one timestamp, take an untouched control pair, then measure.
- **Acceptance:** every Anime4K catalogue id and chain still resolves and stays
  byte-identical (`TestAnime4K` passes unchanged); with native off, Anime4K's
  measured effect is unchanged from the pre-change baseline; with native on, the
  composed output is measured and reported, including any stage that no longer
  has an effect and **why**; a shader error under native processing is surfaced
  as an Anime4K error, not swallowed.
- **Depends:** M31, M23. **Status:** blocked — requires real spatial output;
  unchanged shader identities remain independently testable.

<a id="m33"></a>

### M33 — Surface native choices in the existing enhancement tree

- **Purpose:** put the native branch inside the **existing** enhancement tree and
  sparkle/dock button — **no seventh button, no new top-level window**. The RIFE
  placeholder leaf becomes a real capability subtree with the current RIFE rows
  and a live capability status each; RealESRGAN and "other models" stay disabled
  placeholders. Rows are `MenuEntry::disabled` when unavailable and always carry
  a tooltip naming the reason. Preserve the tree's existing behaviour: opening
  always resets to the root, submenu rows leave the popover open, `hasChildren`
  drives drill-down, and the marked-current row uses a **stable id**, never a
  menu index or enum ordinal.
- **Acceptance:** the enhancement root shows a native branch with a working
  submenu; unavailable providers/models appear, are visibly disabled, and their
  tooltip states why; the current selection is marked by stable id and survives a
  menu reorder; opening the tree always lands on the root; existing Anime4K rows
  are unchanged.
- **Depends:** M20, M26. **Status:** Standard desktop menu smoke passes (real
  clicks open the native branch; unavailable choices stay disabled with tooltips;
  stored request/current marks survive reopen; Off clears the request without
  invoking media recovery; `native_menu_probe` OK). Native-on acceptance still
  requires M26; no seventh button or effective activation claimed.

<a id="m34"></a>

### M34 — Compact processing settings and versioned preference persistence

- **Purpose:** add a compact **Processing** section to the existing settings
  dialog, themed with the existing tokens — no new dialog window and no new
  visual language. Persist under a **new, versioned** group (`player/native`).
  **Additive only:** existing `player/anime4k`, `player/presetShortcuts`, the
  quality cache, feeds, home/feeds and language settings are never repurposed or
  cleared. The native namespace is separate from the Anime4K preset-key space,
  and a **shared conflict check** refuses a native key that collides with an
  existing reserved or preset key. A saved tick for an unavailable choice
  persists anyway, with the effective value and reason shown separately.
  `MainWindow::openSettings()` must **not** trigger an API cache reset because a
  native preference changed.
- **Acceptance:** changing a native preference and reopening settings shows the
  stored value, including for a currently unavailable choice; no existing setting
  is lost or reset by a native preference change; a key that collides with a
  reserved or preset key is refused with a message naming the owner; opening
  settings changes nothing in the quality cache or the feed state.
- **Depends:** M33. **Status:** implemented, dialog verification in progress —
  additive versioned preferences, Processing settings, RIFE-only/combined
  requests, exact threshold editing and future-schema preservation are
  implemented; the actual UK RIFE settings consumer run now exits `0`
  (Save/Cancel/store/reload/`3:2`/order/threshold/min `560x600`, §1.7);
  environment-file preservation checks remain open.

<a id="m35"></a>

### M35 — Model import, asset preparation and safe cache management controls

- **Purpose:** in the Processing section: import a model file, show the
  inventory (label, role, dtype, size, backend compatibility, terms, content ID),
  download or prepare assets, view preparation state per stage, and clean the
  engine cache **safely** — the cleanup may delete only entries under the
  app-owned `AppLocalDataLocation/native/artifacts/` (plus a cleared `scratch/`)
  whose manifests validate, never anything in a user or shared directory. Import
  must run the M11 inspection and refuse an incompatible model with the actual
  reason. Deleting an engine in use must be refused while a plan references it.
- **Acceptance:** importing a compatible model adds exactly one inventory entry
  and it is immediately selectable; importing an incompatible one adds nothing
  and says why; "Clean cache" removes only app-owned validated artifacts and
  leaves user files byte-identical; cleaning an artifact in use is refused with a
  reason; a preparation triggered from here updates the panel live.
- **Depends:** M34, M11, M15. **Status:** implemented portions unverified —
  owned import/inventory/cache panel and process-wide artifact leases landed;
  cancellation now gates publication; actual preparation now has a genuine private
  helper/source (M15, §1.7); no engine progress or Ready capability is fabricated.

<a id="m36"></a>

### M36 — Native shortcut bindings with shared collision checks

- **Purpose:** add a **separate** `player/native/shortcuts` group with its own
  stable action ids, reusing — not duplicating — the reserved-key predicate and
  the duplicate-claim check the preset bindings already use. A native binding
  must be refused if it collides with a reserved player key, an existing preset
  binding, or another native binding. A stale or now-reserved stored binding is
  skipped at load exactly as a preset binding is, never installed over a key the
  player acts on. On every path, a binding is written only once the whole set
  validates.
- **Acceptance:** binding a reserved key is refused with the owner named; binding
  a key already used by an Anime4K preset is refused; duplicate native bindings
  are refused; a stored binding that has since become reserved is skipped and
  reported; valid bindings install and fire the native action; no existing preset
  binding changes behaviour.
- **Depends:** M34. **Status:** independent binding/request path verified —
  shared collision checks and persistence pass; `native_menu_probe` exercises
  actual stored/default/cleared bindings, a non-Off preset collision and a
  reserved fullscreen key. Prepared native activation remains unverified; no
  saved request claims Active.

<a id="m37"></a>

### M37 — Truthful native processing and cadence status in the HUD

- **Purpose:** show the selected provider, model, source→output geometry, fps
  and the rational active-interpolation factor, driven from M20's snapshot. **The
  existing `estimated-vf-fps` remains an mpv-measured output indicator — it is
  not inference proof and not a source work rate**, and must be labelled so. A
  native "active" claim comes from the filter-reported effective plan, not the
  HUD. The HUD must show `Preparing` and the reason, not a blank or an optimistic
  spinner, and must reflect `Unavailable` truthfully.
- **Acceptance:** with native active the HUD names the provider, model, geometry
  and interpolation factor; with native preparing it says so with the reason;
  with native unavailable it says why; the label for `estimated-vf-fps` makes no
  claim about inference; the HUD never reports `Active` before the filter's
  snapshot does.
- **Depends:** M20, M33. **Status:** HUD integration is present —
  dialog status updates reach `PlayerShell::updateNativeHud`, which reads the
  typed snapshot and labels FPS provenance. Native-active surface verification
  remains open. The
  corrected `NativeHudProbe` builds and runs (exit `0`), writing a real `909x32`
  HUD PNG: requested-but-not-active shown, FPS output provenance, counters never
  shown as FPS. Presentation fixture only — no NN runtime or effective-plan
  proof; an earlier fabricated Python "screenshot" was rejected and deleted.

<a id="m38"></a>

### M38 — English/Ukrainian, accessibility and geometry coverage

- **Purpose:** every native-facing string is an English `tr()` source with a
  Ukrainian translation, including unavailable reasons, preparation stage names,
  provider/model labels and error text. Preserve Qt placeholders `%1`/`%n`,
  numerus forms and markup. **Do not translate** stable IDs, capability enums,
  model content hashes, protocol/schema version strings, paths or backend names.
  Native settings and menu rows follow the same accessibility contract as the
  rest of the player: a tooltip and an accessible name on every interactive row,
  keyboard reachability through the existing `MenuPopup` arrow-key model, and
  layout that survives long labels and long reasons at the smallest supported
  window size. The catalog must contain no unfinished entries before release.
- **Acceptance:** every native string appears in `mikai_uk.ts` with a real
  translation; no unfinished entry remains; `%1`/`%n` placeholders and numerus
  forms are preserved; switching language updates settings, menus and the HUD
  without a restart of the player; long reason text wraps or ellipsises without
  clipping a row; every interactive native row is keyboard reachable and has an
  accessible name.
- **Depends:** M34, M36. **Status:** catalog compilation passes and geometry
  **PASS** — Qt lrelease reports 0 unfinished Ukrainian entries (no-unfinished,
  placeholder and numerus rules retained); the geometry probe passes on the
  current source in all four runs
  (en/uk × DPR1/DPR2) at minimum `560x600`, with tab bar, actions (`24px`),
  buddies, reachability, no horizontal scroll and Save/Cancel all PASS
  (`.probe-out/native-factory-corrected-locale-geometry-en-dpr{1,2}.log`,
  `.probe-out/native-factory-corrected-installed-geometry-uk-dpr{1,2}.log`).
  Older logs erroneously tagged `EN lang --language uk` are **not** UK proof and
  are not cited. Artifact 361 passed English/Ukrainian access to all 47 RIFE
  and five spatial choices, language Save/refusal, and Save/Cancel at 760x780,
  DPR 2; the latest catalog has 999 finished and zero unfinished translations.
  The actual UK RIFE settings consumer run exits `0` (§1.7). M38 remains partial:
  final placeholder/in-context checks and live language updates are not
  implemented; Settings saves `ui/language` and explicitly
  requires a restart, contrary to this task's live-update acceptance.

<a id="ws-f"></a>

### Workstream F — Media recovery and trust (M39–M40)

<a id="m39"></a>

### M39 — Separate processing failures from media retry and signed-URL recovery

- **Purpose:** classify engine errors into **media** failures and **processing**
  failures. A media failure keeps today's behaviour exactly: the player shows the
  error with the retry chip, `retryPlayback()` retries the same URL, and for an
  anime host `playbackFailed()` re-extracts via `recoverStream()` — the 30-minute
  TTL, the `forget()` on failure, the episode/dub/sub/quality preference
  preservation and the "does not rewind to the variant it opened with" rule all
  stay. A **processing** failure must never trigger a stream re-extraction, must
  not consume API rate budget, and must not masquerade as an expired link: it
  reports its own reason and offers Prepare / switch provider / stay on Original.
  A user-requested frame capture that fails is a capture failure, not a playback
  failure.
- **Acceptance:** an expired signed URL still shows the retry chip and recovers
  through re-extraction; a native preparation or engine failure shows a
  processing reason with processing remedies, triggers **no** API call and does
  not change the URL; a direct-media (non-anime) player gets no anime host
  controls on either path.
- **Depends:** M34, M20. **Status:** separation implemented:
  `reportNativeProcessingFailure` uses `showProcessingStatus`, not media retry
  or host re-extraction; processing requests and provider/preparation controls
  remain separate from `retryPlayback`. Actual player-level coverage of both
  failure paths and the offered remedies remains incomplete; source wiring is
  not native-active acceptance.

<a id="m40"></a>

### M40 — Harden runtime, model, artifact, path and diagnostic trust boundaries

- **Purpose:** every path that reaches native code is validated: no traversal, no
  UNC surprise, no unquoted metacharacters, no over-long paths, and no directory
  ever chosen from a manifest without checking it is inside the app-owned root.
  Model imports are validated before touching the inventory. Artifacts are
  validated before any deserialize. **Diagnostics must never contain URL tokens
  or API keys** — the native plan follows the source's w/h/fps, not URL identity;
  scrub the native status, the manifest and any exported diagnostic of anything
  URL-shaped. Keep the existing download safety in `Runtime.cpp` intact. **Unicode
  paths throughout:** every native load goes through the wide `LoadLibraryExW`
  with a proper UTF-8 → UTF-16 conversion, and a path outside the native
  filesystem's representable set is rejected with a named reason.
- **Acceptance:** a manifest naming a path outside the app-owned root is
  rejected; a traversal-shaped model path is rejected; an exported diagnostic and
  the status snapshot contain no URL, token or API key; a crafted path with a
  drive colon, comma, percent or non-ASCII byte never corrupts the filter command
  or the log.
- **Depends:** M34, M18. **Status:** independent hardening implemented,
  unverified — status/artifact diagnostic guards reject sensitive identities and
  redact exported URLs/credentials; ownership/path validations exist. Hostile-
  input CPU and surface proof remains; native deserialize acceptance still
  requires the M18 prepared producer. The source audit did not establish the
  required over-long-path, UNC-surprise and native-filesystem representability
  refusals. Containment and UTF-8 command escaping are not proof of those
  separate requirements.

<a id="ws-g"></a>

### Workstream G — End-to-end correctness, fidelity and performance (M41–M49)

<a id="m41"></a>

### M41 — Focused consumer-visible CPU regression coverage

- **Purpose:** cover the pure, CPU-testable contracts from M10–M14, M34, M36,
  M39 and M40: recipe round-trip and ordering; availability reasons; manifest
  identity and invalidation; preparation-plan geometry over the M02 matrix;
  preference persistence and migration; shortcut collision; failure
  classification; bundle validation; and the structured status snapshot
  (requested vs effective, revision monotonicity, protocol mismatch). Tests must
  assert **consumer-visible behaviour and boundaries** — an exact binding
  mismatch, a precedence rule, a transition, an error — never that a function was
  called, never a mock echo, never "not empty", never a re-assertion of source
  wording. Do not resurrect anything deleted in M01.
- **Acceptance:** every new suite is registered in `main.cpp` and in the
  `CMakeLists.txt` source list; `ctest --test-dir build-cmake -R mikai_tests`
  exits 0 hermetically with `MIKAI_LIVE` unset and `TestLiveOracle` **skipping**;
  each new suite fails when its contract is broken (deliberately verified by a
  temporary mutation, reverted afterwards).
- **Depends:** M40, M38, M39, M37. **Status:** source settled 2026-10-04; the
  latest complete CPU matrices are ON `21.48 s` / OFF `15.92 s` (§1.6/§1.7). The old incidental components doc-source JSON assertions
  were removed (mandated, not repinned) and 3 honest `blockReasons` were
  separately repaired. A green matrix does not prove the required deliberate
  contract-breaking mutation check; no such evidence was established by the
  source audit.

<a id="m42"></a>

### M42 — Prove fresh plain-player and native-off desktop regressions

- **Purpose:** rebuild every probe against the **current** source through the new
  `MpvApi` and re-establish the pre-change desktop behaviour: real window, real
  GL, non-black framebuffer, real `estimated-vf-fps`, real decoder/VO drop
  counts, working close/reopen, and no crash on shutdown. All must run with the
  **standard** bundle and native off, and none may depend on a network, a key or
  an ambient install. No native-on acceptance is inferred. Widget raw metric
  access and explicit filter absence remain unproved; no unavailable counter is
  invented.
- **Acceptance:** every probe builds and runs on a freshly configured tree with
  exit 0 (or an explicitly reported skip), reports real numbers (`hasVideo`,
  playing, position, duration, video size, context ready, non-black percentage,
  drop counts) and closes cleanly; `widget_pixel_probe` and `mpv_surface_smoke`
  show a real non-black frame with the standard bundle and native off; any probe
  failure is classified as app defect versus harness/asset gap.
- **Depends:** M41, M07. **Status:** Standard desktop regression slice verified;
  full metric gate open. Current attached runs: `interaction_probe` 149/149; PiP
  pool 159/159 over 3 runs; `compare_probe` 61/61 over 3 runs (noise 0.000,
  shader diff .734); `widget_pixel_probe` 25, `dialog_probe` 20, `shell_probe`
  10 and `anime4k_menu_probe` 148 on the actual framebuffer (attached GL 4.6,
  NVIDIA 616.92); `hls_recovery_probe` 23/23 and `external_media_probe` 21/21.
  Title API `7428`/COVER returned OK but did **not** terminate (killed; no
  exit-`0` claim). Loader/runtime-selection PASS, with the Native case falling
  back to Standard — **not** a Native proof.

<a id="m43"></a>

### M43 — Prove CUDA native inference reaches the actual Qt framebuffer

- **Purpose:** prove, end to end and on **current source**, that CUDA inference
  output actually reaches the Qt framebuffer: decode → native vf → colorscale →
  render API → the `QOpenGLWidget`'s own FBO. Measure the framebuffer the player
  actually shows (mean, variance, non-black fraction), save PNGs for visual
  inspection, and confirm the output geometry matches the learned plan; compare
  against native-off on the same fixture. Do **not** present this as performance
  or fidelity certification, and do not claim a zero-copy path. If the debug
  `GL_INVALID_ENUM` reappears, disclose it as unresolved.
- **Acceptance (conditional):** on a **seekable** fixture (or a valid cached
  frame), Prepare pressed while **paused** yields a real device UUID (or adapter
  LUID) and a resolved output state **without unpausing**, and the status shows
  Probe before Prepare and the resolved output state after it. On a
  **non-seekable** source with no frame available, the identity stays **Unknown**,
  playback stays on Original, the user is **not** unpaused, and no identity is
  guessed. With native active the framebuffer is non-black, its geometry matches
  the plan, and its content visibly differs from native off on a real frame. The
  evidence includes saved PNGs and measured statistics, not exit code alone; any
  GL debug error is reported explicitly.
- **Depends:** M42, M23, M32. **Status:** blocked — requires the licensed native
  runtime and real prepared engines. `NativeFramebufferProbe` is complete and
  built ON/OFF but **not run**, so no Qt-framebuffer N1 proof exists (§1.7).

<a id="m44"></a>

### M44 — Prove RIFE timing, cuts, padding, seeks and end-of-stream behaviour

- **Purpose:** measure real RIFE behaviour with real video: output frame rate
  equals the rational factor times the source rate and stays rational over a long
  clip (no drift, no rounding); a hard cut produces **no** blended ghost frame
  (the `AJI_SCENE` path emits a duplicate of frame `a`); padding at the first and
  last frames is correct; a seek mid-stream re-anchors PTS correctly and the
  timeline stays monotonic; EOF and near-EOF do not hang or emit frames past the
  end; audio stays in sync with the interpolated video. Compare against the same
  fixture with RIFE off.
- **Acceptance:** frame counts and PTS are monotonic across seeks and EOF; the
  cut frame is a duplicate of `a`, not a blend; drift over the clip stays within
  one frame; A/V offset is measured and reported, not assumed; every claim is
  backed by a recorded number.
- **Depends:** M43, M24. **Status:** blocked — requires M12's numerical
  acceptance (full/ensemble rejected on `maxAbsoluteError`) and the real licensed
  runtime; the RIFE assets are present, so this is not a missing-asset blocker.
  Artifact 366 passed one selected v4.14 lite run's fresh build/render and
  relative presentation-output count (145 versus baseline 73 over 3 seconds;
  1.5x minimum passed), with pause/seek and audio/subtitle control checks. It
  does not prove exact rational output rate over a long clip, cut behavior,
  padding/edge behavior, EOF, or A/V sync; M44 remains incomplete.

<a id="m45"></a>

### M45 — Stress native PiP, Compare, close and resource lifetimes

- **Purpose:** run the two-core, Compare and close paths hard: 100 PiP
  enter/leave cycles, 100 Compare open/close cycles, 100 player open/close cycles
  with a native plan active, interleaved, with resource sampling throughout (GPU
  memory, CUDA context count, engine handles, thread count, file handles).
  Include the close-while-PiP case that today only calls `leavePip(true)`. Assert
  flat resources after the first cycle and no leak trend; assert no crash and no
  driver fault.
- **Acceptance:** resources return to baseline after the first cycle and stay
  flat; no crash, no hang, no GPU fault across 100 cycles each; a close during
  PiP releases the parked float's stream and native resources; a device, context
  or cadence change during a handoff invalidates the plan and re-probes for the
  new epoch rather than carrying a stale build key across — the stress run must
  exercise that path, not just the identical-device happy case.
- **Depends:** M44, M30, M31, M32. **Status:** blocked — requires real active
  native recipes; plain pooled PiP/Compare coverage remains reachable.

<a id="m46"></a>

### M46 — Prove native local/HTTP/HLS and anime session transitions

- **Purpose:** prove the full media matrix with native processing enabled: a
  local file, a `file://` URL, an HTTP progressive stream, and an adaptive HLS
  manifest with variant switching — plus the anime session transitions: quality
  change, episode change, dub/sub variant change, and stream-expiry recovery.
  Every one must keep the native plan correct for the new source geometry, keep
  the user's quality/episode/dub/sub preferences, and preserve the HTTP headers
  the CDN requires. The native plan follows source w/h/fps, not URL identity. A
  live CDN is **optional** — the hermetic M02 fixtures are the required proof.
- **Acceptance:** each media kind plays with native active and reaches the
  prepared plan for its geometry; quality and episode switches re-derive the
  plan; when the new geometry is unprepared, playback continues on Original with
  a visible reason; an expired signed URL recovers through the existing
  re-extraction path and does not disturb the native plan; preferences survive
  every transition.
- **Depends:** M02, M28. **Status:** blocked — requires real recipes; the local
  HTTP/HLS fixture and native-off playback checks are independently reachable,
  and the stock external `21/21` / HLS `23/23` runs exit `0` (§1.7).

<a id="m47"></a>

### M47 — Prove preparation and artifact failure recovery

- **Purpose:** run the full negative matrix against real artefacts, not mocks:
  **cold** (no engines — Prepare from scratch), **corrupt** (truncated engine, a
  manifest that does not match, a foreign engine file dropped into the cache),
  **stale** (driver or model changed under an existing engine), **missing
  asset**, **cancelled**, **builder failure**, and **disk-full-ish** (a full temp
  volume). For each, the installed bundle, the reported status, the visible state
  and the pixels must all tell the truth: no silent pass, no log tail, no
  half-activated chain, no leftover partial artifact, and Original still
  playable. Recovery must be reachable without a restart.
- **Acceptance:** each failure mode produces a named reason, keeps Original
  playing, and leaves the cache in a state the next run can use; a corrupt or
  mismatched engine is never deserialized; cancelling mid-build leaves nothing
  loadable behind; no failure mode prints a bare "exit 0" success.
- **Depends:** M14, M15, M18, M28, M35. **Status:** partial observed evidence:
  the real prepared CAPI probe exercises named corrupt/mismatched-engine
  refusals, unchanged engine bytes, suspend/resume/drain and epoch handling
  (`.probe-out/native-batch-prepared-snapshot-proof.log`). This is not the full
  player-level failure matrix or proof that Original keeps playing; it is no
  longer accurate to describe the helper and produced engine as absent.

<a id="m48"></a>

### M48 — Prove DirectML provider identity and GPU rendering where offered

- **Purpose:** on a DirectML-capable machine, prove the real thing: the actual
  provider and device identity, that Qt's GL surface can actually receive
  DirectML-produced frames (the interop the CUDA path does **not** prove), and
  that a spatial chain runs and is pixel-verified. Where the proof fails, the
  capability stays disabled with a factual reason and that outcome is recorded —
  a disabled DirectML track is a valid, honest delivery. Record whether a debug
  GL error appears.
- **Acceptance:** with DirectML proven, the status names the provider and device
  and the framebuffer is pixel-verified; without the proof, the UI shows DirectML
  as disabled with the specific missing capability, and no part of the app claims
  DirectML support; no DirectML RIFE is offered or claimed.
- **Depends:** M25, M43. **Status:** blocked — **D3D11 → WGL interop is
  unproven.** The probe is actual on an attached session: the legacy `SHARED`
  path imports the D3D11 texture and blits exact RGBA corner pixels, while the
  required `SHARED|SHARED_NTHANDLE` registration fails `wglDXRegisterObjectNV`
  `0xC007006E` → `CAPABILITY_UNAVAILABLE`, exit `2`, `directMlMayBeEnabled:false`,
  no CPU fallback. An honest refusal, not an NN proof.

<a id="m49"></a>

### M49 — Measure sustained processing, responsiveness and resource stability

- **Purpose:** measure, over a long warm run (tens of minutes) with real media:
  sustained processed frame rate and its variance; per-stage and end-to-end GPU
  latency; CPU and GPU memory over time; engine load time; and **GUI
  responsiveness** during native apply, status polling, preparation and PiP
  handoff — measured as input-to-paint latency and event-loop lag, not asserted.
  This is the **responsiveness gate** for the whole async design: the event loop
  must stay responsive while the native core configures, builds, drains and
  suspends on its own thread; the 500 ms status poll is async (M20), plan apply
  is async (M26), and both coalesce into the existing 50 ms bank (M07). Measure
  the lag during each of those four operations separately. Compare native-off,
  spatial-only, RIFE-only and combined where available. **Comparative throughput
  gate:** compare Mikai with standalone Janai/mpv using identical source
  bytes/time range, model digests, ordered recipe/RIFE factor, precision/TF32/
  build options, stage and output geometry, GPU, driver and CUDA/TensorRT
  versions; match decode mode, playback speed, OpenGL render size, shaders and
  presentation limits; record any unavoidable mismatch instead of treating it as
  a speedup; prepare both before measurement and run them sequentially with
  exclusive GPU access, never building an engine alongside active inference.
  Record throughput/latency distributions including repeat-run variation;
  display-refresh or playback caps must not be mistaken for an inference
  ceiling; `estimated-vf-fps` alone proves neither inference nor maximum speed.
  Trace full-frame H2D/D2H/D2D transfers and CUDA↔OpenGL synchronization stalls;
  distinguish prohibited full-frame GPU→CPU→GPU inference transport from
  permitted software-decode upload, device copies and RIFE's scalar D2H scene
  check/synchronize. Standalone Janai/mpv is an isolated benchmark reference,
  never a production playback subprocess or runtime/cache dependency; any
  "maximum speed" statement must be bounded to the measured matched workloads and
  hardware.
- **Acceptance:** numbers are recorded per configuration with the machine and
  inputs stated, and per operation (apply / poll / prepare / handoff) rather than
  only in aggregate; event-loop lag stays bounded in each of the four; the
  numbers are stated as observations against whatever reference was used; memory
  is flat over the run after warm-up; frame pacing shows no long stall at
  activation; matched standalone Janai/mpv results, relative throughput/latency,
  workload/version identities and transfer/synchronization evidence are recorded
  for each offered native configuration; differences and measurement limits are
  explicit; no absolute maximum-throughput claim follows; any measurement that
  cannot be taken is reported as not measured, not estimated.
- **Depends:** M41, M42, M43, M44, M45, M46, M47, M48. **Status:** blocked —
  requires functional prepared spatial/temporal processing; no fabricated
  performance.

<a id="ws-h"></a>

### Workstream H — Distribution, docs and release (M50–M52)

<a id="m50"></a>

### M50 — Prove independent clean installation, upgrade and rollback

- **Purpose:** prove the shipped artefact works on a **clean machine with no
  Python, no Conda, no ambient CUDA/TensorRT/ONNX install, and no network**.
  Install the bundle, verify the self-contained dependency closure, run the
  standard and native paths, upgrade from a previous bundle version, and roll
  back to it — with the app detecting and reporting each state. Prove an offline
  baseline, that a restart after a selection change picks the right bundle, and
  that rollback leaves a working app. **Licensed bundle only**: if M03 blocked
  any component, this task cannot pass and must say so.
- **Acceptance:** a clean, network-free machine runs both paths with no ambient
  installs; an upgrade preserves preferences and rebuilds nothing it need not; a
  rollback restores the previous working state; a failed install leaves the
  previous bundle intact and usable; restart-after-selection is deterministic.
- **Depends:** M05, M08, M49. **Status:** blocked — the real coherent native
  bundle is absent and a shipped bundle has **no third-party clearance** (the
  owner grant covers this project's own integration only; the shim's upstream
  terms are not verified and the permission request stays unsent).

<a id="m51"></a>

### M51 — Update this project's user and maintainer documentation

- **Purpose:** update the documentation to match what actually landed.
  Maintainer side: the `MpvApi` loader contract and why no DLL is linked at load
  time; the bundle layout and the atomic install; the native patch set and the
  `preparedOnly`/manifest/`GET_META` semantics; the preparation CLI and that it
  is out-of-band; the DirectML gate and what "disabled with a reason" means; the
  new suites and probes and how to run each; the `MPV_INCLUDE_DIR` rename and the
  updated configure command. User side: turning native on, preparing models,
  what `Preparing` means, what an unavailable provider looks like, and that
  Anime4K and native compose rather than replace each other. Correct any place
  the docs still describe the pre-change behaviour (including the stale
  `provisionMpvRuntime()` ordering comment and the "RIFE is planned" leaf).
  **Only describe verified behaviour.**
- **Acceptance:** every documented command runs as written; no document claims a
  capability that M42–M50 did not verify; the old
  `MPV_RUNTIME_DIR`/linked-`MPV_LIBRARY` instructions are gone everywhere,
  including `CMakeLists.txt` comments.
- **Depends:** M49, M50. **Status:** independent documentation reconciled to the
  current audit; native preparation/provider operation remains unproved; M49/M50
  and full-release documentation acceptance stay open (current-wave facts: §1.7).

<a id="m52"></a>

### M52 — Complete the full no-regression release acceptance matrix

- **Purpose:** run the terminal gate over the whole matrix and record evidence
  for every cell, including the cells that are **not** satisfied: the
  prepared/cold/corrupt/status negative matrix; RIFE real-video cuts/EOF/seek/
  rational counts/A-V; native PiP and Compare across two cores and 100 cycles;
  DirectML capability where offered and its honest disabled state elsewhere; long
  warm performance and responsiveness and memory; a clean machine with no Python,
  no Conda and offline; restart and rollback; and the licensed bundle. Confirm
  the catalog has no unfinished entries; confirm every existing probe and suite
  still passes; confirm no task is marked complete while a reachable acceptance
  is unrun. **Do not ship with implementation tasks ticked and acceptance
  unexecuted.**
- **Acceptance:** every matrix cell has an evidence pointer (command, saved
  artefact or recorded measurement) or an explicit, justified `not satisfied`
  with the blocker named; any unsatisfied cell blocks release; the matrix is
  reproducible from a clean checkout by someone who did not write the code.
- **Depends:** M51, M41, M43, M45, M46, M47, M48. **Status:** blocked — the
  full release matrix remains incomplete until every native and independent
  acceptance gate passes.

---

## 3. Cross-cutting non-negotiables

These hold for every task above; a task that violates one is wrong, not
"acceptable for now".

1. **No Python, no AnimeInterp dependency, no Conda/torch, no external process
   runtime.** Preparation may use an explicit native CLI through `QProcess` —
   that is an out-of-band build/inference tool, never a playback subprocess and
   never a decoder/render/frame dependency.
2. **One `MpvApi` module, many core handles.** Two live cores for PiP are
   required and correct; a hot module swap is not permitted.
3. **Preparation is explicit and cancellable.** No first-play build, no
   background build, no log-tail "did it work", no silent pass.
4. **Full scope.** Spatial-only, original RIFE-only and combined are all in
   scope. DirectML spatial is a conditional track that ships disabled-with-reason
   when unproven. None of these is a v1, MVP, foundation or follow-up.
5. **Preserve what exists.** API, streams, language, theme and settings;
   Anime4K catalogue ids and chain order; the 30-minute quality TTL; the episode,
   dub, sub and quality preferences; the signed-URL recovery path; the shader
   Compare semantics and its noise floor; the PiP hand-back state.
6. **No PiP bypass, no NN Compare A/B.** PiP runs the same effective plan;
   Compare's A/B axis stays the shader catalogue.
7. **Honest data-path claims.** CUDA↔Qt GL interop does device copies and a sync;
   RIFE scene detection does a D2H scalar copy plus a CPU stream sync per pair.
   Neither may be described as zero-copy or "no device transfers".
8. **Delete obsolete incidental tests; never re-pin them.**
9. **Truthful status.** Requested and effective are separate; an unavailable
   capability is visible with a factual reason, not hidden and not silently
   substituted.
10. **Proof is measurement.** Exit code 0, a non-empty string, or a mock echo is
    not acceptance. Real frames, real numbers, saved artefacts.

---

## 4. Acceptance catalog (M/C/R/N) and observed status

<a id="catalog"></a>

This appendix is the **full acceptance catalog**: the M index, the C ledger of
observed repairs, the R no-regression rows and the N native acceptance rows. The
R and N rows previously lived in `plan.md`; this is now their single canonical
home, and `plan.md` links here rather than keeping a second copy.

### 4.1 M acceptance index

<a id="catalog-m"></a>

Each M task carries its own full `Acceptance:` bullets in §2. This index is the
navigation map from workstream to task; `M52` is the sole terminal task and walks
the release gate.

| WS | Tasks |
| --- | --- |
| A | [M01](#m01), [M02](#m02), [M03](#m03), [M04](#m04), [M05](#m05), [M06](#m06), [M07](#m07), [M08](#m08), [M09](#m09), [M10](#m10) |
| B | [M11](#m11), [M12](#m12), [M13](#m13), [M14](#m14), [M15](#m15), [M16](#m16), [M17](#m17), [M18](#m18) |
| C | [M19](#m19), [M20](#m20), [M21](#m21), [M22](#m22), [M23](#m23), [M24](#m24), [M25](#m25) |
| D | [M26](#m26), [M27](#m27), [M28](#m28), [M29](#m29), [M30](#m30) |
| E | [M31](#m31), [M32](#m32), [M33](#m33), [M34](#m34), [M35](#m35), [M36](#m36), [M37](#m37), [M38](#m38) |
| F | [M39](#m39), [M40](#m40) |
| G | [M41](#m41), [M42](#m42), [M43](#m43), [M44](#m44), [M45](#m45), [M46](#m46), [M47](#m47), [M48](#m48), [M49](#m49) |
| H | [M50](#m50), [M51](#m51), [M52](#m52) |

### 4.2 C acceptance and observed repairs (C01–C22)

<a id="catalog-c"></a>

The C ledger records the verification/repair work executed outside the M
packages. It is evidence and an acceptance record, not a second task list.

<a id="c01"></a>
- [x] **C01 — Resolve verification restriction; run the fresh isolated baseline.**
  Historical: isolated OFF/ON 24/25 suites PASS + LiveOracle SKIP. Settled
  source: full `ctest` green ON/OFF, recorded before the later CRT/source updates
  (see §1.6); the attached current-tree
  widget renders a moving 640×360 frame with an active GL logger, ErrorType 0 and
  `glGetError` 0. M42's full metric/filter-observation gate remains open, not the
  baseline run.

<a id="c02"></a>
- [x] **C02 — Record private non-commercial scope and draft an unsent inference permission request.**
  Recorded in `native/permission-request.md`; the draft was not sent and grants
  no permission.

<a id="c03"></a>
- [x] **C03 — Obtain and configure official native development SDKs without replacing installations.**
  Isolated official SDK acquisition and genuine MSVC/CUDA/TensorRT/GPU smoke
  evidence are recorded in `native/sdk-acquisition.md`. No driver, administrator
  install or existing runtime was replaced.

<a id="c04"></a>
- [ ] **C04 — Implement legally independent player-side slices with explicit prerequisites.**
  Loader, bundles, recipes, planning, artifacts, inspection, preferences and
  status consumers have landed edits or active owners. Acceptance is tracked
  under the individual M tasks, never inferred from a worker handoff.

<a id="c05"></a>
- [ ] **C05 — Record per-task observed evidence and precise blocker scope.**
  Status lines distinguish pending work, implemented-but-unverified edits and
  named Native/provenance prerequisites. Final verification records remain open.

<a id="c06"></a>
- [x] **C06 — Use m5/GPT-6-Luna for scout jobs.**
  Bounded read-only source scouts completed and returned integration findings.

<a id="c07"></a>
- [x] **C07 — Use m6/DeepSeek V4.1 Flash for deep implementation jobs.**
  Deep implementation slices use m6. Interrupted HTTP-400 worker edits remain
  unverified; fresh bounded correlation/status/preference owners have handed off
  their source without claiming runtime acceptance.

<a id="c08"></a>
- [x] **C08 — Keep todos and tasks.md synchronized with observed progress.**
  All 52 original tasks remain tracked. Every blocked task names its real
  prerequisite; implementation status is not an acceptance tick. C13–C22 track
  observed repair work rather than hiding it in pending parent tasks.

<a id="c09"></a>
- [x] **C09 — Inspect VideoJaNai 2.1.0 manifests, all supplied asset pins and component provenance.**
  Recorded the 14 uploaded asset identities plus two generated source archive
  URLs; six acquired component/metadata digests were independently recomputed.
  See `native/videojanai-acquisition.md` and
  `native/manifest/videojanai-release.json`.

<a id="c10"></a>
- [x] **C10 — Verify SM120, PTX and TensorRT runtime compatibility without changing the NVIDIA driver.**
  The pack's declared TensorRT 11.0 / CUDA 13.2 line differs from the owned
  11.3.0.99 / 13.4.92 SDK; they stay separate and shared DLL names are not
  compatibility proof. The **official matched runtime + SM120 resource** is an
  actual build+infer **PASS** on the same RTX 5090 / driver `616.92`; the project
  11.3.0.99 / CUDA 13.4.92 cohort is **newer and incompatible** with the pack's
  11.0 / CUDA 13.2 engines/resources in both directions. A **PTX-only** closure
  **fails at `createInferBuilder`** Error Code 6 (`Unable to load library:
  nvinfer_builder_resource_sm120_11.dll: 126`, `libLoader.cpp:302`) — a precise
  **missing-SM120-resource** refusal, not a global "no PTX" claim; no SM was
  renamed or mixed. The PTX path carries **no Engine/NN/JIT proof**; N10 full
  fidelity remains **OPEN**. Evidence: `.probe-out/c10-ptx-only-build.log`.

<a id="c11"></a>
- [x] **C11 — Check component-rife against the required original RIFE variants and weight lineage.**
  All four exact 4.14 graphs match the acquired files byte-for-byte; 128/128
  weight names/shapes and FP32→FP16 rounding bytes match the author's MIT
  checkpoints. Exporter identity, non-weight graph material and whole-graph
  redistribution remain unresolved. M12 has no native execution or Ready proof.

<a id="c12"></a>
- [ ] **C12 — Preserve declared HD metadata; require real reference and GPU-binding proof for symbolic output batch.**
  Owner selected the verified-output policy after real ORT inspection exposed the
  four HD graphs' symbolic output batch. Declared identity stays unchanged;
  preparation eligibility requires measured N=1/C=3 FP16 output at each requested
  geometry; native activation still requires M16's real GPU binding proof.
  Catalogue core contracts have landed (`ModelRecord.verifiedReferenceProfile`,
  `spatialCandidateRefusal`, `spatialReferenceRefusal`). Parent CLI proof on
  original HD Balanced exits 0 at 64×64, retaining declared N=-1 and recording
  actual FP16 N=1/C=3 and 128×128 output; the other three HD graphs and SD also
  exit 0 at 96×64 with measured FP16 N=1/C=3 and 192×128 output. Catalog
  import/reload verification remains open under C13, and M16 GPU-binding proof
  remains unsatisfied.

<a id="c13"></a>
- [ ] **C13 — Repair and verify first-use owned inventory trust and backend-preserving reload/planning.**
  The rebuilt isolated CPU matrix passes, including fresh-root ownership,
  explicit backend support reload/re-import, declared HD metadata and actual
  reference proof. Model manifests use `mikai.native.model/2`; an empty provider
  list remains empty, never guessed. Real asynchronous panel imports, negative
  graphs, exact owned copies and lease-safe deletion pass. Its screenshot exposes
  stale missing-root diagnostics, tracked by C18.

<a id="c14"></a>
- [x] **C14 — Repair and verify bounded Processing viewport with visible Save/Cancel at minimum size.**
  Windows English settings and Ukrainian RIFE probes both retain exactly 560×600
  logical pixels at DPR 2; inspected PNGs show Save/Cancel outside the vertical
  viewport. Save/Cancel/reload, exact threshold and byte-identical
  credential-free `.env` preservation pass. C16/C17 separately track observed
  shortcut text and horizontal RIFE overflow; M38 is not complete.

<a id="c15"></a>
- [x] **C15 — Resolve observed native CPU regression failures and rerun the full isolated matrix.**
  `ctest --test-dir build-native-baseline -R mikai_tests --output-on-failure`
  passes in 25.48 s with actual owned ORT/model references enabled and no live
  API key. The fresh inspection-off `build-native-plain` matrix passes in
  14.55 s. Availability gates, relative artifact paths, exact numeric JSON
  boundaries, first-use inventory and explicit provider metadata are covered.
  Validation fixtures each violate one boundary; incidental pins and the
  undocumented duplicate-content bundle restriction are removed. LiveOracle
  remains a deliberate skip, not a pass; native GPU acceptance is unaffected.

<a id="c16"></a>
- [x] **C16 — Repair and verify fully visible shortcut bindings in styled minimum-size settings.**
  Verified against the current tree: `native_settings_probe` passes with the
  native field holding the longest binding, every character visible and the clear
  action inside the field at the styled minimum size, and the inspected
  screenshot shows the full `Ctrl+Shift+J` text. The width now derives from the
  styled input's own font metrics plus the clear action, never a text pixel
  constant. The four current geometry runs prove **all** shortcuts at the minimum
  size.

<a id="c17"></a>
- [ ] **C17 — Repair and verify Ukrainian RIFE controls without horizontal panning at minimum size.**
  The current `rife_settings_probe` **passes UK 0** on the real consumer
  (`.probe-out/native-current-rife-settings-consumer-uk.log`, exit `0`): exact
  `3/2` backend + both orders + combined base pre-resize persist Save/reopen,
  Cancel preserve, threshold exact factor only kept, invalid blank/out-of-range
  writes nothing, minimum `560x600` no horizontal with all RIFE controls/role
  labels inside and Save/Cancel visible. The earlier 1-FAIL was an incidental
  requested-default `820x940` equality (not control clipping) and was removed,
  not repinned; a small default-containment assert was added but is **not yet
  rebuilt**, so C17 stays **pending** only for that final rebuilt 0 run. The
  exact request id lives verbatim in a disclosure row.

<a id="c18"></a>
- [x] **C18 — Repair and verify honest lazy inventory and post-import diagnostics.**
  Verified against the current tree with the licensed ORT runtime named
  (`MIKAI_ORT_RUNTIME_DIR=.../runtimes/win-x64/native`): `native_assets_probe`
  passes 44/44, including a fresh owned store reading empty with no missing-root
  diagnostic, real fp16 import with byte-identical owned copy and distinct
  content identities for same-basename graphs, fp32/two-input graphs recorded as
  unsupported with the observed dtype/count reason and no inventory write,
  mutation refresh clearing the pre-import diagnostic, and populated screenshots
  showing models with a blank status. Ownership and corrupt-manifest refusals are
  untouched. The one-line read-path change makes the empty-root invariant local.

<a id="c19"></a>
- [x] **C19 — Resolve actual standard-player interaction failures and late probe access violation.**
  One real application defect, four probe defects and one harness defect, now
  separated and fixed. **App:** `PlayerDialog::openStream` parsed its `quality`
  label back into a number so every quality press stored `currentHeight_ = 0`;
  it now takes the height as an `int` and derives the label, and an unmatched
  press is logged/shown instead of a silent no-op. **Probe:** dub/subtitle
  pickers driven with kind-less variants; drag corner guessed from the pointer
  quadrant; parked-float read before `WA_DeleteOnClose`. **Harness:** Qt's
  `quitOnLastWindowClosed` ended `app.exec()` with 0 before the runner printed
  its verdict. `interaction_probe` passes 149/149 (`RESULT: PASS`, exit 0) on a
  rebuilt current tree with the session attached; an episode swap from a paused
  player restarts at 00:00 and stays paused (`pauseAfterLoad_` +
  `applyPendingPause()`), a quality switch carries its position and pause; three
  consecutive runs green.

<a id="c20"></a>
- [x] **C20 — Update and verify Compare probe against player-selectable preset contract.**
  Contract update landed in the probe source: the chooser asserts every leaf is
  an `availableModes()` preset, the off id resolves through `modeId(Mode::Off)`,
  the loose restore candidate is catalogue-selected, chain-internal files derive
  from the catalog, and category paths resolve through `categoryLabel()`. Pixel
  proof on the attached session against the 1080p fixture: `RESULT: PASS`, 30/30,
  noise floor 0.000 without re-seeking, chain effect 0.132/255 above the floor.
  Two consecutive current-tree runs each `RESULT: PASS`, 30/30
  (`.probe-out/cmp-oct2.log`, `.probe-out/cmp-oct2b.log`).

<a id="c21"></a>
- [ ] **C21 — Release and verify active and parked PiP pools with their owning dialog.**
  Source inspection found a real leak: the destructor deleted the active alias,
  not the parentless pool retained after restore. Teardown now clears aliases,
  hides both GL surfaces and releases the pool once. Observed in a real window
  against the current source: re-entering PiP reuses the parked float, the
  restore parks rather than destroys, closing the player with the float parked
  destroys it, and the Esc-ladder close destroys the float the first player had
  parked. The native half of this task still needs a real active native recipe.

<a id="c22"></a>
- [x] **C22 — Repair real file-picker proof and verify signed-HLS visible recovery.**
  Actual file-picker QA timed out and forcibly rejected the modal; its "accepted"
  flag recorded an attempt, not acceptance. The real chooser driver now requires
  chooser-closed plus a canonical path match, and a failed picker no longer
  blocks the later phases. `external_media_probe` passes every phase on a fresh
  loopback fixture with the session attached (`RESULT: PASS`, exit 0): picker
  accept with canonical path, local 320x180 colored pixels, standalone compare
  capture, HTTP 320x180 pixels, failed-URL retry chip with no host, retry
  recovery of the same URL, and file-URL reopen. Two defects fixed: the
  first-pump spurious `playingChanged(true)` hid the retry chip the 404 had just
  raised (`onEnginePlaying` now requires real video before dismissing a media
  error), and fixture `retryServed_` was process-global. HLS half passes 23/23
  with stale 403, visible failure, cache forget, fresh extraction and a real
  640x360 colored framebuffer with the retry cleared (inspected recovery PNG).

### 4.3 R no-regression acceptance (R1–R22)

<a id="catalog-r"></a>

Every row is a gate. "Existing probe" means a probe that exists today;
"proposed probe" names the target [M02](#m02)/[M51](#m51) adds. `plan.md` keeps the durable
verification strategy and links here for the row content.

| # | Behaviour that must not break | Edge case exercised | Verification |
|---|---|---|---|
| R1 | Qt/OpenGL playback with a stock core | offscreen Mesa vs real desktop; null framebuffer path | existing `widget_pixel_probe`, `mpv_surface_smoke`, `headless_pixel_probe` — rebuilt fresh; a stale binary is not evidence |
| R2 | `PlayerDialog` composition | hostless (direct media) vs episode session; 640×480 and 720×480 layouts | existing `dialog_probe`, `shell_probe`, `external_media_probe` |
| R3 | Anime4K preset identity and chain order | preset ids stable, `Off` resolves nothing, deleted shader → `chainIsComplete` false | existing `TestAnime4K` minus the retired wording/placeholder assertions ([M01](#m01)) |
| R4 | Anime4K menu interaction | category row opens beside, preset applies and closes, placeholder disabled | existing `anime4k_menu_probe` with the Back-row/wording assertions removed ([M01](#m01)) |
| R5 | Compare freeze and noise floor | paused frozen frame; two settled grabs; side-by-side, divide, difference; gain > 1 | existing `compare_probe`, `interaction_probe` — **must pass unchanged**, including the frozen-frame floor, which must not be weakened |
| R6 | Compare leaves the player as found | chain read before both sides, restored after, on every path including capture failure | `compare_probe` + a new assertion that the live chain id is unchanged after close |
| R7 | PiP semantics | one core plays; float owns its controls; volume/mute carry; position hand-back | existing `InteractionProbe` PiP cases |
| R8 | HTTP header requirement | `Accept-Language: uk-UA` present and comma-free | new hermetic `QTcpServer` fixture asserting the received headers ([M02](#m02), [M46](#m46)) |
| R9 | Signed-URL expiry recovery | 403/expired once, then success; quality cache `forget()` path | [M02](#m02) fixture + `TestQualityCache` |
| R10 | HLS master/adaptive | `ashdiMaster`, `moonMaster`, `tortugaMaster`, `tortugaDecode`, `parseVariants`, `qualityUrl` ladders | existing `TestStreams` + new local HLS fixture ([M02](#m02)) |
| R11 | Quality cache invariants | TTL 30 min, 240 entries oldest-first, `no_source` page lifetime | existing `TestQualityCache` — unchanged |
| R12 | Anime/media session transitions | title page → episode → player → next episode; refreshed streams; release tabs isolated | existing `title_page_probe`, `TestPagers`, `TestReleases` |
| R13 | Rate limiting | 429 parked on `Retry-After`, reset expiry, queued callers | existing `TestRateLimiter` — unchanged |
| R14 | Layering guards | domain has no Qt GUI headers, no app-layer names | existing `TestLayering` — unchanged, plus a new guard that `src/player/native/` stays QtCore-only |
| R15 | Language | explicit en/uk override, system mapping, catalog loading | existing `TestI18n` + new explicit catalog-load coverage ([M38](#m38)) |
| R16 | Settings persistence | API key, quota, feed toggles, preset shortcuts survive; native group is additive | existing `anime4k_menu_probe` settings survival + new native round-trip ([M34](#m34)) |
| R17 | Crash logging | a deliberate fault writes a real report | existing `crash_probe` |
| R18 | Failure separation | media failure → retry same URL / refresh stream; **processing** failure → retry processing, never a URL retry loop | new negative matrix ([M39](#m39), [M47](#m47)) |
| R19 | Startup robustness | missing runtime, missing native bundle, corrupt prefs | [M06](#m06), [M08](#m08), [M50](#m50) |
| R20 | Deployment | clean machine, no Python, no Conda, offline baseline | [M50](#m50) |
| R21 | GUI responsiveness under native | UI input latency sampled while a recipe is Active, during a first configure, during a preparation job holding the lease, and during a PiP park/unpark — no synchronous native call on the GUI thread and no `vf-metadata` query in a repaint | measured in the [M49](#m49) gate; async apply, async status and correlation per §7.8 |
| R22 | Native teardown ordering | pause → drain/suspend/cancel acknowledged **while core and render context are still alive** → detach callbacks → free the render context with GL current → terminate core; close-after-leave of the parked `pipPool_` float releases the native resources; no widget or worker destroyed from a native callback | [M30](#m30) plus the close probe in [M45](#m45) |

### 4.4 N native acceptance (N1–N10, N3b)

<a id="catalog-n"></a>

Native-specific non-regression rows. Each is proved by the `M` task named in
`plan.md`'s proof index; the row content is canonical here.

* **N1** CUDA inference reaches the actual Qt framebuffer: pixels captured from
  `grabFramebuffer()` after the native chain, with the decoded source and the
  post-native output both identified; a stock-core control run for comparison.
  Separately, the paused **Prepare** path must be exercised end to end. A real
  device UUID (never device 0) plus the geometry and CSP is **required** for a
  seekable fixture, or for a source that already has a valid cached frame; the
  output state after Prepare is `NeedsPreparation` or `Preparing`, never
  `Active` while still probing. A **non-seekable source with no frame at all**
  is the honest exception and must land on `Unknown` + **Original**, without
  unpausing and without guessing an identity — the same rule as §6.11. A real
  UUID is not promised unconditionally.
* **N2** Native-off and plain-player desktop regressions are freshly built and
  pass; the debug `INVALID_ENUM` observation is re-tested and reported as found
  or not found.
* **N3** RIFE timing, scene cuts, padding, seeks, EOF and rational output counts
  on real video, including A/V sync across a cut.
* **N3b** Source-change re-probe: when [M27](#m27) handles a context, device or
  cadence change, the plan is invalidated and the probe re-runs for the new epoch
  through the filter paths owned by [M19](#m19)/[M22](#m22), and a PiP handoff
  that changes those properties forces the same re-probe in [M45](#m45).
* **N4** Native PiP: 100 enter/leave cycles across two cores with resource
  counters for GPU memory and engine objects, plus Compare open/close inside the
  same cycle, and a close-after-leave path. A handoff that changes the reported
  device, context or cadence must force a re-probe for the new epoch before any
  activation.
* **N5** Local file, HTTP and HLS playback with native, plus anime session
  transitions (episode → next, refresh, release/episode context preserved).
* **N6** Preparation and artifact failure matrix: cold cache, corrupt artifact,
  cancelled build, build failure, missing model — each landing on Original with
  a specific visible reason and a working recovery.
* **N7** DirectML: where offered, provider identity and real GPU rendering are
  proved on pixels; where not, the row is disabled with the factual reason and
  no RIFE-on-DirectML claim is made.
* **N8** Sustained processing: warm-up, steady-state frame cadence, playback
  responsiveness while a recipe is active, memory high-water mark and leak
  checks — all measured, none invented. M49 additionally compares identical
  model digests, precision, recipe/factor, source/output geometry and hardware
  against standalone Janai/mpv, not merely Native-off. Decode/render/shader and
  presentation limits are matched or named. Prepared runs are sequential and
  GPU-exclusive. Full-frame transfers and CUDA↔OpenGL/scalar-RIFE synchronization
  stalls are traced and attributed. In-process placement is the chosen transport
  strategy, not evidence of absolute “maximum speed,” even after a bounded
  comparison. Missing measurements remain unmeasured, never estimated.
* **N9** Clean-machine install, upgrade and rollback with no Python and no Conda,
  plus offline baseline playback with native disabled.
* **N10** Fidelity and engine checks use **real frames and saturated frames**,
  never random noise; no bare "non-empty output" or "filter echoed the
  property" assertions anywhere.
