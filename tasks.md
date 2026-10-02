# mikai_janai — Native AnimeJaNai integration: implementation tasks

Companion to [`plan.md`](./plan.md). Every design decision, evidence row, hash,
limit and rejected alternative lives there; this file is the executable work
list. The two documents are one contract: a task that contradicts `plan.md` is a
defect in one of them, not a choice.

---

## 0. How to read this file

- **Every implementation checkbox remains unchecked until its acceptance is observed.**
  Individual statuses below record work in progress and exact blockers. The
  prototypes recorded in `plan.md` §"Runtime proof" were throwaway harnesses
  compiled **outside** this tree; they prove feasibility, not delivery, and
  they discharge **no** checkbox below.
- **`Depends Mxx`** lists genuine implementation prerequisites. The DAG in §1
  is acyclic and every task is reachable from a root. Pure contracts, planning,
  ownership logic and the Standard runtime path do not depend on permission
  to adopt the inference implementation or on a completed Native bundle.
  Where one task contains both independent implementation and a native
  integration gate, its status records those parts separately. An incomplete
  producer still blocks its actual runtime acceptance; no gate is waived.
- **PROPOSED** marks a file, directory, CMake target, CLI, settings group or
  probe that does **not** exist yet. Everything else in a *Files* list is an
  existing path in this repository, cited by its real symbol names. Reference
  trees outside the repo (`C:/Users/myhli/Downloads/janai/…`,
  `F:/ZKS/janai/…`) are **read-only** inputs, cited with line ranges.
- **Blocking prerequisites** are stated as facts. Where an SDK, licence
  clearance, model asset or hardware capability is missing, the task says so and
  names it. A missing prerequisite is a stop — never a silent pass, never a
  placeholder presented as a feature.
- Verification names the *actual* command. `ctest --test-dir build-cmake -R
  mikai_tests` and `build-cmake/<probe>.exe` exist today. Anything marked
  PROPOSED does not exist until its own task lands.
- Research status and proof limits are **not** task completion.

Stable anchors precede every heading so `plan.md`'s links cannot drift with
heading wording: `<a id="m01"></a>` … `<a id="m52"></a>` for the 52 tasks, plus
`<a id="dag"></a>`, `<a id="dag-interfaces"></a>`, `<a id="dag-waves"></a>` and
`<a id="dag-critical-path"></a>` for §1 and its subsections. `plan.md` links
`tasks.md#dag` as the single authoritative DAG and may point at those three deep
anchors; it does not restate the wave matrix or the critical path, because two
copies of the same table can only drift.

---

## 1. DAG: phases, waves and the critical path

<a id="dag"></a>

### 1.1 Shared interfaces (agreed once; every task builds against these names)

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

### 1.2 Wave matrix (generated from the `Depends` edges)

<a id="dag-waves"></a>

Each task sits in wave *n* when every task it depends on is in wave *n−1* or
earlier — that is, the wave number is the task's longest path from a root. What
this guarantees is one-directional: **no task in a row depends on another task in
that same row**, so the members of a wave have no prerequisite relationship and
may be started in any sequence. It does **not** claim that every pair of
independent tasks shares a wave — two tasks can be independent of each other and
still sit at different levels, because their depths depend on their own chains.
Wave membership is a scheduling aid, not a measure of whether one task needs
another.

Two hardware-bound exceptions exist by nature, not by dependency: the GPU proofs
(M43, M44, M45, M48, M49) contend for the same physical device, so they are run
**serially and exclusively** even though no `Depends` edge orders them against
one another. Treat exclusive-GPU access as an execution constraint of those tasks
rather than an edge in this table.

This table is generated from the `Depends` lines in §2; if you change a
dependency, regenerate it rather than editing it by hand. `plan.md` links this
table as the authoritative DAG and must not restate a different one.

| Wave | Tasks (independent once their prerequisites are complete) |
| --- | --- |
| 1 | M01, M02, M03, M06, M10 |
| 2 | M04, M07, M11, M12, M13, M14 |
| 3 | M05, M08, M09, M15 |
| 4 | M16 |
| 5 | M17 |
| 6 | M18 |
| 7 | M19 |
| 8 | M20 |
| 9 | M21 |
| 10 | M22 |
| 11 | M23 |
| 12 | M24, M25, M26 |
| 13 | M27, M33 |
| 14 | M28, M34, M37 |
| 15 | M29, M31, M35, M36, M39, M40, M46 |
| 16 | M30, M32, M38, M47 |
| 17 | M41 |
| 18 | M42 |
| 19 | M43 |
| 20 | M44, M48 |
| 21 | M45 |
| 22 | M49 |
| 23 | M50 |
| 24 | M51 |
| 25 | M52 |

Within a wave, the task IDs are listed in numeric order and that order carries
**no** meaning: the members are independent of each other and may be started in
any sequence or concurrently. Wave *n* means only that every prerequisite of
every member lies in wave *n−1* or earlier. There is no ordering and no arrow
between two tasks in the same row.

### 1.3 Critical path

<a id="dag-critical-path"></a>

The longest implementation chain is **25 tasks** after separating independent
contracts and Standard runtime work from Native bundle production:

```
M10 → M14 → M15 → M16 → M17 → M18 → M19 → M20 → M21 → M22 → M23
    → M26 → M33 → M34 → M36 → M38 → M41 → M42 → M43 → M44 → M45
    → M49 → M50 → M51 → M52
```

M15 still requires M04's real preparation helper, M11's inspected models and
M13's resolved geometry. The separate provenance/build/bundle branch is
`M03 → M04 → M05`; it gates Native artifacts, runtime proof and distribution,
not development of the Standard loader or pure recipe/manifest contracts.
All existing runtime, GPU, lifecycle and final-release acceptance stays intact.

Independent roots and runtime branches join that line:

- **M01** (baseline), **M02** (fixtures), **M06** (bundle contracts/install
  logic) and **M10** (typed recipe contracts) are independent roots.
  **M01 enters at M07**: the loader cutover must be measured against the
  observed baseline. **M02** feeds M46 and the desktop probes.
- Branches that leave and rejoin the line are real dependency edges, drawn here
  in words rather than as arrows: **M09 → M19** (ABI/capability protocol into
  the status surface), **M12 → M17** (RIFE capability into combined recipes),
  **M24** (RIFE-only and combined, beside M25/M26), **M28 → M29 → M30** (PiP and
  teardown), **M31 → M32 → M43** (Compare plan locking and Anime4K composition
  into the framebuffer proof), **M35/M39/M40 → M41**, **M46/M47/M48 → M49**,
  and **M12/M25** feeding M24/M48 respectively.

Three Native execution prerequisites remain:

1. **M04** — native-source builds require the inference grant and real SDKs.
   Pure contracts, Standard playback and owned-cache logic remain independent.
2. **M07** — the loader cutover touches every client, render and probe call
   site and must pass its Standard-path regression gates.
3. **M15/M16** — real preparation and binding/numerical inspection require a
   licensed helper and real artifacts; unit-level identities cannot satisfy
   those gates.

Off the critical path but **not** deferrable: **M12** depends on **M10** for
its typed capability surface and can proceed independently of M04/M05.
Asset acquisition/provenance can proceed before those types land. Missing
original RIFE assets still block M24/M44; neither model substitution nor a
fabricated Ready state is permitted.

---

## 2. Task list

<a id="m01"></a>

### M01 — Establish fresh independent baseline and retire obsolete incidental assertions

- [x] **Status:** implemented and verified. Incidental pins retired from the suite
  and the probe; the temporary-settings harness and the shader-deploy rules
  exist; the per-suite hermetic ledger is current (26 PASS + LiveOracle SKIP by
  design, `mikai_tests` exit 0). The attached-session rerun after the pin
  deletions is observed: `interaction_probe` reported `RESULT: PASS`, 149 ok /
  0 FAIL on three consecutive runs against the same rebuilt tree, and the
  shaders-deploy `POST_BUILD` step ran for both `mikai_tests` and
  `interaction_probe` in that build.
- **Depends:** none
- **Files (actual):** `tests/cpp/TestAnime4K.cpp`, `src/probes/InteractionProbe.cpp`
  (display-text matchers at ≈ 782-785 and 956-958, preset-count floor at ≈ 825;
  the Cyrillic in this file is probe-authored fixture data, not assertions),
  `src/probes/Anime4KMenuProbe.cpp` (planned-page two-row assertions
  ≈ lines 488–507), `tests/cpp/main.cpp`, `AGENTS.md`, `readme.md`.
- **Work:** Record the pre-change truth. (a) On a **freshly configured tree**,
  run `ctest --test-dir build-cmake -R mikai_tests` and record the per-suite
  PASS/FAIL lines verbatim into `plan.md`'s baseline table. The existing
  `build-cmake/widget_pixel_probe.exe` that produced the `plan.md` proof is a
  **stale binary** and is not evidence about current source — rebuild or say so.
  (b) **Delete** (never re-pin) assertions that test wording, object names or
  incidental defaults: the stale Ukrainian literals and row/`objectName` checks in
  `InteractionProbe` and `Anime4KMenuProbe`, and the placeholder-default
  assertions in `TestAnime4K`. Replace one only where a genuinely
  **consumer-visible** behaviour is at stake, and then assert it through the
  loaded catalog (explicit English/Ukrainian) — never against a source literal.
  (c) Correct `AGENTS.md`/`readme.md` where they describe deleted assertions.
- **Acceptance:** A clean configure yields `ctest … -R mikai_tests` exit 0 with
  no assertion that would fail on a reworded label, a renamed object or a
  changed incidental default. Every surviving assertion names an observable
  behaviour. **Boundary:** if deleting an assertion exposes a real regression,
  re-express it as a behaviour test — do not restore the old assertion.
- **Fixture deployment belongs to the baseline, not to a follow-up:** today the
  tree copies `shaders/` only as a `POST_BUILD` step on the `mikai` target, so
  a fresh build directory has no shaders beside `mikai_tests.exe` or the probes
  and `TestAnime4K` fails on `findShadersDir()` being empty. That is a **harness
  gap, not an application regression** — do not paper over it by weakening the
  assertion. Add a real `POST_BUILD` deploy for `mikai_tests` and for every probe
  that reads `shaders/`, so a from-scratch build can produce an honest baseline.
  With `MIKAI_LIVE` unset, `TestLiveOracle` **skips** by design; a skip is not a
  pass and must never be reported as one.
- **Verification:** `ctest --test-dir build-cmake -R mikai_tests` on a freshly
  configured build directory, plus `build-cmake/mikai_tests.exe TestAnime4K`.
  Record per-suite PASS/FAIL/SKIP lines verbatim and classify each failure as
  app defect versus harness gap before anything else lands.
- **Blockers:** none.

<a id="m02"></a>

### M02 — Add hermetic real-media and local HTTP/HLS fixtures

- [x] **Status:** implemented and verified. The generated media, the loopback
  server and the three real HLS renditions exist. On a fresh fixture instance
  (`--port 0`, `--require-accept-language`, media and HLS roots from the source
  tree) `external_media_probe` reported `RESULT: PASS`, 21 ok / 0 FAIL — real
  file-picker acceptance with a canonical path, local 320×180 colored pixels, a
  standalone Compare capture, HTTP 320×180 pixels, the failed-URL retry chip,
  retry recovery of the same URL, and file-URL reopen — and
  `hls_recovery_probe` reported `RESULT: PASS`, 23 ok / 0 FAIL: the expired
  signed variant fails visibly, the cache entry is forgotten, fresh extraction
  returns a valid signature, and a real 640×360 colored framebuffer renders with
  the retry cleared. No outbound network and no API key were used.
- **Depends:** none
- **Files (actual):** `src/probes/ExternalMediaProbe.cpp` (documents the existing
  `/retry.mp4` contract in `main`'s usage text), `src/probes/HlsRecoveryProbe.cpp`,
  `tests/fixtures/media/`, `tests/fixtures/hls/`, `tests/fixtures/http_fixture.cpp`
  → CMake target `mikai_http_fixture`, `tests/fixtures/generate_media.cmd`,
  `tests/fixtures/CMakeLists.txt`, `tests/fixtures/README.md`, and `readme.md` §Testing.
- **Work:** Every later proof needs media that is real — decodable, with real
  motion, real chroma, hard cuts — and needs no network and no API key.
  Produce: 320×180 and 1920×1080 clips; 8-bit and 10-bit HEVC; 23.976 and
  24 fps; one clip containing a hard cut, a saturated segment and a near-black
  segment; one non-multiple-of-32 height for the dyn-HW graph; one portrait clip.
  Add a `QTcpServer`-based fixture server (the project already links
  `Qt6::Network`) serving: a plain file, an adaptive HLS master plus variants, a
  `/retry.mp4` that 404s once and then serves the same bytes, and one variant
  whose signed query string has already expired. No CDN, no live key.
- **Acceptance:** `mikai_http_fixture` serves every case with no outbound
  network. `external_media_probe` passes against a fixture URL. The HLS master
  with an expired signed variant produces a **visible failure followed by a
  visible recovery**, never a silent success.
- **Verification:** `cmake --build build-native-baseline --target mikai_http_fixture`,
  then the existing
  `build-native-baseline/external_media_probe.exe <file> <http://127.0.0.1:PORT/media.mp4>`
  and `build-native-baseline/hls_recovery_probe.exe <http://127.0.0.1:PORT>`.
  Recorded: `RESULT: PASS`, 21/21 EXT lines in `.probe-out/ext42.log` and 23/23 HLS
  lines in `.probe-out/hls42.log`, each with zero FAIL lines. Re-verified after
  the lock-free fixture rebuild against a new fresh instance: `RESULT: PASS`,
  21/21 in `.probe-out/ext-final.log` and 23/23 in `.probe-out/hls-final.log`,
  zero FAIL lines each.
- **Blockers:** Fixture clips must be licence-clean — generate them or use assets
  already inside this repository. They are: generated by `tests/fixtures/generate_media.cmd`
  from ffmpeg's own lavfi sources, so no third-party footage is used.

<a id="m03"></a>

### M03 — Pin native sources, component provenance, and redistribution decisions

- [ ] **Status:** blocked by inference-source permission — manifests and checker implemented; checker executed, reporting 14 matching recorded file digests and exit 3 for recorded redistribution blocks. Private local non-commercial development is authorized, but it is not a copyright-holder grant. This blocks inference-source adoption and native-bundle/runtime delivery, not independent player contracts or Standard playback work.
- **Depends:** none
- **Files (actual, read-only references):**
  `C:/Users/myhli/Downloads/janai/animejanai-inference-main/CMakeLists.txt`,
  `.../include/aji.h`, `.../src/aji_conf.cpp`, `.../src/aji_trt.cpp`,
  `F:/ZKS/janai/mpv-upscale-2x_animejanai-v3.6.3/`.
  **PROPOSED:** `native/manifest/sources.json`, `native/manifest/components.json`,
  `native/manifest/THIRD_PARTY_NOTICES.md`, `native/README.md`.
- **Work:** Write the pin manifests. Record: the native mpv fork commit
  `63d127174d8f70336c82a22408929e2bc301edcb` with the exact upstream files this
  work touches (`video/filter/vf_animejanai.c`, `filters/filter.h`,
  `player/command.c`, `filters/f_lavfi.c`, `video/out/hwdec/hwdec_cuda_gl.c`,
  `video/out/hwdec/hwdec_cuda.c`, `meson.build`); the inference source identity;
  C ABI version 8 and every symbol in `include/aji.h`; `AJI_NVINFER` default
  `nvinfer_10`, with the build here explicitly set to `nvinfer_11` for the
  targeted **TensorRT 11.3** — TRT 10 is not a dependency, and the CUDA minor is
  **never** inferred from the TensorRT version; CUDA architectures
  `75-real 80-real 86-real 89-real 90-real 100-real 120-real 120-virtual`; and
  the observed package binaries with their SHA-256 (stock core, native fork
  core, `aji.dll`, `aji_trt.dll`). Then record, per component, the decision:
  vendored-and-rebuilt, redistributed-binary, or linked-externally — with its
  licence and whether redistribution inside a shipped bundle is cleared.
- **Acceptance:** `native/manifest/components.json` lists every binary that will
  exist in a shipped bundle with a source identity, a licence and an explicit
  `redistribution: cleared | blocked | external`. **Any component marked
  `blocked` makes M50 and M52 unachievable and must be escalated, not worked
  around.** The observed hashes are labelled descriptive research observations,
  explicitly not release pins.
- **Verification:** PROPOSED `mikai_manifest_check` (CMake/CTest entry) that
  recomputes each listed file's SHA-256 and fails on a mismatch.
- **Blockers:** **Legal clearance is a real blocker.** AnimeJaNai model weights
  are not confirmed redistributable; the TensorRT/CUDA redistributable closure
  is not cleared. Until M03 records a decision, no bundle ships.

<a id="m04"></a>

### M04 — Establish reproducible optional native SDK builds

- [ ] **Status:** blocked only on inference-source adoption/build permission. Official isolated Windows SDK, CUDA 13.4.92 and TensorRT 11.3 SDKs were acquired and configured; a genuine all-architecture MSVC/CUDA build and RTX 5090 kernel smoke passed. This proves SDK setup, not the native implementation build.
- **Depends:** M03
- **Files (actual, read-only):** `…/animejanai-inference-main/CMakeLists.txt`,
  `.../docs/BUILD-WINDOWS.md`, `.../src/harness.c`, `.../src/kernel_test.c`,
  `.../sanity/dml_spike.cpp`, `.../tools/convert_rife_fp16.py` (reference only —
  never invoked).
  **PROPOSED:** `native/CMakeLists.txt`, `native/patches/`,
  `native/build-msvc.md`, `native/tools/mikai_native_prep`.
- **Work:** Stand up a second, **optional** CMake project (MSVC + nvcc; C, CXX17,
  CUDA) beside the MinGW/Qt default build. The GUI stays MinGW/Qt6 and links
  only the C ABI. Vendor the pinned sources as an explicit patch set in
  `native/patches/` rather than editing the read-only trees in place. First
  patches: filter-local `GET_META` (see M19 for the exact route), a
  `preparedOnly` refusal, a strong content-addressed engine cache replacing
  `aji_trt.cpp`'s weak key and deleting `clean_stale_engines`' habit of removing
  foreign files from `model_dir`, and native plan suspend/resume. **No neural
  architecture is rewritten.** Add the explicit **preparation helper CLI** as its
  own target here; it is an out-of-band `QProcess` child and never appears in
  the decoder, render or frame path.
- **Acceptance:** Two consecutive clean builds produce byte-identical
  `aji.dll`/`aji_trt.dll` (or a recorded, explained difference). With
  `MIKAI_BUILD_NATIVE=OFF` the default MinGW/Qt build is bit-for-bit unaffected.
- **Verification:** PROPOSED
  `cmake -S native -B native/build -G "Visual Studio 17 2022" -DAJI_TRT_ROOT=<dir> -DAJI_NVINFER=nvinfer_11`
  followed by `cmake --build native/build --config Release`, plus the native
  `harness` and `kernel_test` targets.
- **Blockers:** **MSVC 19.44 is present** and is the toolchain this project
  builds against. What is **not found in the roots checked so far** is `nvcc`
  and the `NvInfer*` headers; that is a bounded search result, not a claim that
  the SDKs do not exist on this machine. `AJI_TRT_ROOT` must point at the
  **TensorRT 11.3** tree and the build must set `-DAJI_NVINFER=nvinfer_11` — the
  `nvinfer_10` default is not the target, and **no TensorRT 10 SDK is
  required**. Note also that a real prebuilt CUDA path has already been proven
  from core through the Qt GL bridge (`plan.md`), so the CUDA deliverable is
  exercisable ahead of this task; what this task adds is a *reproducible* native
  build plus the patches.

<a id="m05"></a>

### M05 — Assemble coherent versioned native runtime bundles

- [ ] **Status:** blocked — M03 rights gate and M04 reproducible native build outstanding.
- **Depends:** M03, M04
- **Files (actual):** `CMakeLists.txt` (`MPV_RUNTIME_DIR` block ≈ lines 38–45;
  `find_library(MPV_LIBRARY …)` ≈ 108–118), `readme.md` §Build.
  **PROPOSED:** `native/bundle/manifest.schema.json`, `native/bundle/stock.json`,
  `native/bundle/native.json`, `native/tools/bundle_tool`.
- **Work:** A bundle is **one coherent unit**: exactly one mpv `DLLMODULE`
  (standard **or** native, never mixed), its self-contained dependency closure,
  the native filter and inference DLLs, and `manifest.json` recording kind,
  module path, mpv API version, native protocol version, engine-schema version,
  per-file hashes and bundle version. The research harness needed
  `AddDllDirectory` into existing TensorRT/torch-runtime folders; production must
  **not** depend on Conda, torch, Python or any ambient install. Produce a
  **standard** bundle and a **native** bundle.
- **Acceptance:** `bundle_tool verify` passes only when every declared file is
  present, hash-matching and loadable by restricted-search `LoadLibraryEx`
  (see M07). A bundle whose dependency closure reaches outside its own directory
  is rejected. A bundle labelled `native` that cannot register `mikai_native`
  is rejected as incoherent.
- **Verification:** PROPOSED `bundle_tool verify native/bundle/native.json`.
- **Blockers:** M04's binaries. On the research machine stock and native were
  **two different core files**, so one filename cannot host both; the layout must
  name them explicitly.

<a id="m06"></a>

### M06 — Implement validated runtime discovery and atomic installation

- [ ] **Status:** implementation verified on the current tree — 45 bundle slots pass, including the G1 cross-kind refusal (a manifest-carrying Native dir beside the exe is refused as Standard by name), corrupt-behind-pointer, default-roots, kind-mismatch, unpublished-pointer, and single-hierarchy layout proofs. Standard discovery, corrupt/interrupted-install, boundary, immutability, self-containment and stock-tree refusal all pass on synthetic PEs with no native DLL. Only the real-Native-bundle acceptance still requires M05.
- **Depends:** none
- **Files (actual):** `src/backend/Runtime.cpp` / `Runtime.hpp` (`cacheDir()`,
  `findRuntime()`, `ensureRuntime()` — the existing **stock** discovery and
  download into `%LOCALAPPDATA%/MPVBackend/runtime`; **do not overwrite it**,
  it is a separate concern).
  **PROPOSED:** `src/player/native/NativeBundle.cpp` (`discover()`,
  `validate()`, `installAtomic()`), CMake target `mikai_bundle`.
- **Work:** Add bundle-aware discovery alongside the stock path. Search
  `QStandardPaths::AppLocalDataLocation` plus a path shipped beside the executable.
  **One consistent app-owned layout, used by every later task:**
  `AppLocalDataLocation/native/{runtime, models, artifacts, scratch, logs}` —
  installed bundles and their manifests under `runtime/`, imported and
  provisioned models under `models/`, engine artifacts and their manifests under
  `artifacts/`, transient preparation working space under `scratch/`, and native
  logs under `logs/`. `scratch/` is the only writable-during-preparation
  directory and is safe to clear; `models/` and `artifacts/` are never used as a
  writable cache for anything else. Validate each candidate against its manifest.
  Install **atomically**: extract into a temp directory on the same volume,
  verify, then flip a `current` pointer; an interrupted install leaves the
  previous bundle intact and usable. Never write into a user, shared or
  installed model directory.
- **Acceptance:** With no bundle present, discovery reports "none" with a
  factual reason and the app still runs. A corrupt bundle (truncated DLL, hash
  mismatch, missing dependency) is rejected **by name** and the previous good
  bundle is still selected. An install interrupted mid-copy leaves no
  half-installed bundle visible. **Boundary:** two bundles of the same kind, one
  invalid → the valid one wins silently and correctly.
- **Verification:** PROPOSED `mikai_bundle_selftest` (temp dir, corrupted copies,
  an interrupted install simulated by a pre-created marker), plus the existing
  `ctest --test-dir build-cmake -R mikai_tests` for the no-regression side.
- **Blockers:** M05 blocks discovery/installation proof with a real Native
  bundle, not implementation or Standard/corrupt/interrupted-install proofs.
  Bundle schema is a shared M05/M06 contract, not a dependency on native DLLs.

<a id="m07"></a>

### M07 — Cut over every client/render caller to the explicit MpvApi loader

- [ ] **Status:** Standard module path verified against the current tree; Native module path blocked on M05. `objdump -p` shows **zero** `libmpv` imports in `mikai.exe`, `mikai_tests.exe` and the playback probes, so no load-time import remains. `mpv_loader_probe <module>` passes on the current build: explicit `LoadLibraryExW` selection loads the pinned stock module (client API 131077 against header 131077), a live module swap is refused by name while the pinned module survives, async property/command/node replies correlate one-to-one, unknown properties report their error, and a refused load delivers its terminal end-file error event to the sink - the exact event the player turns into the retry chip. A real Qt frame rendered through the loaded module while the desktop session was attached (`widget_pixel_probe` 100% non-black, inspected PNG), and `interaction_probe` passes 149/149. The Native bundle half still needs M04/M05; interrupted worker edits are not accepted evidence.
- **Depends:** M01, M06
- **Files (actual):** `CMakeLists.txt` — the `MPV_RUNTIME_DIR` block,
  `find_library(MPV_LIBRARY …)`, `target_link_libraries(mikai_player PUBLIC
  ${MPV_LIBRARY})`; `src/player/MpvHandle.hpp` (`#include <mpv/client.h>`,
  `mpv_handle* get()/raw()`, `static int64_t apiVersion()`);
  `src/player/MpvHandle.cpp` (`mpv_create`, `mpv_set_option_string`,
  `mpv_initialize`, `mpv_command`, `mpv_get_property`, `mpv_set_property`,
  `mpv_wait_event`, `mpv_terminate_destroy` — note there is **no**
  `mpv_set_wakeup_callback` call site in `src/`; the header only mentions it in
  a comment, so do not go looking for one to convert);
  `src/player/MpvRenderContext.cpp` (`loadGlApi()`, `resolve()`,
  `mpv_render_context_create/_update/_render/_free/_set_update_callback`);
  `src/player/VideoSurface.cpp` (`ensureEngine()`);
  `src/player/PlayerDialog.cpp` (PiP creates a **second** core through a second
  `VideoSurface` — legal and required); and **every** target that transitively
  links libmpv today: `mpv_surface_smoke`, `headless_pixel_probe`,
  `widget_pixel_probe`, `shell_probe`, `dialog_probe`, `interaction_probe`,
  `compare_probe`, `compare_shot_probe`, `anime4k_menu_probe`, `resolver_probe`,
  `title_page_probe`, `external_media_probe`, `mikai_tests`, `mikai`.
  **PROPOSED:** `src/player/MpvApi.hpp/.cpp` — one small, boring loader module
  serving both the standard and native bundles, deliberately **not** named
  `MpvRuntime` and deliberately **not** nested under `native/`, because it is the
  mpv loader for the whole player rather than part of the native feature.
- **Work:** One process-wide `MpvApi` holds the selected `DLLMODULE`, loaded with
  `LoadLibraryExW` under `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` plus an explicit
  validated dependency directory — **no global `PATH` mutation**, no
  `QDir::addSearchPath` pretence. It resolves every needed entry point once and
  hands out a function table; the module is pinned for the life of the process,
  past all core handles and render contexts. **Many core handles are expected and
  legal** — PiP runs one per `VideoSurface` — and there is no hot module swap
  ever. Then convert, exhaustively: `MpvHandle` through the table,
  `MpvRenderContext` through `render_context_*`, `VideoSurface::ensureEngine()`
  obtains the module from `MpvApi`, `apiVersion()` reads the table. Finally
  **remove every load-time import**: delete the `MPV_LIBRARY` link, the
  `find_library` block, and the `MPV_RUNTIME_DIR` option, replacing it with a
  **headers-only** `MPV_INCLUDE_DIR`; update `readme.md`/`AGENTS.md` and every
  documented configure command. One mechanism, not a decorative loader beside
  static imports. **Do not treat `MPV_CPLUGIN_DYNAMIC_SYM` as an existing
  loader**: that header symbol is populated by an mpv *host* for C-plugin
  function pointers, not a general-purpose player DLL loader — reuse it as one
  would be a category error, so the loader here is written from scratch.
- **Acceptance:** `dumpbin /imports build-cmake/mikai.exe` (and every probe and
  `mikai_tests.exe`) shows **no** `libmpv-2.dll` import. The app opens a media
  file and renders a non-black frame using only the explicitly loaded module.
  Two cores coexist (main + PiP float) from **one** module. Changing the selected
  bundle persists and requires a restart, and says so.
- **Verification:** `dumpbin /imports`; `build-cmake/widget_pixel_probe.exe`;
  `build-cmake/mpv_surface_smoke.exe`; `ctest --test-dir build-cmake -R
  mikai_tests`; plus a PROPOSED `mikai_loader_probe` that runs two bundles in
  separate processes and asserts the refusal to swap a live module.
- **Async surface, in this same cutover rather than a later patch:** `MpvApi` and
  `MpvHandle` gain `mpv_command_async` and `mpv_get_property_async`, and the
  existing `observe()` primitive is made correct before anything reuses it — it
  asks `MPV_FORMAT_NODE_ARRAY` while its dispatcher handles only DOUBLE/FLAG/STRING,
  and nothing reads it today, so it is fixed rather than reused unchanged. The
  reason is concrete: the 50 ms `VideoSurface` state pump issues synchronous
  `getNumber`/`getFlag` calls that take the mpv core lock, and native
  first-configure, engine deserialization and GPU drain can hold that lock even
  when the work is not on the Qt thread. Native commands and status reads
  therefore go async; replies are correlated by (handle, source id, request
  epoch); UI state coalesces into the **existing** 50 ms bank — one bank, not
  parallel copies. No synchronous native query in `paintGL` or on any GUI-thread
  path. A node arriving on an **event** is owned by libmpv until the next
  `mpv_wait_event` and must not be freed by the client. A node the **app**
  manually allocates is freed with the **app's** allocator, not with
  `mpv_free_node_contents`; the free-node API is used only for
  libmpv-allocated owned results (a synchronous get, a command result, or an
  `mpv_event_to_node` conversion) through the selected API.
- **Acceptance (added):** a deliberately slow native operation — first configure
  or engine deserialize — does not stall the Qt event loop; the HUD keeps
  updating from the coalesced bank throughout it; an async reply for a superseded
  epoch is discarded, never applied; no event-owned node is freed by the client.
- **Verification (added):** PROPOSED `mikai_async_probe`, measuring event-loop lag
  across a slow native configure.
- **Blockers:** none.

<a id="m08"></a>

### M08 — Wire startup runtime selection, recovery, and restart preferences

- [ ] **Status:** Standard selection verified against the current tree; missing/corrupt-runtime recovery runs remain. `runtime_selection_probe` passes on the current build: Standard resolves, validates and loads; a requested native bundle that is not installed is refused with its named reason (never silently substituted); a different choice never hot-swaps the pinned module and reports that it needs a restart; the loaded selection carries the validated bundle descriptor, dependency closure and API version. No Native capability is claimed.
- **Depends:** M07
- **Files (actual):** `src/app/main.cpp` — `provisionMpvRuntime()` runs **after**
  `QApplication` construction and after the application name, organization name
  and stylesheet are set, not before (the file's own comment is stale on this
  point); `src/player/PlayerDialog.cpp:1916` and `:1942` (`emit
  runtimeRequested()` in `retryPlayback()`/`startPlayer()`);
  `src/app/pages/TitlePage.cpp:1593` — the handler currently only calls
  `session->dialog->show()` and does **not** provision; `src/backend/Runtime.cpp`.
  **PROPOSED:** selection state in `src/player/native/NativeBundle.cpp`.
- **Work:** Choose the bundle from preferences **after** `QApplication` and the
  application/organization names are configured (so `QStandardPaths` resolves)
  and **before** any `VideoSurface::ensureEngine()` can run. Record the selected
  bundle's kind, version, path, API and capabilities. Make `runtimeRequested`
  actually provision: the `TitlePage` handler must run the provisioning path and
  then call `startPlayer()` again instead of merely re-showing the dialog.
  A selection change persists and requires a restart, and the UI says so rather
  than pretending to hot-swap. Keep `provisionMpvRuntime()`'s stock behaviour
  intact for the standard bundle.
- **Acceptance:** Starting with the native bundle selected loads it; starting
  with the standard selected loads that. First play with no bundle produces one
  clear log line plus a provisioning attempt, then either recovers or reports a
  named failure — never a silent black player. A failed provisioning attempt
  leaves the app usable and retryable.
- **Verification:** `build-cmake/widget_pixel_probe.exe`,
  `build-cmake/dialog_probe.exe`, and a PROPOSED `runtime_selection_probe` that
  starts the app once per selection in a temp `QSettings` scope.
- **Blockers:** none.

<a id="m09"></a>

### M09 — Negotiate native ABI, protocol, and runtime capabilities

- [ ] **Status:** pending — Standard/no-filter unavailability can be exercised independently. Genuine Native ABI/protocol/provider negotiation requires a permitted, reproducibly built Native runtime; descriptor labels are not capability evidence.
- **Depends:** M07
- **Files (actual, read-only):** `.../animejanai-inference-main/include/aji.h`
  (C ABI 8, every `aji_*` export).
  **PROPOSED:** `src/player/native/NativeCapabilities.hpp/.cpp`,
  `native/include/aji_native.h`.
- **Work:** Define the **capability and probe protocol** here, so M19 and M22
  implement it and M13 consumes it later: `mpv_client_api_version()`; whether
  `vf-metadata/mikai_native` resolves under `MPV_FORMAT_NODE`; the native
  protocol and engine-schema versions; which inference backends are present.

  The protocol must cover an explicit **PROBE-ONLY PASSTHROUGH** state, because
  the actual decoder `CUcontext`'s **CUdevice UUID** and the
  **D3D11 adapter LUID** are *not* supplied by mpv's public `video-params` API, so
  the artifact build key cannot be computed from the property surface and
  **preparation must never guess device 0**. In that state the filter attaches as
  a pure pass-through and reports, from the filter thread: the real
  `AVHWFramesContext` device, its UUID or adapter LUID, and the decode geometry
  and CSP. It performs **no** engine configure, no deserialization, no build, no
  NN inference and no RIFE, and the status it publishes is `Probe`, **never**
  `Active` — `NativeOff` by default installs no filter at all, so a probe filter
  appears only after the user presses Prepare or Enable. Add one **additive** structured snapshot export on the native side
  with the protocol version negotiated explicitly and every consumer built
  against the same header. **The new export is a C export from `aji.dll`**, the
  inference backend — it is *not* added to libmpv and *not* routed through the
  Qt `MpvApi`; `MpvApi` stays a libmpv loader and nothing else. **No legacy log-tail parsing anywhere**: a version mismatch yields
  "native unavailable" with both versions as the reason, never a partial
  working state. Out-parameters alone are insufficient — the status must be
  structured, including for the 1:1 (no-scale) case where the geometry says
  nothing.
- **Acceptance:** A bundle whose core predates the native filter reports
  `NativeUnavailable(reason: "core has no mikai_native filter")` and the app
  plays normally with native off. A newer or older protocol reports both
  versions and does not half-work. The declared capability set explicitly
  includes `probe_passthrough`, `CUdeviceUUID` and `D3DAdapterLUID`, and an
  absent capability is reported as absent rather than approximated.
- **Verification:** PROPOSED `native_capability_probe` against both bundle
  kinds; `build-cmake/dialog_probe.exe` for the standard path.
- **Blockers:** M04 (the additive export exists only after the native patch).
  Note the split: this task **defines** the ABI, capability and probe protocol;
  M19 implements the filter-local status surface and M22 implements filter
  attachment and passthrough; M13 only *consumes* a validated snapshot and is
  unit-testable against fixtures beforehand, so it must not be read as runtime
  proof before M19/M22 land.

<a id="m10"></a>

### M10 — Define native model, recipe, preference, and availability contracts

- [x] **Status:** verified independent contracts — isolated inspector-on and fresh inspector-off matrices pass. Recipe round trips, exact temporal order/factor, unavailable model/backend reasons and no silent chain fallback are covered; real RIFE settings store/reload preserves exact requests. This does not claim Native execution.
- **Depends:** none
- **Files (actual, constraints honoured):** `src/player/Anime4K.hpp`
  (`ModeInfo::id` is the persistence key and the enum ordinal is never
  persisted — the identical rule applies to native models);
  `src/player/PlayerPopups.hpp` (`MenuEntry::disabled` is how a
  visible-but-unavailable row is expressed);
  `src/player/PresetShortcuts.hpp` (the one shared reserved-key list pattern
  that M36 extends).
  **PROPOSED:** `src/player/native/NativeTypes.hpp`, `NativeRecipe.hpp/.cpp`,
  `NativeCatalog.hpp` (types only here; behaviour lands in M11).
- **Work:** Define the typed vocabulary. `ModelRole {Spatial, Temporal}`;
  `Backend {TensorRT, DirectML}`; `Availability {Ready, Preparing,
  Unavailable}` with a mandatory factual reason when not Ready.
  `ModelRecord {id, label, bytesSha256, terms, dtype, role, backends,
  actualProfile}` — stable IDs, content-addressed, in a catalogue **separate from
  Anime4K**. `SpatialStage {modelId, preResize}` reflecting the native per-model
  pre-upscale resize from `aji_conf.cpp`'s `resizefactor`/`resizeheight`.
  `TemporalStage {version, full|lite|ensemble, scdetThreshold,
  order(BeforeSpatial|AfterSpatial), factorNum, factorDen}`.
  `Recipe {stages, backend}` in three supported shapes — spatial-only, RIFE-only,
  combined — with **no** identity-faking ONNX and no substitute model.
  **Stages do not carry prepared artefact ids.** A stage names a model; the
  *prepared* side is a join performed later, on the **build key** (the M14
  pre-build identity: model identity + build options + device/runtime), while
  **engineSha256** is the separate post-serialize product-integrity value M14
  records and M16 verifies. Conflating the two would let a stale engine look
  prepared.
  Model explicitly that "temporal only" is a different thing from "spatial
  configured to 0". Record the requested value, the effective value and the
  reason as three separate fields from the outset.
- **Acceptance:** A recipe round-trips through its serialised form unchanged.
  A recipe naming an unavailable model or backend yields `Unavailable` **with
  the reason** and never a silent fallback to another chain. Ordering is
  preserved exactly, and a `combined` recipe cannot be expressed with an
  ambiguous order.
- **Verification:** PROPOSED `tests/cpp/TestNativeRecipe.cpp` and
  `tests/cpp/TestNativeTypes.cpp` added to `mikai_tests`; run
  `ctest --test-dir build-cmake -R mikai_tests`.
- **Blockers:** none.
<a id="m11"></a>

### M11 — Implement compatible ONNX import and independent model inventory

- [ ] **Status:** inspection boundaries verified on the current tree — the full hermetic suite passes with fp32/bf16-unsupported, truncated/corrupt, zero/multi-IO, rank, batch, channel, and fixed-stage reference slots, and `native_inspect` proves all four original HD graphs (declared N=-1, real 64×64 → 128×128 N=1/C=3 run, scale 2) plus the original SD graph (declared N=1, 48×48 → 96×96 run, scale 2) with pinned digests. Desktop import acceptance remains open; no prepared engine or Active processing is claimed.
- **Depends:** M10
- **Files (actual, read-only):**
  `…/animejanai-inference-main/src/aji_dml.cpp:1156-1179`
  (`SessionGetInputCount`/`SessionGetOutputCount`/`SessionGetInputName`/
  `SessionGetInputTypeInfo`/`GetTensorElementType`), and the package's
  `F:/ZKS/janai/mpv-upscale-2x_animejanai-v3.6.3/animejanai/onnx/` (five
  spatial ONNX models, verified present).
  **PROPOSED:** `src/player/native/NativeCatalog.cpp` and a C++
  `NativeModelInspect` helper reusing the already-present ONNX Runtime C API for
  declared metadata plus a CPU reference run.
- **Work:** Import a user-supplied `.onnx` into the native catalogue and validate
  it by **actual engine inspection** — never by filename prefix and never through
  an architecture whitelist. Check the exact input count and output count (a
  generic spatial model is 1×3 in / 1×3 out; refuse anything else, naming the
  actual counts and a fix, rather than partially accepting it), the declared
  dtypes, and the **real graph geometry**: 1:1 or a supported scale, with the
  factor learned from the actual engine rather than read off a `2x_`/`4x_`
  filename.

  **Eligibility boundary.** The original TensorRT and ORT staging path is **FP16
  NCHW**, and the bridge expects FP16 I/O. So FP16 NCHW graphs are the eligible
  set — that is what "FP16-compatible" means here, not a hint to re-export
  something as FP32. **FP32 and BF16 graphs are recorded in the inventory as
  unsupported with an explicit reason** (no FP32 binding or cast bridge is
  implemented), not silently converted, not re-exported, not auto-promoted. If a
  provider bridge capability for them is ever implemented, it must be **real and
  proved** before those graphs become eligible. Stable content IDs stop two
  different models sharing a basename from colliding. No Python anywhere in this
  path.

  **Verified shape (C12, owner-approved).** Both tensors require **rank 4,
  FP16, C=3, NCHW**; the declared input batch must be **N=1**. A declared
  output batch of **N=1** follows the fixed-boundary path. A **symbolic output
  batch** stays symbolic in the declared metadata and is eligible only after a
  **real CPU reference run at the requested geometry** proves actual output
  **N=1/C=3** and the dtype/geometry constraints. This is preparation eligibility,
  not Native activation: **M16 still requires verified GPU engine bindings**.
  An explicit output N≠1, invalid channel/rank/dtype, or unproved symbolic
  boundary is never clamped or promoted. Scale must be a
  **uniform positive integer** (identity / 1× included). Dynamic H/W is
  resolved from the **actual stage geometry**, not from a `dyn-HW` filename
  token or any special form; a file whose name happens to contain `dyn-HW` gets
  no special treatment.
- **Acceptance:** Importing
  `2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo.onnx`
  succeeds and reports a **learned** factor. A model with two inputs is rejected
  with the actual counts in the message. Two same-named files in different
  folders yield two distinct IDs. A truncated file is rejected **without**
  writing anything into the inventory. An FP32 and a BF16 graph are **recorded
  in the inventory as unsupported with an explicit reason**, with no converter
  run and no re-export. A rank-5 or C≠3 graph, or a non-integer / non-uniform
  scale, is refused naming the actual declared shape. A dynamic-HW graph resolves
  its geometry from the stage it runs at, and the same file at a different
  resolution yields a different resolved geometry without a different identity.
- **Verification:** PROPOSED `tests/cpp/TestNativeCatalog.cpp` (CPU-only, over
  the M02 fixtures) and a PROPOSED `native_inspect` CLI run across
  `animejanai/onnx/*.onnx`.
- **Blockers:** A CPU reference run requires an independently acquired, pinned
  ONNX Runtime dependency with its real license and closure. It does not
  require the inference shim or TensorRT. M05 still gates use of that
  dependency inside the final Native runtime bundle; it does not gate
  standalone inspection/import development with the licensed ORT package.
  `sanity/dml_spike.cpp` is read-only API evidence, never a runtime dependency.

<a id="m12"></a>

### M12 — Provision original Janai RIFE model variants and capabilities

- [ ] **Status:** provenance blocks execution — the four exact FP16 variant assets were acquired, hashed and matched against VideoJaNai 2.1.0. RIFE-only/combined typed request controls, exact threshold editing and durable full-recipe persistence landed; integrated desktop verification remains. The converted-graph-to-original-weight lineage hop remains unresolved; no substitute model, execution, Ready capability or temporal acceptance is claimed.
- **Depends:** M10
- **Files (actual, read-only):** `…/src/aji_trt.cpp:932-946` and
  `…/src/aji_dml.cpp:1311-1323` (`rife_model_name()`: code `414` →
  `rife_v4.14`; a four-digit code ending in `1` → `_lite`; `+_ensemble` when
  ensembling); `…/src/aji_conf.cpp:171-180` (RIFE keys; `rife_before_upscale`
  default **yes**; `rife_scene_detect_threshold` default `0.150`);
  `…/src/aji_conf.h:23-28`. Verified in the package:
  `F:/ZKS/janai/mpv-upscale-2x_animejanai-v3.6.3/animejanai/` ships the five
  spatial ONNX files and **no** `rife_v*.onnx`.
  **PROPOSED:** `src/player/native/NativeRife.cpp`.
- **Work:** Build the RIFE capability surface **in full scope**: v4.14 full,
  v4.14 lite, ensemble, rational `factorNum`/`factorDen`, the SCDetect threshold,
  and the ordering choice (interpolate-then-upscale vs
  upscale-then-interpolate, i.e. `rife_before_upscale` yes/no) together with the
  per-model pre-resize that RIFE-first ordering implies (`aji_pre_resize`). The
  user's recipe is honoured exactly: no silent substitution of another chain or
  model, no silent skip of RIFE. This turns the informational
  "Planned: RIFE is not implemented yet" leaf at `src/player/Anime4K.cpp:263-265`
  into a real capability subtree; RealESRGAN and "other models" stay
  informational placeholders, because they still are.
- **Acceptance:** Each variant, factor, threshold and ordering is selectable and
  round-trips. A variant whose assets are absent reports `Unavailable` with the
  missing-asset reason and the recipe fails loudly — readiness is *the entire
  selected recipe, or a visible Original plus a failure*.
- **Verification:** PROPOSED `tests/cpp/TestNativeRife.cpp` (capability table,
  code→name mapping, ordering constraints); runtime acceptance in M24/M44.
- **Blockers:** **HARD BLOCKER, verified in the checked package paths:** the
  reference package's `animejanai/onnx/` holds the five spatial ONNX files and no
  `rife_v*.onnx`. This is a statement about the package that was scanned, not a
  global claim about every location on this machine. Obtaining the original RIFE
  weights is part of this task's manifest acquisition, so M24 and M44 are gated
  on it, not on anything the app can work around. **Note on ordering:** this task
  depends on **M10**, so it cannot run parallel to M04/M05 — the capability
  surface needs M10's typed vocabulary. What is not blocked by that ordering is
  the *asset question*: the missing-weights finding above is known now and should
  be raised now, rather than surfacing at M24.

<a id="m13"></a>

### M13 — Resolve source-aware preparation plans and geometry

- [ ] **Status:** resolver verified on the current tree — 51 prep-plan slots pass, including rotation-to-portrait, portrait/crop temporal padding, non-zero-origin crop, full-range, 10-bit/23.976/portrait interaction, odd-dimension asymmetric pad, two-stage chain threading, and 320x180 SD planning with a real window refusal. Actual inspected-profile adapter execution and live native source/device reports remain; they require M19/M22 and must never be guessed.
- **Depends:** M10
- **Files (actual, read-only):** `…/src/aji_conf.cpp:105-205` — native
  pixel-count and fps windows, per-model resize factor/height, and the
  missing-model **skip** behaviour; `…/src/aji_conf.cpp:226-287` — built-in
  slots `1001/1002/1003` plus benchmarks `1010-1013`, which are **reference
  templates, not the source of model identity**.
  **PROPOSED:** `src/player/native/NativePreparationPlan.cpp`.
- **Work:** Given an actual source (w, h, fps, CSP, matrix/range/siting, decoder
  device), produce a `PreparationPlan`: which stages apply, each stage's
  resolved input/output geometry, the resulting source→output geometry, and any
  blockers. **The source geometry is learned from the real source and the real
  engine — not assumed before the first core exists.** Until it is known,
  `NativeModelIdentity::sourceGeometry` and `outputGeometry` are an **optional
  `QSize` / unknown**, not a fabricated 1×1 placeholder; once resolved they hold
  the actual stage geometry. A plan computed from the
  user's guess of their video is wrong, so the app stays `Preparing` until real
  geometry is known and only then re-resolves. Handle orientation, crop and CSP
  exactly as the native reference behaviour does. Do **not** adopt the native
  "missing model → skip" behaviour for a user-selected recipe: a requested
  recipe is all-or-nothing.
- **Acceptance:** A 1080p23.976 10-bit BT.1886 source yields a plan whose
  output geometry derives from the real inspected graph, not from a filename. A
  source outside every chain window yields a blocked plan with a named reason,
  not a fallback chain. Two sources with identical geometry share one prepared
  artifact set. **Boundary:** a portrait or cropped source produces a plan whose
  geometry matches the reference behaviour for that orientation.
- **Verification:** PROPOSED `tests/cpp/TestNativePrepPlan.cpp` over the M02
  fixture matrix (8-bit/10-bit, 23.976/24, portrait, cropped, odd height).
- **Blockers:** M11's inspector. **This is pure planning and is unit-testable
  against fixture inputs before any producer exists** — that is not runtime
  proof. The live variant consumes a *validated* M19/M22 snapshot and must not
  be described as proven until M19 and M22 land.

<a id="m14"></a>

### M14 — Implement content-addressed engine manifests and ownership boundaries

- [ ] **Status:** identities and ownership verified on the current tree — the full hermetic suite passes with declared-profile/build-key agreement, manifest/engine-name collision refusal, explicit-root confinement with a model-directory sentinel, read-side redaction, manifest reparse checks, and the hostile manifest/store/runtime/LUID slot matrix. Complete prepare/activate/cleanup acceptance still needs real preparation and runtime producers.
- **Depends:** M10
- **Files (actual, read-only):** `…/src/aji_trt.cpp:203-222` (`crc32_z`);
  `:419-441` (`trt_engine_path_for`: basename + CRC32(settings) + engine suffix +
  GPU name/SM — **not** a content hash and **not** the GPU UUID);
  `:436-438` (`clean_stale_engines` removing foreign engine files out of
  `model_dir`). **PROPOSED:** `src/player/native/NativeManifest.cpp`.
- **Work:** **Two distinct identities, never conflated.**
  `GraphContentIdentity` = role + graph content hash + the graph's *actual*
  declared I/O — device-independent, and the same for the same graph anywhere.
  `EngineBuildIdentity` = the TensorRT/ORT build identity — GPU UUID + SM,
  precision/TF32 flags, plugin, runtime, core version, source ABI,
  CUDA/driver version and build options — plus `engineBytesSha256` over the
  produced engine file. A graph can be valid and eligible while its build
  identity is not yet producible on this machine; the two must be reportable
  separately. Store
  manifests beside the artifacts in an **app-owned** cache directory. Never use
  a user, shared or installed model directory as a writable cache. Never
  deserialize an engine before its manifest validates. Detect an in-place source
  file change and invalidate. **Two keys, not one:** the *pre-build* key (model
  identity + build options + device/runtime) decides what to compile and where to
  look; the *post-serialize* `engineBytesSha256` is recorded afterwards and
  validates the produced artifact. The second is not derivable from the first
  and must not be substituted for it.
- **Acceptance:** The same graph imported twice yields **one**
  `GraphContentIdentity` and two `EngineBuildIdentity` values when the two
  machines differ. Two models with the same basename in different folders get
  different identities. A changed driver version invalidates. A truncated engine
  file invalidates. A probe drops a sentinel file into a model directory and
  asserts it survives a full prepare/activate/cleanup cycle byte-for-byte.
- **Verification:** PROPOSED `tests/cpp/TestNativeManifest.cpp`.
- **Blockers:** none — pure logic, so it may proceed alongside M13's
  integration work.

<a id="m15"></a>

### M15 — Implement explicit cancellable native engine preparation

- [ ] **Status:** blocked — Real preparation requires M04 licensed helper and SDK, plus M11 inspected graphs and M13 resolved plans; independently implemented cache contracts do not prove engine builds.
- **Depends:** M04, M11, M13, M14
- **Files (actual, read-only):** `…/include/aji.h` (`trtexec`, `trtexec_env`,
  `async_build`, `aji_poll`); `…/src/aji_trt.cpp` (the build path).
  **PROPOSED:** `src/player/native/NativePreparation.cpp`, driven through
  `QProcess` against M04's `native/tools/mikai_native_prep`.
- **Work:** Preparation is an **explicit user action**, runnable while the
  original plays. The controller keeps the picture on `Original` and the state at
  `Preparing` — never `Active`. `Ready` requires **all** assets, all engines, the
  resolved per-stage geometry and all capabilities; anything short is a visible
  `Preparing`. Cancellation is real: terminate the child and discard the partial
  artifact rather than leaving something that merely looks loadable. One
  process-wide lease serialises GPU build against GPU inference so preparation
  never contends with playback. **No `trtexec` invocation, background build or
  log tail ever appears in a frame, configure, render or paint path** — the
  preparation process and the playback process are different things and must not
  be conflated.
- **Acceptance:** Prepare starts visible progress; cancel stops it and leaves no
  usable-looking partial artifact; a failed build lands in `Failed(reason)`, keeps
  Original visible and marks nothing Ready; a second Prepare press while one is
  running does not start a second build; preparation while the video plays does
  not stutter or drop frames (quantified in M49).
- **Verification:** PROPOSED `native_prep_probe` against a fixture recipe —
  cancel, failure and success paths.
- **Blockers:** M04's CLI and a licensed engine builder (TensorRT SDK).

<a id="m16"></a>

### M16 — Inspect built engines and prove their bindings and numerical behavior

- [ ] **Status:** blocked — Requires M15 real produced engines and CUDA numerical/binding verification; no inference-source grant yet.
- **Depends:** M15
- **Files (actual, read-only):** `…/include/aji.h` (C ABI 8 plane/byte-pitch
  contract; NV12/P010/YUV444P16; caller-supplied `CUcontext`/`CUstream`; ticket
  `aji_flush`/`aji_done`/`aji_wait`; `AJI_SCENE`);
  `…/src/aji_trt.cpp` (TensorRT bindings, FP16 staging, enqueue and readiness);
  [the pinned fork's `hwdec_cuda_gl.c`](https://github.com/the-database/mpv/blob/63d127174d8f70336c82a22408929e2bc301edcb/video/out/hwdec/hwdec_cuda_gl.c)
  (CUDA/OpenGL registration, device-to-array copies and synchronization);
  `…/src/aji_dml.cpp` (D3D11 texture planes).
  **PROPOSED:** `native/tools/mikai_native_verify`.
- **Work:** After a build, inspect the produced engine and **prove** rather than
  assume: every input and output binding's count, dtype and shape matches the
  plan (**validate all bindings, not just the last one**); the geometry factor is
  the one the plan expects; and a CPU-reference numerical comparison on a real
  frame. Record the data path honestly in the results: the CUDA↔Qt OpenGL interop
  performs device copies and synchronisation, and the RIFE scene metric is a
  **D2H scalar copy plus a CPU stream sync per frame pair**. Neither is a defect,
  and neither may be written up as "no device transfers". A full-frame CPU
  conversion/readback is prohibited in the steady-state GPU path; a
  user-requested frame capture is allowed and separate.
- **Acceptance:** A correct engine passes with matching bindings, dtypes and
  geometry. A mismatched or stale engine fails loudly, naming the actual
  bindings. A corrupted engine never deserializes.
- **Verification:** PROPOSED
  `mikai_native_verify --engine <path> --onnx <path> --fixture <frame>` over the
  M02 fixtures; the GPU portions run on the CUDA machine only.
- **Blockers:** M15 producing at least one real engine; a CUDA machine.

<a id="m17"></a>

### M17 — Prepare complete ordered spatial and temporal recipes

- [ ] **Status:** blocked — Requires actual original RIFE assets and real fully verified spatial/temporal prepared engines.
- **Depends:** M12, M15, M16
- **Files (PROPOSED):** `src/player/native/NativePreparation.cpp` (composite
  planning), `NativeRecipe.cpp`.
- **Work:** Take a `Recipe` — spatial-only, RIFE-only, or combined — and drive
  it to a fully prepared, manifest-validated, readiness-published state. Apply
  stages in order: per-stage pre-resize in chain order; RIFE before or after
  spatial exactly as the recipe says; RIFE's own working resolution when a
  pre-resize applies. The curated Quality/Balanced/Performance entries are
  honest reference recipes assembled from the shipped HD/SD Balanced/Performance
  and Sharp1 ONNX models — **not** the native slot numbers `1001/1002/1003`.
- **Acceptance:** A combined RIFE-first recipe with per-model pre-resize prepares
  a chain whose every stage geometry matches the plan, published all-or-nothing.
  A recipe whose second stage cannot be prepared ends in `Failed` naming that
  stage, keeps Original visible, and activates **no** partial chain.
- **Verification:** PROPOSED `native_prep_probe --spatial-only`, `--rife-only`,
  `--combined`.
- **Blockers:** M12's missing RIFE assets for the RIFE-only and combined
  variants; spatial-only is unblocked once M15/M16 land.

<a id="m18"></a>

### M18 — Replace native weak-cache and playback-triggered build paths

- [ ] **Status:** blocked — Patching inference cache/build paths requires permission to adopt the pinned inference sources.
- **Depends:** M09, M17
- **Files (actual, read-only):** `…/src/aji_trt.cpp:419-441, 436-438` (weak key;
  stale-engine deletion), `…/include/aji.h` (`async_build`, `trtexec`,
  `aji_poll` — the first-play build path), `src/player/VideoSurface.cpp`
  (`ensureEngine()`, the only place a core is created today).
  **PROPOSED:** `native/patches/`, plus the filter-construction side in
  `src/player/MpvApi.cpp` (M07's loader module) and
  `src/player/native/NativeController.cpp`.
- **Work:** On the native side: replace the weak cache key with M14's identity;
  make `clean_stale_engines` never touch anything it does not own; refuse to
  build during playback (`preparedOnly`); retire the configure-time build and
  `trtexec` from every playback path. On the client side: the native filter is
  inserted only with a manifest-validated artifact set, or not at all — an
  absent filter is an honest state meaning "no native processing", never a
  silent passthrough presented as active.
- **Acceptance:** Starting playback with a missing engine produces a named
  `PreparingRequired` state — not a minutes-long freeze and not a silent
  passthrough. Playing with a prepared engine writes nothing to the cache. A
  process monitor watching a full session sees **no** `trtexec` child. The
  user's model directory is byte-identical before and after.
- **Verification:** PROPOSED `native_cache_probe` (empty cache + playback ⇒ no
  writes, no child processes).
- **Blockers:** M17.

<a id="m19"></a>

### M19 — Expose authoritative filter-local native metadata snapshots

- [ ] **Status:** blocked — Producing native filter-local snapshots requires licensed native-source adoption and M04/M18 actual patched builds.
- **Depends:** M09, M18
- **Files (actual, read-only):** mpv `filters/filter.h:374-397`
  (`MP_FILTER_COMMAND_GET_META`), `player/command.c:1427-1483`
  (`vf-metadata/<label>` → tag property → `talloc_free`),
  `filters/f_lavfi.c:815-835` (`mp_tags_dup`). **This route already exists and is
  verified — it is not an open question.** **PROPOSED:** `native/patches/`
  (filter-local `GET_META` owned by our own `vf`),
  `src/player/native/NativeStatus.cpp`.
- **Work:** Serve a structured snapshot under label **`mikai_native`**, read via
  `vf-metadata/mikai_native` with `MPV_FORMAT_NODE`. Contents: protocol version,
  schema version, monotonic revision, and a typed state document carrying
  **requested plan, effective plan**, engine identities in use, per-stage
  geometry, decoder device, cadence counters and the last error.

  **Probe-only passthrough state** is part of this contract, because the build
  key needs a device identity that `video-params` does not supply: in
  PROBE-ONLY PASSTHROUGH the filter reports the actual `AVHWFramesContext`
  device UUID (CUDA) or adapter LUID (D3D11) plus decode geometry and CSP, and
  the status is `Probe` — **never** `Active`, never a partially built engine.
  Any context/device/cadence change invalidates the plan and triggers a fresh
  probe for the new epoch.

  Two layers, deliberately: the **flat `mp_tags` layer** carries the scalars a
  caller can read without parsing — `protocol`, `schema`, `revision`, and the
  typed status — and the **snapshot itself is one valid JSON string** in a single
  tag. Tags are strings, so the flat layer must state its own typing explicitly
  rather than letting a reader guess, and the JSON must always parse: a malformed
  snapshot is a named error, never a silently accepted partial document.

  **Node ownership is exact** (`client.h` ≈760–762, 1394–1420, 1622–1655): the
  `data` of an async `mpv_event_property` is **borrowed, library-owned** until
  the next `mpv_wait_event` or handle destruction. So decode the node and deep-copy
  the typed `NativeStatus` `QString` values **synchronously, inside the dispatch
  that receives the event**, and never call `mpv_free_node_contents` on a borrowed
  async node. The free-node API is used only for nodes libmpv allocated and handed
  us as owned — a synchronous `mpv_get_property` result, a command result, or the
  contents produced by `mpv_event_to_node` — always through the **selected**
  `MpvApi`'s function table under RAII, never a raw `talloc_free`. A copy the
  **app** allocated is freed with the **app's** allocator, not with
  `mpv_free_node_contents`. **Do not assume mpv generates property-change
  notifications** for this route.
- **Acceptance:** Reading `vf-metadata/mikai_native` returns a well-formed
  document whose revision strictly increases on each real change and is
  unchanged on a re-read with nothing changed. Reading it on a core without the
  filter returns the property error — not a crash, not an empty success. The
  snapshot distinguishes requested from effective even for a 1:1 spatial chain,
  where geometry alone would say nothing.
- **Verification:** PROPOSED `native_status_probe` reading the node directly via
  `MpvApi` and asserting the typed fields, including a case where requested and
  effective differ.
- **Blockers:** M09.

<a id="m20"></a>

### M20 — Consume versioned native status through the selected MpvApi

- [ ] **Status:** consumer slice verified on the current tree — the full hermetic suite passes including detach-clears-view, named-refusal halt, preparing-availability, and non-Active plan refusal slots. `native_status_probe` still reports the absent filter as `Unavailable`/`native.status.missing` with a bounded poll, and the chip renders the reader's own refusal with the still-running plan beside the reason. Authoritative Native snapshots remain blocked on M19; no `Switching` enumerator was added.
- **Depends:** M07, M19
- **Files (actual):** `src/player/MpvHandle.cpp` (`observe`/`drainEvents`,
  `setEventSink`), `src/player/VideoSurface.cpp` (`pumpEvents()` and the existing
  50 ms state pump), `src/player/PlayerDialog.cpp` (`connectEngine()`).
- **Work:** Poll the metadata property on a **bounded 500 ms timer**, and only
  while native is `Active`, `Preparing` or `Switching`. Never inside `paintGL`,
  never per frame. Skip work when the revision is unchanged — polling is the
  mechanism, because notifications are not assumed. Parse into
  `StatusSnapshot {version, revision, typed state}` and emit a Qt signal only on a
  genuine change. Unsubscribe on core teardown. **The poll request itself is
  asynchronous** (M07's `mpv_get_property_async`): the 500 ms tick fires a
  request tagged with the current epoch and does not wait for the reply, so a
  core busy with native configure or drain cannot stall the Qt thread through
  this path. A late reply for a superseded epoch is dropped. Ownership follows the
  rule from M19 exactly: an async `mpv_event_property` node is borrowed and is
  never freed; only a **synchronous** get yields an owned node for
  `mpv_free_node_contents`, and only an app-allocated copy is freed by the app.
- **Acceptance:** The status a viewer sees is the filter's own report, not an
  echo of what was requested — so a plan that failed to activate shows the old
  effective plan plus the reason. With the filter absent, status is
  `Unavailable` with a reason. Polling does not measurably change frame pacing,
  and it does not measurably block the GUI thread (quantified in M49).
- **Verification:** PROPOSED `native_status_probe`; `build-cmake/dialog_probe.exe`
  for the no-native path.
- **Blockers:** M19.

<a id="m21"></a>

### M21 — Implement acknowledged filter epochs, drain, suspend, and resume

- [ ] **Status:** blocked — Real drain/suspend/resume acknowledgement requires M19 native producer and filter implementation; pure transport does not satisfy native acceptance.
- **Depends:** M19, M20
- **Files (actual):** `src/player/VideoSurface.cpp` — `setAnime4KChain()` and
  `setAnime4KMode()` call `render_->update()` because `vo=libmpv` rebuilds the
  filter chain only when a frame renders; that discipline carries over to native
  plan changes. `src/player/PlayerDialog.cpp:1293-1335` (`openStream`,
  `restoreResumePoint`). **PROPOSED:** `native/patches/`,
  `src/player/native/NativeController.cpp`.
- **Work:** Plan changes are transactional and **acknowledged**. One controller
  per `PlayerDialog`/session owns requested vs effective state plus a
  process-wide native work lease. Every request carries a revision; completions
  from obsolete revisions are dropped. The sequence: pause → drain pending
  inference tickets → set the named filter plan → apply a decoder-compatibility
  override if required → re-anchor the clock keeping the same PTS → **wait for the
  ack** → then restore play/pause, audio, subs and speed. Requesting a choice
  never changes the model until the ack arrives, and the HUD stays truthful while
  it waits.
- **Acceptance:** A plan change mid-playback does not jump the position, does not
  mute, does not change speed, and preserves the audio and subtitle selection.
  Two rapid changes leave exactly the second one effective. A failed change
  leaves the previous effective plan intact and reports why.
- **Verification:** PROPOSED `native_epoch_probe` (position/track/speed
  continuity across a switch, plus an obsolete-completion drop).
- **Blockers:** M19.

<a id="m22"></a>

### M22 — Integrate named native filters with decoder and source compatibility

- [ ] **Status:** blocked — Actual native filter attachment/probe pass requires M19/M21 native producer and coherent module.
- **Depends:** M21
- **Files (actual):** `src/player/VideoSurface.cpp` — `ensureEngine()`'s option
  table (`vo=libmpv`, `hwdec=auto-safe`, `keepaspect`, the comma-free
  `http-header-fields`, the `user-agent`, `input-default-bindings=no`,
  `input-vo-keyboard=no`, `input-cursor=no`, `osc=no`);
  `src/player/MpvRenderContext.cpp`. **PROPOSED:** filter construction in
  `src/player/MpvApi.cpp` (M07's loader) and
  `src/player/native/NativeController.cpp`.
- **Work:** Insert the named filter with correct mpv escaping —
  `@mikai_native:animejanai=key=value:...` — with `%`-length quoting computed
  over the **UTF-8 byte count**, or better, build an mpv **command node** instead
  of interpolating a shell string. The filter has **two modes**: PROBE-ONLY
  PASSTHROUGH (attached only after the user presses Prepare/Enable, passes
  Original frames unchanged, performs no engine configure / deserialization /
  build / NN / RIFE, and reports the real device UUID or adapter LUID plus
  decode geometry and CSP from the filter thread), and READY NN (attached only
  after every stage is prepared). Playback and the UI stay on Original in probe
  and preparation states. **No CUDA calls or hwctx pointers cross the Qt
  thread**; preparation helpers build on the GPU the filter reported. If playback
  is paused and no frame is available, the source probe may request one frame at
  the existing PTS through the existing async source coordinator while preserving
  pause; if the source is not seekable it waits for the next real frame or
  reports `Unknown` — it never guesses a device identity. No shell string interpolation anywhere; a
  Windows drive colon, a comma and a percent are the three escaping edge cases.
  The composition is fixed: `decode → native vf (RIFE in the recipe's order) → mpv
  colorscale → Anime4K shader hook stages`. Decoder compatibility: native codec
  decode may require NVDEC and must use the **actual** CUDA context/device the
  decoder reports, never global device 0. Preserve the HTTP headers and every
  existing mpv/vo option verbatim. A software-decoded source may need one GPU
  upload — label that honestly, do **not** claim NVDEC for a CPU decode, and do
  not put a GPU→CPU→GPU round trip into the steady-state inference path.
- **Acceptance:** PROBE-ONLY PASSTHROUGH attaches on demand, passes frames
  unchanged, publishes `Probe` (never `Active`), and yields a real device UUID or
  adapter LUID that a subsequent build key is computed from — preparation never
  assumes device 0. Paused-with-no-frame either yields one frame at the existing
  PTS without unpausing, or reports `Unknown` on a non-seekable source. Any
  context/device change re-probes for the new epoch. With the native bundle and
  a prepared spatial recipe, the chain mpv reports contains `mikai_native` in
  the right position relative to `colorscale` and the Anime4K hook stages. Sources whose paths contain a drive
  colon, a comma or a percent all open correctly. `hwdec` and the HTTP headers
  survive. A software-decoded source is labelled as such in the status.
- **Verification:** PROPOSED `native_filter_probe` (chain listing via `MpvApi`;
  path-escaping matrix over the M02 fixtures).
- **Blockers:** M05's native bundle.

<a id="m23"></a>

### M23 — Run generic spatial-only native model chains

- [ ] **Status:** blocked — Real spatial processing requires licensed native inference bundle plus verified prepared engines.
- **Depends:** M22
- **Files (actual, read-only):**
  `F:/ZKS/janai/mpv-upscale-2x_animejanai-v3.6.3/animejanai/onnx/` — the five
  shipped spatial models, including
  `2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo.onnx`
  and the four HD Balanced/Performance/Sharp1 2× models.
  **PROPOSED:** runtime wiring under `src/player/native/`.
- **Work:** Run an ordered spatial chain end to end: multi-stage 2× chains and the
  1×3 dynamic-HW compact, with per-stage pre-resize, confirming the output
  geometry is the learned one. This is the **mandatory full CUDA deliverable**
  and must work for any generic-compatible imported model, not only the shipped
  five.
- **Acceptance:** A 1080p source through a two-stage 2× chain reaches the Qt
  framebuffer at 3840×2160 with visibly correct output. The dyn-HW model handles
  a non-multiple-of-32 source without a wrong-size crash or a silently wrong
  scale. A deliberately broken stage fails loudly.
- **Verification:** PROPOSED `native_spatial_probe` — real frames including the
  saturated segments from M02, with non-black pixel statistics plus visual
  inspection of saved PNGs. Never a random-noise "fidelity" claim.
- **Blockers:** a CUDA machine with a supported driver (the research machine had
  an RTX 5090 on driver 616.92, OpenGL 4.6).

<a id="m24"></a>

### M24 — Run original RIFE-only and combined native recipes

- [ ] **Status:** blocked — Real original RIFE/combined processing requires licensed inference runtime, exact proven RIFE assets and fully prepared engines.
- **Depends:** M23, M17
- **Files (actual, read-only):** `…/src/aji_conf.cpp:171-180`,
  `…/src/aji_trt.cpp:932-946`, `…/include/aji.h` (`aji_infer_rife`,
  `aji_pre_resize`, `aji_rife_before_upscale`, `AJI_SCENE` semantics).
- **Work:** Run RIFE alone, RIFE-before-spatial and spatial-before-RIFE, each
  with its rational factor, SCDetect threshold, full/lite/ensemble variant and
  optional pre-resize. A scene change returns `AJI_SCENE` and must emit a
  duplicate of frame `a`, as the reference pipeline does. Record in the results
  that scene detection costs a D2H scalar copy and a CPU stream sync per pair —
  a real property of the design, not something to hide.
- **Acceptance:** Output frame rate is the rational factor applied to the source
  rate, and the timeline stays rational — no drift and no rounding to a fixed
  multiplier. At a hard cut there is no blended ghost frame. Frame counts and PTS
  are monotonic across a seek and across EOF.
- **Verification:** PROPOSED `native_rife_probe` over the M02 cut/EOF fixtures;
  deep runtime proof in M44.
- **Blockers:** **M12's missing `rife_v*.onnx` assets.** This task is full scope
  — not deferred, shrunk, or turned back into a placeholder. It is blocked on
  assets only.

<a id="m25"></a>

### M25 — Integrate and capability-gate the native DirectML spatial path

- [ ] **Status:** blocked — DirectML execution requires licensed inference-source adoption, own ORT/DirectML closure and actual D3D11/WGL proof; unavailable reason remains factual.
- **Depends:** M23
- **Files (actual, read-only):** `…/src/aji_dml.cpp`, `…/sanity/dml_spike.cpp`,
  `…/include/aji.h` (DirectML `aji_frame` contract: `plane[0]` =
  `ID3D11Texture2D*`, `plane[1]` = subresource index, textures created `SHARED`
  + `SHARED_NTHANDLE`, `d3d11_device` required); package
  `animejanai/inference/aji_dml.dll` and `DirectML.dll`.
  **PROPOSED:** `src/player/native/NativeDirectML.cpp`.
- **Work:** DirectML is a **conditional capability track**, not a promise.
  Integrate backend selection into the recipe model and gate the offering on a
  **real** provider plus Qt GPU-interop proof. Explicitly: the CUDA↔OpenGL proof
  does **not** license a claim that WGL can import D3D11 — that must be
  demonstrated for DirectML on a DirectML-capable machine, or the backend stays
  disabled with a factual reason. **No DirectML RIFE**: if the DirectML backend
  cannot do temporal work, say so and offer spatial-only there. No Vulkan
  requirement, and no hidden fallback to CUDA behind a DirectML label.
- **Acceptance:** On a machine without the required D3D11/Qt interop, DirectML
  rows are visible, disabled, and carry a reason naming what is missing. On a
  machine with it, a spatial chain runs and the status names the provider and the
  device actually used. Nothing anywhere claims DirectML RIFE.
- **Verification:** PROPOSED `dml_capability_probe` (records provider and device
  identity and whether the interop actually imported); deep proof in M48.
- **Blockers:** **The open question is the D3D11 → Qt WGL interop, not the
  hardware.** The package ships `aji_dml.dll` and `DirectML.dll` alongside the
  ONNX Runtime runtime, and the machine is D3D-capable, but the CUDA↔GL proof
  says nothing about importing D3D11 textures into a WGL surface — that remains
  unproven. Until it is, this track ships disabled-with-reason: the honest
  state, not a scope reduction.

<a id="m26"></a>

### M26 — Implement transactional requested-to-effective recipe activation

- [ ] **Status:** blocked — Activation acceptance requires post-output native epoch snapshots; command success or pure state logic is insufficient.
- **Depends:** M22, M23
- **Files (actual):** `src/player/PlayerDialog.cpp:1909-1944` (`retryPlayback()`,
  `startPlayer()`), `src/player/VideoSurface.cpp`. **PROPOSED:**
  `src/player/native/NativeController.cpp`.
- **Work:** The controller exposes `requestRecipe()` — recording the
  **requested** value, persisting the tick even when the choice is unavailable,
  and publishing `effective` plus a reason as separate fields — and drives the
  M21 transaction. Native Off is the default and must mean **no `vf` inserted at
  all**; toggling on/off live bypasses slot 0 rather than building an
  unnecessary chain, and a deactivated plan is reported as deactivated, never as
  active-with-nothing. **Admission is a distinct state**: Native Off installs no
  filter; pressing Prepare/Enable admits a probe filter (M22) to obtain the
  device identity and geometry; only once every stage in the selected recipe is
  prepared does the controller switch transactionally to READY NN. Turning
  Native Off removes the probe filter **after its ack**, never speculatively. **Apply is asynchronous**: the plan is applied with
  `mpv_command_async` tagged by (handle, source id, epoch), so the GUI thread
  never blocks on the core lock while native configures, deserializes or drains.
  A command reply being accepted is **not** the same as the native plan being
  active — activation completes only once M19's `GET_META` reports the new epoch
  and the effective plan is published.
- **Acceptance:** Selecting an unavailable model leaves the request persisted and
  the effective plan unchanged, with the reason visible. Turning Native off
  mid-playback removes the filter and leaves position, pause, volume and speed
  intact. Turning it on mid-playback activates without a reload. A command reply
  arriving before the new epoch is published does not flip the UI to Active, and
  the GUI stays responsive throughout the apply. The Probe → Prepared → Active
  sequence is visible and ordered: Probe never reports Active, Prepared never
  activates, and Native Off removes the probe filter only after the removal is
  acknowledged.
- **Verification:** PROPOSED `native_activation_probe`.
- **Blockers:** M21, M22.

<a id="m27"></a>

### M27 — Handle source, geometry, cadence, and discontinuity changes

- [ ] **Status:** blocked — Native source transitions require an actually active verified recipe and native snapshots.
- **Depends:** M26
- **Files (actual):** `src/player/PlayerDialog.cpp` — `applyEpisode`,
  `applyVariant`, `applyRefreshedStreams`, `goToEpisode`, `openStream`,
  `restoreResumePoint`; `src/player/VideoSurface.cpp` (`videoSizeChanged`).
  **PROPOSED:** `src/player/native/NativeController.cpp`.
- **Work:** A new episode, a quality switch, a dub/sub variant and an orientation
  change all invalidate the current plan's geometry. On each: cancel stale source
  jobs, re-derive the plan from the new source, and either keep, re-prepare, or
  fall back to Original with a reason.

  **Reconfiguration is not only about geometry.** A change in the decoder
  **CUcontext / CUdevice UUID**, the **D3D11 adapter LUID**, or the cadence —
  exactly the identities M19 reports and M22 obtains in PROBE-ONLY PASSTHROUGH —
  invalidates the **active plan** and triggers a **fresh probe for the new
  epoch**. A plan must never be carried across a device change on the assumption
  that the device is the same.

  **Invalidating the active plan is not the same as invalidating the persistent
  artifact.** Re-probing produces a newly resolved set of build inputs; those are
  compared against M14's build key, and if **every** newly resolved input matches
  — same GPU UUID/SM, same shapes, same runtime/precision/build options — the
  already-built, already-verified serialized engine is **reused**. A runtime
  context or cadence change does not by itself force a rebuild, and this task must
  not force one: an avoidable engine build on a mere re-probe is a defect, not
  caution. What is *never* shared is a live execution context or a captured CUDA
  graph: reuse means reloading a verified engine into the new context, not
  carrying the old context or graph across.

  Cache reuse is keyed on **source geometry, not URL identity** — the same
  episode at 480p and 1080p are different plans. A discontinuity (seek, episode
  cut, stream recovery) must not leave a half-applied epoch.
- **Acceptance:** Switching 1080p → 480p mid-episode re-derives the plan; if the
  new plan is not prepared, playback continues on Original with a visible reason
  rather than stalling or silently mis-scaling. A device/context/adapter-LUID or
  cadence change invalidates the active plan and re-probes per epoch (M19/M22),
  and the probe reports the new identity before any preparation is attempted.
  When the re-probe resolves build inputs that all match M14's build key, the
  verified serialized engine is reused with **no** rebuild; a rebuild happens only
  when a build input actually differs, and a rebuild on an unchanged input set is
  a failure of this task, not a safety margin. A seek during activation does not
  leave the player on a stale epoch. Returning to a previously prepared geometry
  reuses the artifact without rebuilding.
- **Verification:** PROPOSED `native_source_change_probe` over the M02 fixture
  matrix and the quality ladder.
- **Blockers:** M26.

<a id="m28"></a>

### M28 — Preserve playback state across native activation and media replacement

- [ ] **Status:** blocked — Native activation/media replacement state proof requires M26/M27 real producer; plain-player regressions are independently reachable.
- **Depends:** M27
- **Files (actual):** `src/player/PlayerDialog.cpp` — `openStream`,
  `restoreResumePoint`, `applyEpisode`, `applyVariant`, `applyRefreshedStreams`,
  and the resume fields `resumePending_`/`resumePosition_`/`resumePaused_`;
  `src/player/VideoSurface.cpp` (`close`, `setPaused`, `setSpeed`, `setVolume`,
  `setMuted`). **PROPOSED:** controller resume helpers.
- **Work:** The existing resume machinery already handles position, pause, volume
  and speed across a stream swap; extend it so a **native activation** and a
  **media replacement** land on the same path. Mute, speed, volume, subtitle and
  audio-track selection must all survive. Do not disturb `keepaspect` or the
  volume-100 option. A resume after a media replacement must wait for the new
  duration, exactly as `restoreResumePoint()` already does.
- **Acceptance:** Activating native mid-playback keeps position within one
  frame, keeps mute/speed/volume, and keeps the selected tracks. Replacing the
  episode preserves the resume point and the paused state. A speed of 2× is not
  silently reset to 1×.
- **Verification:** PROPOSED `native_resume_probe` measuring position delta,
  speed, mute and track selection across both paths.
- **Blockers:** M27.

<a id="m29"></a>

### M29 — Preserve native processing through pooled PiP handoffs

- [ ] **Status:** blocked — Native PiP handoff proof requires real native recipe, parked-resource acknowledgement and one-module loader; plain PiP is independent.
- **Depends:** M28
- **Files (actual):** `src/player/PlayerDialog.cpp` — `togglePip` (≈2194),
  which dispatches to the existing `enterPip` (≈2208) / `leavePip` (≈2298);
  `syncPipBar` (≈2350); and the `pipPool_` member with its comment.
  `enterPip` **exists and works today** — this task extends it, it does not
  add it; `PipFloat` (`openStream` ≈446,
  `closeStream` ≈455, `setPlaying`, `setVolume`, `setMuted`, `position`,
  `duration`); `src/player/VideoSurface.cpp`.
  **PROPOSED:** per-surface native state in `NativeController.cpp`.
- **Work:** Both surfaces run the **same effective `Recipe`** and the same
  Anime4K chain — no PiP bypass, no second "safe" plan. There is one `MpvApi`
  module and **two core handles**, each with its own native epoch, CUDA context
  and output pool; no execution context is shared across cores or threads. One
  surface plays and infers at a time: entering PiP suspends and drains the parked
  core's filter, releasing TRT intermediates while keeping the GL widget and the
  last displayed frame alive; leaving restores the hand-back of float position,
  pause, mute, volume, speed, fullscreen state **and the actual native plan**.
  Do not reuse the historical claim that two live `QOpenGLWidget`s must never
  coexist as a licence to bypass native in PiP — the pool exists for a documented
  driver reason, not to dodge the feature.
- **Acceptance:** Entering PiP shows the same native processing on the float;
  the parked core is suspended (proved by resource/handle observation, not by
  assertion), and no second inference lease is taken while the float plays.
  Leaving PiP resumes at the float's position with mute, volume, speed,
  fullscreen and the native plan intact. A core whose plan failed to prepare
  shows Original on **both** surfaces with the same reason — consistency, not a
  silent divergence.
- **Verification:** PROPOSED `native_pip_probe` comparing frame statistics on
  the float and the main surface for the same source; deep stress in M45.
- **Blockers:** M26.

<a id="m30"></a>

### M30 — Release parked native resources on close and ordered shutdown

- [ ] **Status:** blocked — Native GPU release/ordered drain acceptance requires real native producer; plain teardown regressions remain independent.
- **Depends:** M29
- **Files (actual):** `src/player/PlayerDialog.cpp` — `~PlayerDialog` (≈1021,
  which deletes `pip_` but **not** `pipPool_`), `closeEvent` (≈2172, which calls
  `leavePip(true)` and then closes the main surface);
  `src/player/VideoSurface.cpp` (`~VideoSurface`, which resets the render context
  before the handle so the GL context is still current).
  **PROPOSED:** ordered release helpers in `NativeController.cpp`.
- **Work:** Closing must stop the parked float's stream, cancel its native jobs,
  drain its tickets, detach callbacks and release its native resources — the
  source risk that the hidden `pipPool_` never gets `closeStream()` called is
  handled **now**, at the source-risk level, without claiming a reproduced
  leak. Widget-pool lifetime stays owned by application shutdown ordering rather
  than hot deletion while the main surface is visible. **Teardown order is
  fixed**, and each step waits for the previous one: **pause → drain / suspend /
  cancel ACK while both the core and the render context are still alive → detach
  callbacks → free the GL render context with its context current → terminate the
  core.** That ACK is asynchronous — no Qt-side GPU wait and no unsafe
  destruction of a Qt worker that is mid-flight. A fatal driver fault or an
  in-process crash cannot be promised to be recoverable; what is claimed is an
  ordered, bounded release path. Repeated open/close cycles must not accumulate
  engine, core or CUDA resources. This is a source-risk fix, and the probe — not
  the reading — is what proves it.
- **Acceptance:** 100 open/close cycles with PiP entered and left each time show
  flat engine, core and CUDA memory after the first cycle and no growth trend.
  After close, no native job, ticket or callback fires. Shutdown exits cleanly
  with the same shape. Close completes only after the drain/suspend ack and never
  hangs waiting on a GPU.
- **Verification:** PROPOSED `native_lifetime_probe` (100 cycles, sampled GPU
  and handle counts).
- **Blockers:** M29.

<a id="m31"></a>

### M31 — Preserve exact-frame shader Compare with native plan locking

- [ ] **Status:** blocked — Native Compare plan-lock acceptance requires real native plan; existing shader Compare can be independently proven.
- **Depends:** M28
- **Files (actual):** `src/player/CompareWindow.cpp` — `captureFrame()`,
  `grabSettledFrame(double*)`, `renderWithChain(const QString&)`, `populateCandidates()`,
  `showEvent`, `frozenFrame()`, `captureError()`; `src/player/CompareCanvas.cpp`;
  `src/player/PlayerDialog.cpp` — `openCompareWindow` (≈1393, which already
  refuses to open while `pip_ != nullptr`), `copyCurrentFrame` (≈1422).
  **PROPOSED:** plan lock in `NativeController.cpp`.
- **Work:** Shader Compare keeps its existing meaning over a **fixed native
  processed frame**. Freeze the native epoch and take a native-plan lock for the
  whole capture-and-repaint sequence; restore the currently applied shader on
  **every** path including cancel and failure. Native Off → the base is the
  unprocessed frame; Native On → the base is clearly labelled as Janai output
  with the current recipe and RIFE state. Native changes are queued until the
  window closes or the capture is cancelled, with a clear reason. **No native
  model swap, no reseek and no native A/B slot** is used as the A/B mechanism —
  the A/B axis remains the shader catalogue, whose ids are unchanged. Never infer
  same-frame parity across different models or temporal states. The existing
  paused/frozen Compare noise-floor semantics must **pass**; do not disable or
  re-pin those tests to hide a failure.
- **Acceptance:** With native active, both sides of Compare render the same
  frozen source frame with the same native epoch; the base label names the native
  output and the recipe. Closing Compare leaves the previously applied shader and
  the active native plan exactly as they were. A native request made while
  Compare is open is queued, and applying it afterwards is visible.
- **Verification:** PROPOSED `native_compare_probe`, plus the existing
  `build-cmake/compare_probe.exe` and `build-cmake/compare_shot_probe.exe` for the
  shader-side no-regression guarantee and the noise floor.
- **Blockers:** M26.

<a id="m32"></a>

### M32 — Prove native and Anime4K shader composition without changing shader identities

- [ ] **Status:** blocked — Native+Anime4K composition acceptance requires real spatial output; unchanged shader identities remain independently testable.
- **Depends:** M31, M23
- **Files (actual):** `src/player/Anime4K.cpp` / `Anime4K.hpp` —
  `allCandidates()`, `filesForId()`, `shaderStringForCandidate()`,
  `chainIsComplete()`, `ModeInfo::id`; `shaders/` (49 GLSL assets);
  `src/player/VideoSurface.cpp` (`ensureEngine()` writes `glsl-shaders`);
  `tests/cpp/TestAnime4K.cpp`; `src/probes/InteractionProbe.cpp` (the control
  pair / picture-difference measurement). **PROPOSED:** nothing new required —
  this task is measurement and evidence.
- **Work:** Prove the fixed composition `decode → native vf → mpv colorscale →
  Anime4K shader hook stages` behaves, without changing a single shader
  identity, catalogue id, chain order or file. **No mandatory renderer
  rewrite** and no change to the existing `QOpenGLWidget` + render-API bridge.
  Where a gate depends on the ratio of the actual post-native image to the
  canvas, measure that ratio rather than assuming every Anime4K upscale "does
  nothing" once native is on. Active NN processing may legitimately change which
  Anime4K stage has anything left to do — determine that by measurement and say
  so, rather than disabling stages or reordering the catalogue. Preserve the
  existing control-pair methodology: freeze one timestamp, take an untouched
  control pair, then measure.
- **Acceptance:** Every Anime4K catalogue id and chain still resolves and stays
  byte-identical (`TestAnime4K` passes unchanged). With native off, Anime4K's
  measured effect is unchanged from the pre-change baseline. With native on, the
  composed output is measured and reported, including any stage that no longer
  has an effect and **why**. A shader error under native processing is surfaced
  as an Anime4K error, not swallowed.
- **Verification:** existing `ctest --test-dir build-cmake -R mikai_tests`;
  `build-cmake/anime4k_menu_probe.exe`; `build-cmake/interaction_probe.exe`;
  `build-cmake/compare_probe.exe`; plus the PROPOSED `native_composition_probe`
  for the native-on measurements.
- **Blockers:** M23.

<a id="m33"></a>

### M33 — Surface native choices in the existing enhancement tree

- [ ] **Status:** Standard desktop menu smoke passes — real clicks open the existing sparkle tree's native branch; unavailable choices stay disabled with tooltips, stored request/current marks survive reopen, and Off clears the request without invoking media recovery. PNG captured and visually inspected: opaque existing-tree panel, truthful disabled native choices and visible Off/current mark. Re-verified on the current tree (`native_menu_probe`: `OK`, menu-scoped recovery delta holds). Native-on menu acceptance still requires M26; no seventh button or effective activation is claimed.
- **Depends:** M20, M26
- **Files (actual):** `src/player/PlayerShell.cpp` — `openEnhancements()` (≈1096),
  `enhancementEntries(const QString& levelId, const QString& currentId)` (≈43),
  `showEnhancementLevel`, `onEnhancementRow`, `enhancementPanels_`,
  `enhancementLevels_`; `src/player/Anime4K.cpp` — the catalogue and the
  planned-model leaves at `:263-272`; `src/player/PlayerPopups.hpp` —
  `MenuEntry {heading, text, tooltip, payload, current, submenu, disabled}`.
  **PROPOSED:** native menu-row construction in
  `src/player/native/NativeMenu.cpp`.
- **Work:** Put the native branch inside the **existing** enhancement tree and
  the existing sparkle/dock button — **no seventh button, no new top-level
  window**. The RIFE placeholder leaf at `Anime4K.cpp:263-265` becomes a real
  capability subtree with the current RIFE rows and a live capability status
  each; RealESRGAN and "other models" stay disabled placeholders. Rows are
  `MenuEntry::disabled` when unavailable and always carry a tooltip naming the
  reason. Preserve the tree's existing behaviour: opening always resets to the
  root, submenu rows leave the popover open, `hasChildren` drives whether a
  drill-down is offered, and the marked-current row uses a **stable id**, never
  a menu index or enum ordinal.
- **Acceptance:** The enhancement root shows a native branch with a working
  submenu. Unavailable providers/models appear, are visibly disabled, and their
  tooltip states why. The current selection is marked by stable id and survives a
  menu reorder. Opening the tree always lands on the root. Existing Anime4K rows
  are unchanged.
- **Verification:** PROPOSED `native_menu_probe`; the existing
  `build-cmake/anime4k_menu_probe.exe` and `build-cmake/interaction_probe.exe`
  for the surrounding tree.
- **Blockers:** M20, M26.
<a id="m34"></a>

### M34 — Add compact processing settings and versioned preference persistence

- [ ] **Status:** implemented, dialog verification in progress — additive versioned preferences, Processing settings, RIFE-only/combined requests, exact threshold editing and future-schema preservation are implemented. The probe's Qt button API misuse is repaired but not yet rebuilt. Real Save/Cancel/reload and environment-file preservation checks remain open.
- **Depends:** M33
- **Files (actual):** `src/app/dialogs/SettingsDialog.cpp` / `.hpp` —
  `buildGeneralTab()`, `buildPresetsTab()`, `collectPresetShortcuts()`, `save()`,
  the `tabs_` `QTabWidget`; `src/app/pages/MainWindow.cpp:106-111`
  (`openSettings()` calls `search_->refreshFeedSettings()` on accept).
  **PROPOSED:** a Processing tab/section, and `src/player/native/NativePrefs.cpp`.
- **Work:** Add a compact **Processing** section to the existing settings
  dialog, themed with the existing tokens (`theme/Colors.hpp`,
  `theme/Metrics.hpp`, `MenuPopup`) — no new dialog window and no new visual
  language. Persist under a **new, versioned** group (`player/native`, version
  sub-key). **Additive only:** existing `player/anime4k`,
  `player/presetShortcuts`, the quality cache, feeds, home/feeds and language
  settings are never repurposed or cleared. The native namespace is separate from
  the Anime4K preset-key space, and a **shared conflict check** refuses a native
  key that collides with an existing reserved or preset key. A saved tick for an
  unavailable choice persists anyway, with the effective value and reason shown
  separately. `MainWindow::openSettings()` must **not** trigger an API cache reset
  because a native preference changed — only feed settings should.
- **Acceptance:** Changing a native preference and reopening settings shows the
  stored value, including for a choice that is currently unavailable. No existing
  setting is lost or reset by a native preference change. A key that collides
  with a reserved or preset key is refused with a message naming the owner.
  Opening settings changes nothing in the quality cache or the feed state.
- **Verification:** PROPOSED `tests/cpp/TestNativePrefs.cpp` (round-trip,
  version migration, collision refusal) in `mikai_tests`; a PROPOSED
  `native_settings_probe` driving the real dialog.
- **Blockers:** M33.

<a id="m35"></a>

### M35 — Add model import, asset preparation, and safe cache management controls

- [ ] **Status:** implemented portions unverified — owned model-import/inventory/cache panel and process-wide artifact leases landed; cancellation now gates publication. Durable imported-model recipe requests are being connected. Actual preparation remains blocked on the licensed M15 helper; no engine progress or Ready capability is fabricated.
- **Depends:** M34, M11, M15
- **Files (actual):** `src/app/dialogs/SettingsDialog.cpp` (tabs, file dialogs);
  `src/app/pages/TitlePage.cpp:1417-1421` (the existing media-open arguments
  builder, the pattern for a native `QFileDialog`); `src/player/PlayerPopups.hpp`.
  **PROPOSED:** `src/player/native/NativeAssetsPanel.cpp`.
- **Work:** In the Processing section: import a model file, show the inventory
  (label, role, dtype, size, backend compatibility, terms, content ID), download
  or prepare assets, view preparation state per stage, and clean the engine cache
  **safely** — the cleanup may delete only entries under the app-owned
  `AppLocalDataLocation/native/artifacts/` (plus a cleared `scratch/`) whose manifests
  validate, never anything in a user or shared directory. Import must run the
  M11 inspection and refuse an incompatible model with the actual reason rather
  than adding it to the list. Deleting an engine in use must be refused while a
  plan references it.
- **Acceptance:** Importing a compatible model adds exactly one inventory entry
  and it is immediately selectable; importing an incompatible one adds nothing
  and says why. "Clean cache" removes only app-owned validated artifacts and
  leaves user files byte-identical. Cleaning an artifact in use is refused with a
  reason. A preparation triggered from here updates the panel live.
- **Verification:** PROPOSED `native_assets_probe`; plus
  `ctest --test-dir build-cmake -R mikai_tests` for the inventory logic.
- **Blockers:** M11, M15.

<a id="m36"></a>

### M36 — Add native shortcut bindings with shared collision checks

- [ ] **Status:** partially implemented, unverified — shared collision-check and native binding persistence edits landed. Runtime binding/processing-request handler integration and real keyboard checks remain; no unacknowledged activation is allowed.
- **Depends:** M34
- **Files (actual):** `src/player/PresetShortcuts.hpp` — the one shared contract
  for where bindings live (`kPresetShortcutSettingsGroup`) and which keys a
  preset may never take (`isReservedPresetShortcut()`), explicitly kept in one
  place so the settings dialog and the player cannot drift;
  `src/player/PlayerDialog.cpp:1144` (`loadPresetShortcuts()`);
  `src/app/dialogs/SettingsDialog.cpp:237-290, 400-470` (load, validate, save).
  **PROPOSED:** `src/player/native/NativeShortcuts.hpp`.
- **Work:** Add a **separate** `player/native/shortcuts` group with its own
  stable action ids, reusing — not duplicating — the reserved-key predicate and
  the duplicate-claim check the preset bindings already use. A native binding
  must be refused if it collides with a reserved player key, an existing preset
  binding, or another native binding. A stale or now-reserved stored binding is
  skipped at load exactly as a preset binding is, never installed over a key the
  player acts on. On every path, a binding is only written once the whole set
  validates.
- **Acceptance:** Binding a reserved key is refused with the owner named.
  Binding a key already used by an Anime4K preset is refused. Duplicate native
  bindings are refused. A stored binding that has since become reserved is
  skipped and reported. Valid bindings install and fire the native action.
  No existing preset binding changes behaviour.
- **Verification:** PROPOSED `tests/cpp/TestNativeShortcuts.cpp` in
  `mikai_tests`; `build-cmake/interaction_probe.exe` for the live keys.
- **Blockers:** M34.

<a id="m37"></a>

### M37 — Display truthful native processing and cadence status in the HUD

- [ ] **Status:** pending integration — typed status consumer exists; shell/dialog presentation and actual surface verification remain. Original/unavailable must be shown separately from the saved request, and Active/cadence fields require the filter's own report.
- **Depends:** M20, M33
- **Files (actual):** `src/player/ShellWidgets.hpp:159` — `class StateHUD :
  public Panel` with `updateData(const QString& state, const
  QVector<QPair<QString, QString>>& data)`; `src/player/PlayerShell.cpp:879-880`
  (`hud_->updateData(stateName(), …)`); `src/player/VideoSurface.cpp:459-463`
  (`mpv_->getNumber("estimated-vf-fps")`); `src/player/PlayerShell.cpp:1080`
  (`updateInfoChip()`, the enhancement chip).
- **Work:** Show the selected provider, model, source→output geometry, fps and
  the rational active-interpolation factor, driven from M20's snapshot. **The
  existing `estimated-vf-fps` remains an mpv-measured output indicator — it is
  not inference proof and not a source work rate**, and must be labelled so. Do
  not let a native "active" claim come from the HUD; it comes from the
  filter-reported effective plan. The HUD must show `Preparing` and the reason,
  not a blank or an optimistic spinner, and must reflect `Unavailable` truthfully
  when a machine cannot offer a backend.
- **Acceptance:** With native active the HUD names the provider, model, geometry
  and interpolation factor; with native preparing it says so with the reason;
  with native unavailable it says why. The label for `estimated-vf-fps` makes no
  claim about inference. The HUD never reports `Active` before the filter's
  snapshot does.
- **Verification:** PROPOSED `native_hud_probe` (state transitions driven from a
  fixture snapshot) plus `build-cmake/shell_probe.exe` and
  `build-cmake/widget_pixel_probe.exe` for rendering.
- **Blockers:** M20, M33.

<a id="m38"></a>

### M38 — Complete English/Ukrainian, accessibility, and geometry coverage

- [ ] **Status:** catalog compilation passes, geometry repair unverified — Qt lrelease generates 728 finished Ukrainian translations and 0 unfinished. The actual English RIFE screenshot exposed an oversized Processing tab; whole-tab scrolling is implemented but not yet rebuilt/observed. Final placeholder checks and English/Ukrainian minimum-size, accessibility and HiDPI surface proof remain open. Locale-aware probe initialization is wired into settings, RIFE, menu and asset probes. Model table roles and unavailable labels no longer expose wire names as the primary UI text.
- **Depends:** M34, M36
- **Files (actual):** `translations/mikai_uk.ts`; `src/app/I18n.cpp` /
  `I18n.hpp` (`languagePreference()`, `resolvedLanguageCode()`);
  `src/app/main.cpp` (the `":/i18n/mikai_uk.qm"` catalog load and the
  `qtbase_uk.qm` deployment); `CMakeLists.txt`
  (`qt_add_translations(... RESOURCE_PREFIX "/i18n" LRELEASE_OPTIONS
  -nounfinished)`); `src/player/PlayerShell.cpp` (the existing pattern of
  `setToolTip(hint)` + `setAccessibleName(hint)`, e.g. `updateSeriesToggle`).
  **PROPOSED:** none — this task adds strings, not structure.
- **Work:** Every native-facing string is an English `tr()` source with a
  Ukrainian translation, including unavailable reasons, preparation stage names,
  provider/model labels and error text. Preserve Qt placeholders `%1`/`%n`,
  numerus forms and markup. **Do not translate** stable IDs, capability enums,
  model content hashes, protocol/schema version strings, paths or backend names.
  Native settings and menu rows follow the same accessibility contract as the
  rest of the player: a tooltip and an accessible name on every interactive row,
  keyboard reachability through the existing `MenuPopup` arrow-key model, and
  layout that survives long labels and long reasons at the smallest supported
  window size. The catalog must contain no unfinished entries before release
  (`-nounfinished` already drops them, which silently falls back to English —
  verify rather than assume).
- **Acceptance:** Every native string appears in `mikai_uk.ts` with a real
  translation; no unfinished entry remains; `%1`/`%n` placeholders and numerus
  forms are preserved; switching language updates settings, menus and the HUD
  without a restart of the player. Long reason text wraps or ellipsises without
  clipping a row. Every interactive native row is keyboard reachable and has an
  accessible name.
- **Verification:** the existing
  `cmake --build build-cmake --target update_translations` followed by
  `ctest --test-dir build-cmake -R mikai_tests` (`TestI18n`);
  `build-cmake/anime4k_menu_probe.exe`; plus a PROPOSED `native_geometry_probe`
  at the minimum supported window size in both languages.
- **Blockers:** M34.

<a id="m39"></a>

### M39 — Separate processing failures from media retry and signed-URL recovery

- [ ] **Status:** unchecked
- **Depends:** M34, M20
- **Files (actual):** `src/player/PlayerDialog.cpp` — `onEngineError` (≈1221),
  `retryPlayback()` (≈1909, which retries the **same** URL), `startPlayer()`
  (≈1928), `showErrorStatus`, `flashStatus`; `src/app/pages/TitlePage.cpp` —
  `recoverStream()` (≈1616, `qualityCache_.forget(embed)` then re-extract),
  the host `playbackFailed()` (≈225), `switchPlayerVariant()`,
  `probeTrackQuality()`; `src/app/state/QualityCache.cpp` (30-minute TTL).
  **PROPOSED:** native failure classification in `NativeController.cpp`.
- **Work:** Classify engine errors into **media** failures and **processing**
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
- **Acceptance:** An expired signed URL still shows the retry chip and recovers
  through re-extraction. A native preparation or engine failure shows a
  processing reason with processing remedies, triggers **no** API call, and does
  not change the URL. A direct-media (non-anime) player gets no anime host
  controls on either path.
- **Verification:** PROPOSED `native_failure_class_probe` over the M02 expired-URL
  fixture plus injected processing failures; the existing
  `build-cmake/external_media_probe.exe` for the media path.
- **Blockers:** M20.

<a id="m40"></a>

### M40 — Harden runtime, model, artifact, path, and diagnostic trust boundaries

- [ ] **Status:** independent hardening implemented, unverified — status/artifact diagnostic guards reject sensitive identities and redact exported URLs/credentials; ownership/path validations exist. Hostile-input CPU and surface proof remains; native deserialize acceptance still requires the licensed M18 producer.
- **Depends:** M34, M18
- **Files (actual):** `src/backend/Runtime.cpp` (downloads, the GitHub Releases
  API parse, the `7za` extraction, `QDir::addSearchPath` usage);
  `src/backend/MikaiClient.cpp` (token handling, rate limiting);
  `src/app/CrashLog.cpp` / `.hpp`; `src/player/PlayerDialog.cpp:1183`
  (`copyStreamUrl`); `src/player/VideoSurface.cpp` (`onMediaDropped` path
  handling in `PlayerDialog.cpp:1442`). **PROPOSED:** validation helpers under
  `src/player/native/`.
- **Work:** Every path that reaches native code is validated: no traversal, no
  UNC surprise, no unquoted metacharacters, no over-long paths, and no directory
  ever chosen from a manifest without checking it is inside the app-owned root.
  Model imports are validated before touching the inventory. Artifacts are
  validated before any deserialize. **Diagnostics must never contain URL tokens
  or API keys** — the native plan follows the source's w/h/fps, not URL
  identity, so no URL is needed; native inference reads only media metadata
  libmpv already provides. Scrub the native status, the manifest and any exported
  diagnostic of anything URL-shaped. Keep the existing download safety in
  `Runtime.cpp` intact and apply the same care to any asset download. **Unicode
  paths throughout:** every native load goes through the wide `LoadLibraryExW`
  with a proper UTF-8 → UTF-16 conversion, and a path outside the native
  filesystem's representable set is rejected with a named reason rather than
  silently narrowed or substituted.
- **Acceptance:** A manifest naming a path outside the app-owned root is
  rejected. A traversal-shaped model path is rejected. An exported diagnostic and
  the status snapshot contain no URL, token or API key. A crafted path with a
  drive colon, comma, percent or non-ASCII byte never corrupts the filter
  command or the log.
- **Verification:** PROPOSED `native_trust_probe` (hostile manifests, traversal
  paths, token-shaped strings planted in a URL) plus
  `ctest --test-dir build-cmake -R mikai_tests`.
- **Blockers:** M18, M34.

<a id="m41"></a>

### M41 — Add focused consumer-visible CPU regression coverage (matrix green on the current tree: 25 PASS + LiveOracle SKIP, offscreen, ~15 s)

- [ ] **Status:** unchecked
- **Depends:** M40, M38, M39, M37
- **Files (actual):** `tests/cpp/main.cpp` (the suite list, `runOne`,
  `wantsSuite`, `suiteFilterActive` — every new suite must be registered here),
  `tests/cpp/TestSupport.hpp` (the `asText`/`asInt`/`asFlag` adapters),
  `CMakeLists.txt` (`mikai_tests` source list). **PROPOSED:** `TestNativeRecipe`,
  `TestNativeTypes`, `TestNativeManifest`, `TestNativePrepPlan`, `TestNativePrefs`,
  `TestNativeShortcuts`, `TestNativeRife`, `TestNativeStatus`, `TestNativeBundle`,
  `TestNativeFailure`.
- **Work:** Cover the pure, CPU-testable contracts from M10–M14, M34, M36, M39
  and M40: recipe round-trip and ordering; availability reasons; manifest
  identity and invalidation; preparation-plan geometry over the M02 matrix;
  preference persistence and migration; shortcut collision; failure
  classification; bundle validation; and the structured status snapshot
  (requested vs effective, revision monotonicity, protocol mismatch). Tests must
  assert **consumer-visible behaviour and boundaries** — an exact binding
  mismatch, a precedence rule, a transition, an error — never that a function was
  called, never a mock echo, never "not empty", never a re-assertion of source
  wording. Do not resurrect anything deleted in M01.
- **Acceptance:** Every new suite is registered in `main.cpp` and in the
  `CMakeLists.txt` source list; `ctest --test-dir build-cmake -R mikai_tests`
  exits 0 hermetically with `MIKAI_LIVE` unset and `TestLiveOracle` **skipping**.
  Each new suite fails when its contract is broken (deliberately verified by a
  temporary mutation, reverted afterwards).
- **Verification:** `ctest --test-dir build-cmake -R mikai_tests`;
  `build-cmake/mikai_tests.exe TestNativeRecipe` etc. for single suites.
- **Blockers:** M38, M40.

<a id="m42"></a>

### M42 — Prove fresh plain-player and native-off desktop regressions

- [ ] **Status:** unchecked
- **Depends:** M41, M07
- **Files (actual):** `src/probes/WidgetPixelProbe.cpp`,
  `src/probes/HeadlessPixelProbe.cpp`, `src/probes/DialogProbe.cpp`,
  `src/probes/ShellProbe.cpp`, `src/probes/mpv_surface_smoke.cpp`,
  `src/probes/InteractionProbe.cpp`, `src/probes/CompareProbe.cpp`,
  `src/probes/Anime4KMenuProbe.cpp`, `src/probes/ResolverProbe.cpp`,
  `src/probes/CrashProbe.cpp`.
- **Work:** Rebuild every probe against the **current** source through the new
  `MpvApi` and re-establish the pre-change desktop behaviour: real window, real
  GL, non-black framebuffer, real `estimated-vf-fps`, real decoder/VO drop
  counts, working close/reopen, and no crash on shutdown. All of them must run
  with the **standard** bundle and native off, and none may depend on a network,
  a key, or an ambient install.

  **Fresh evidence already gathered, which this task consolidates rather than
  re-collects:** all three current project C++ libraries plus the full original
  `tests/cpp` sources compiled in a temp Debug build; an INI-isolated runner
  wrapping the original `tests/cpp/main.cpp` (no filters, `MIKAI_LIVE=0`, empty
  API key) gave **175 pass / 0 fail / 1 skip (LiveOracle off), exit 0** once the
  existing `shaders/` were properly deployed beside the runner — the earlier
  `TestAnime4K` failure was a scratch prerequisite (no shaders next to the
  runner), not an application defect. No linter is configured. A fresh
  `DialogProbe.cpp` wrapper linked against the freshly built `mikai_player.a` (not
  the stale prebuilt archive) with a real clip, on **both** the stock core and
  the Janai fork core with the native vf **off**, both exited 0 with **identical
  geometry on both cores** — dialog 600×1300, shell/surface 720×1300, source
  1920×1080, playing, 48 visible children — and a lit framebuffer (native core
  71732/234000 lit vs stock 71109/234000); the full widget PNG was visually
  inspected. Both cores came from the same fresh temp Debug build. The tall
  geometry is a property of the probe (no main window, no Fusion stylesheet, no
  normal placement), **not** a native-core defect.

  What that evidence does **not** cover, and what this task still owes: the NN
  filter has **not** been proven inside the full `PlayerDialog` — it is proven
  only through the current core/render bridge; PiP, Compare, HLS and native RIFE
  are all unproven. Any probe still asserting stale literals or renamed objects is
  fixed **here**, as behaviour, not by re-pinning wording.
- **Acceptance:** Every probe builds and runs on a freshly configured tree with
  exit 0 (or an explicitly reported skip), reports real numbers — `hasVideo`,
  playing, position, duration, video size, context ready, non-black percentage,
  drop counts — and closes cleanly. `build-cmake/widget_pixel_probe.exe` and
  `build-cmake/mpv_surface_smoke.exe` show a real non-black frame with the
  standard bundle and native off. Any probe failure is classified as app defect
  versus harness/asset gap and reported as such.
- **Verification:** the probe targets listed in `CMakeLists.txt`, each run
  directly, plus `ctest --test-dir build-cmake -R mikai_tests`.
- **Blockers:** M07's loader cutover (probes must not link libmpv at load time).

<a id="m43"></a>

### M43 — Prove CUDA native inference reaches the actual Qt framebuffer

- [ ] **Status:** blocked — Actual Qt framebuffer native inference proof requires licensed native runtime and real prepared engines.
- **Depends:** M42, M23, M32
- **Files (actual):** `src/player/VideoSurface.cpp` (`grabFramebuffer()` usage,
  `videoSizeChanged`), `src/player/MpvRenderContext.cpp` (`render(int fbo, …)`);
  the M02 fixtures. **PROPOSED:** `native_framebuffer_probe`.
- **Work:** Prove, end to end and on **current source**, that CUDA inference
  output actually reaches the Qt framebuffer: decode → native vf → colorscale →
  render API → the `QOpenGLWidget`'s own FBO. Measure the framebuffer the player
  actually shows (mean, variance, non-black fraction), save PNGs for visual
  inspection, and confirm the output geometry matches the learned plan. Compare
  against native-off on the same fixture. Do **not** present this as performance
  or fidelity certification, and do not claim a zero-copy path: the interop does
  device copies and a sync. If the debug `GL_INVALID_ENUM` seen in the research
  harness reappears, disclose it as unresolved rather than declaring the run
  warning-free.
- **Acceptance:** **Conditional, and deliberately so.** On a **seekable** fixture
  (or when a valid frame is already cached), Prepare pressed while **paused**
  yields a real device UUID (or adapter LUID) and a resolved output state
  **without unpausing**, and the status shows Probe before Prepare and the
  resolved output state after it. On a **non-seekable** source with no frame
  available, the correct outcome is the opposite: the identity stays **Unknown**,
  playback stays on Original, the user is **not** unpaused, and no identity is
  guessed. An unconditional "must always produce a UUID" requirement would be
  wrong, because it would force exactly the guess this design forbids.
  With native active the framebuffer is non-black, its geometry matches the plan,
  and its content visibly differs from native off on a real frame. The evidence includes saved PNGs and measured statistics, not exit code
  alone. Any GL debug error is reported explicitly.
- **Verification:** PROPOSED `native_framebuffer_probe --fixture <file> --recipe
  <id> --png <out.png>`, run on the CUDA machine.
- **Blockers:** M23, and a CUDA machine.

<a id="m44"></a>

### M44 — Prove RIFE timing, cuts, padding, seeks, and end-of-stream behavior

- [ ] **Status:** blocked — Temporal GPU/cadence acceptance requires exact original RIFE assets and real licensed runtime.
- **Depends:** M43, M24
- **Files (actual):** the M02 cut/EOF fixtures; `src/player/PlayerDialog.cpp`
  (`restoreResumePoint`, `openStream`), `src/player/VideoSurface.cpp`
  (`duration()`, `position()`).
  **PROPOSED:** `native_rife_timing_probe`.
- **Work:** Measure real RIFE behaviour with real video: output frame rate equals
  the rational factor times the source rate and stays rational over a long clip
  (no drift, no rounding); a hard cut produces **no** blended ghost frame (the
  `AJI_SCENE` path emits a duplicate of frame `a`); padding behaviour at the
  first and last frames is correct; a seek mid-stream re-anchors PTS correctly and
  the timeline stays monotonic; EOF and near-EOF do not hang or emit frames past
  the end; audio stays in sync with the interpolated video. Compare against the
  same fixture with RIFE off.
- **Acceptance:** Frame counts and PTS are monotonic across seeks and EOF. The
  cut frame is a duplicate of `a`, not a blend. Drift over the clip stays within
  one frame. A/V offset is measured and reported, not assumed. Every claim is
  backed by a recorded number.
- **Verification:** PROPOSED `native_rife_timing_probe` over the M02 fixtures
  with per-frame PTS/drop dumps and saved PNGs at the cut.
- **Blockers:** **M12's missing RIFE assets.**

<a id="m45"></a>

### M45 — Stress native PiP, Compare, close, and resource lifetimes

- [ ] **Status:** blocked — Native lifetime stress requires real active native recipes; plain pooled PiP/Compare coverage remains reachable.
- **Depends:** M44, M30, M31, M32
- **Files (actual):** `src/player/PlayerDialog.cpp` (`togglePip` dispatching to
  `enterPip`/`leavePip`, plus `closeEvent`), `src/player/CompareWindow.cpp`,
  `src/player/VideoSurface.cpp`.
  **PROPOSED:** `native_stress_probe`.
- **Work:** Run the two-core, Compare and close paths hard: 100 PiP enter/leave
  cycles, 100 Compare open/close cycles, 100 player open/close cycles with a
  native plan active, interleaved, with resource sampling throughout (GPU memory,
  CUDA context count, engine handles, thread count, file handles). Include the
  close-while-PiP case that today only calls `leavePip(true)`. Assert flat
  resources after the first cycle and no leak trend; assert no crash and no
  driver fault. This is where the PiP source risk and the historical
  `DrvPresentBuffers` concern are actually settled by measurement.
- **Acceptance:** Resources return to baseline after the first cycle and stay
  flat; no crash, no hang, no GPU fault across 100 cycles each; a close during
  PiP releases the parked float's stream and native resources. A device,
  context or cadence change during a handoff invalidates the plan and re-probes
  for the new epoch rather than carrying a stale build key across — the stress
  run must exercise that path, not just the identical-device happy case.
- **Verification:** PROPOSED `native_stress_probe` with a resource trace saved
  to a file for review.
- **Blockers:** M30, M31.
<a id="m46"></a>

### M46 — Prove native local/HTTP/HLS and anime session transitions

- [ ] **Status:** blocked — Full native streaming/session acceptance requires real recipes; local HTTP/HLS fixture and native-off playback checks are independently reachable.
- **Depends:** M02, M28
- **Files (actual):** `src/probes/ExternalMediaProbe.cpp`; `src/backend/Streams.cpp`
  (`ashdi`/`moon`/`tortuga` extraction, `detectSite`, `qualityUrl`);
  `src/app/pages/TitlePage.cpp` (`recoverStream`, `ensureQualities`,
  `applyEpisode`, `applyVariant`, `applyRefreshedStreams`, `openPlayer`);
  `src/app/state/QualityCache.cpp`; `src/app/dialogs/SettingsDialog.cpp` (URL
  and quality keys).
  **PROPOSED:** `native_media_probe`.
- **Work:** Prove the full media matrix with native processing enabled: a local
  file, a `file://` URL, an HTTP progressive stream, and an adaptive HLS
  manifest with variant switching — plus the anime session transitions: quality
  change, episode change, dub/sub variant change, and stream-expiry recovery.
  Every one must keep the native plan correct for the new source geometry, keep
  the user's quality/episode/dub/sub preferences, and preserve the HTTP headers
  the CDN requires. The native plan follows source w/h/fps, not URL identity.
  A live CDN is **optional** — the hermetic fixtures from M02 are the required
  proof; a live run, if a key is available, is additional.
- **Acceptance:** Each media kind plays with native active and reaches the
  prepared plan for its geometry. Quality and episode switches re-derive the
  plan; when the new geometry is unprepared, playback continues on Original with
  a visible reason. An expired signed URL recovers through the existing
  re-extraction path and does not disturb the native plan. Preferences survive
  every transition.
- **Verification:** PROPOSED `native_media_probe` against the M02 fixtures;
  `build-cmake/external_media_probe.exe` for the standard path.
- **Blockers:** M45, M02.

<a id="m47"></a>

### M47 — Prove preparation and artifact failure recovery

- [ ] **Status:** blocked — Engine/artifact failure-recovery acceptance requires real preparation helper and at least one produced engine.
- **Depends:** M14, M15, M18, M28, M35
- **Files (actual):** `src/player/PlayerDialog.cpp` (`onEngineError`,
  `showErrorStatus`, `retryButton_`), `src/app/CrashLog.cpp`.
  **PROPOSED:** `native_prep_failure_probe`.
- **Work:** Run the full negative matrix against real artefacts, not mocks:
  **cold** (no engines — Prepare from scratch), **corrupt** (truncated engine, a
  manifest that does not match, a foreign engine file dropped into the cache),
  **stale** (driver or model changed under an existing engine), **missing
  asset**, **cancelled**, **builder failure**, and **disk-full-ish** (a full
  temp volume). For each, the installed bundle, the reported status, the
  visible state and the pixels must all tell the truth: no silent pass, no log
  tail, no half-activated chain, no leftover partial artifact, and Original
  still playable. Recovery must be reachable without a restart.
- **Acceptance:** Each failure mode produces a named reason, keeps Original
  playing, and leaves the cache in a state the next run can use. A corrupt or
  mismatched engine is never deserialized. Cancelling mid-build leaves nothing
  loadable behind. No failure mode prints a bare "exit 0" success.
- **Verification:** PROPOSED `native_prep_failure_probe` over the whole matrix,
  reporting status, visible state and pixels per case.
- **Blockers:** M15.

<a id="m48"></a>

### M48 — Prove DirectML provider identity and GPU rendering where offered

- [ ] **Status:** blocked — DirectML GPU/provider proof requires a real offered DirectML runtime and D3D11/WGL capability; never infer from CUDA.
- **Depends:** M25, M43
- **Files (actual, read-only):** `…/sanity/dml_spike.cpp`, `…/src/aji_dml.cpp`
  (D3D11 shared textures, the ORT session), package `aji_dml.dll` +
  `DirectML.dll`. **PROPOSED:** `dml_runtime_probe`.
- **Work:** On a DirectML-capable machine, prove the real thing: the actual
  provider and device identity, that Qt's GL surface can actually receive
  DirectML-produced frames (the interop the CUDA path does **not** prove), and
  that a spatial chain runs and is pixel-verified. Where the proof fails, the
  capability stays disabled with a factual reason and that outcome is recorded —
  a disabled DirectML track is a valid, honest delivery. Record whether a debug
  GL error appears, unresolved or not.
- **Acceptance:** With DirectML proven, the status names the provider and device
  and the framebuffer is pixel-verified. Without the proof, the UI shows
  DirectML as disabled with the specific missing capability, and no part of the
  app claims DirectML support. No DirectML RIFE is offered or claimed.
- **Verification:** PROPOSED `dml_runtime_probe` with saved PNGs and provider
  identity in the report.
- **Blockers:** **D3D11 → WGL interop is unproven.** The GPU, the DirectML
  backend DLL and the DirectML runtime are all present, so this is a capability
  question rather than a missing-SDK one; if the interop does not import, a
  disabled DirectML track is the correct and legitimate outcome.

<a id="m49"></a>

### M49 — Measure sustained processing, responsiveness, and resource stability

- [ ] **Status:** blocked — Sustained native measurements require functional prepared spatial/temporal processing; no fabricated performance.
- **Depends:** M41, M42, M43, M44, M45, M46, M47, M48
- **Files (actual):** `src/player/VideoSurface.cpp` (the 50 ms state pump,
  `estimated-vf-fps`), `src/player/PlayerShell.cpp` (HUD), `src/player/PlayerDialog.cpp`
  (`openStream`, the resume fields). **PROPOSED:** `native_perf_probe`.
- **Work:** Measure, over a long warm run (tens of minutes) with real media:
  sustained processed frame rate and its variance; per-stage and end-to-end GPU
  latency; CPU and GPU memory over time; engine load time; and **GUI
  responsiveness** during native apply, status polling, preparation and PiP
  handoff — measured as input-to-paint latency and event-loop lag, not asserted.

  This is the **responsiveness gate** for the whole async design. The event loop
  must stay responsive while the native core configures, builds, drains and
  suspends on its own thread. A HUD that updates from a 500 ms status poll must
  not also block the GUI thread on that poll: the poll is an async
  `mpv_get_property_async` (M20), plan apply is an async `mpv_command_async`
  (M26), and both coalesce into the existing 50 ms state bank (M07). Measure the
  lag during each of those four operations separately, so a stall can be
  attributed rather than averaged away. Compare native-off, spatial-only,
  RIFE-only and combined where available. Report real numbers; do not invent a
  target and do not call a number good or bad without stating the reference it
  was compared to.
- **Acceptance:** Numbers are recorded per configuration with the machine and
  inputs stated, and per operation (apply / poll / prepare / handoff) rather than
  only in aggregate. Event-loop lag stays bounded in each of the four, and the
  numbers are stated as observations against whatever reference was used.
  Memory is flat over the run after warm-up. Frame pacing shows no long stall at
  activation. Any measurement that cannot be taken on this machine is reported as
  not measured, not estimated.
- **Verification:** PROPOSED `native_perf_probe --duration <s> --json <out>`,
  run on the CUDA machine.
- **Blockers:** every gate listed above — a long warm run and the performance
  numbers derived from it are release evidence, so they may only be taken after
  the correctness gates they measure have passed.

<a id="m50"></a>

### M50 — Prove independent clean installation, upgrade, and rollback

- [ ] **Status:** blocked — Complete Native clean-install/upgrade/rollback acceptance requires cleared applicable terms and real coherent native bundle. No redistribution currently authorized.
- **Depends:** M49
- **Files (actual):** `CMakeLists.txt` (`MPV_INCLUDE_DIR`, the probe/test
  targets, the `POST_BUILD` shader and `qtbase_uk.qm` deploys), `readme.md`
  §Build/§Deploy, `src/app/main.cpp`, `src/backend/Runtime.cpp`.
  **PROPOSED:** `native/install/`, `native/tools/bundle_tool`.
- **Work:** Prove the shipped artefact works on a **clean machine with no
  Python, no Conda, no ambient CUDA/TensorRT/ONNX install, and no network**.
  Install the bundle, verify the self-contained dependency closure, run the
  standard and native paths, upgrade from a previous bundle version, and roll
  back to it — with the app detecting and reporting each state. Prove an offline
  baseline (existing features work with no network at all), that a restart
  after a selection change picks the right bundle, and that rollback leaves a
  working app. **Licensed bundle only**: if M03 blocked any component, this task
  cannot pass and must say so.
- **Acceptance:** A clean, network-free machine runs both paths with no ambient
  installs. An upgrade preserves preferences and rebuilds nothing it need not. A
  rollback restores the previous working state. A failed install leaves the
  previous bundle intact and usable. Restart-after-selection is deterministic.
- **Verification:** PROPOSED `native/install/verify_clean.ps1` (or an
  equivalent documented manual procedure) plus `bundle_tool verify`.
- **Blockers:** **M03's licence/redistribution decision. A blocked component
  makes this unachievable and it must be escalated rather than waived.**

<a id="m51"></a>

### M51 — Update this project's user and maintainer documentation

- [ ] **Status:** in progress — independent docs landed and verified: the loader
  contract (`MpvApi`, no load-time libmpv import, `MPV_INCLUDE_DIR`), the
  observed acceptance state (fresh matrices, `dumpbin`, Qt/OpenGL smoke,
  attached-session desktop runs), the rights-blocked native-track note, the
  `mikai_http_fixture` fixture contract and the observed EXT/HLS lines, and the
  episode-swap pause invariant. Native preparation/provider behavior stays
  undocumented until M49/M50 verify it. Blockers M49/M50 unchanged.
- **Depends:** M49, M50
- **Files (actual):** `readme.md`, `AGENTS.md`, `docs.md` (the API contract —
  unchanged by this work), `CMakeLists.txt` comments.
- **Work:** Update the documentation to match what actually landed. Maintainer
  side: the `MpvApi` loader contract and why no DLL is linked at load time; the
  bundle layout and the atomic install; the native patch set and the
  `preparedOnly`/manifest/GET_META semantics; the preparation CLI and that it is
  out-of-band; the DirectML gate and what "disabled with a reason" means; the
  new suites and probes and how to run each; the `MPV_INCLUDE_DIR` rename and
  the updated configure command. User side: turning native on, preparing models,
  what `Preparing` means, what an unavailable provider looks like, and that
  Anime4K and native compose rather than replace each other. Correct any place
  the docs still describe the pre-change behaviour (including the stale
  `provisionMpvRuntime()` ordering comment and the "RIFE is planned" leaf).
  **Only describe verified behaviour** — anything not proven by M42–M50 is
  documented as unproven or absent.
- **Acceptance:** Every documented command runs as written. No document claims a
  capability that M42–M50 did not verify. The old `MPV_RUNTIME_DIR`/linked-`MPV_LIBRARY`
  instructions are gone everywhere, including `CMakeLists.txt` comments.
- **Verification:** run each documented command from a clean checkout; a
  documentation audit of `readme.md`/`AGENTS.md` against the landed build.
- **Blockers:** M49, M50 — documentation describes **implemented, measured**
  behaviour, not intentions.

<a id="m52"></a>

### M52 — Complete the full no-regression release acceptance matrix

- [ ] **Status:** blocked — Full52-task release matrix remains incomplete until every native and independent acceptance gate passes.
- **Depends:** M51, M41, M43, M45, M46, M47, M48
- **Files (actual):** `readme.md`, `AGENTS.md`, `translations/mikai_uk.ts`,
  `CMakeLists.txt`. **PROPOSED:** `docs/` acceptance record (the matrix itself,
  with every cell's evidence pointer).
- **Work:** Run the terminal gate over the whole matrix and record evidence for
  every cell, including the cells that are **not** satisfied: the prepared/cold/
  corrupt/status negative matrix; RIFE real-video cuts/EOF/seek/rational
  counts/A-V; native PiP and Compare across two cores and 100 cycles; DirectML
  capability where offered and its honest disabled state elsewhere; long warm
  performance and responsiveness and memory; a clean machine with no Python, no
  Conda and offline; restart and rollback; and the licensed bundle. Confirm the
  catalog has no unfinished entries. Confirm every existing probe and suite
  still passes. Confirm no task is marked complete while a reachable acceptance
  is unrun. **Do not ship with implementation tasks ticked and acceptance
  unexecuted.**
- **Acceptance:** Every matrix cell has an evidence pointer (command, saved
  artefact or recorded measurement) or an explicit, justified `not satisfied`
  with the blocker named. Any unsatisfied cell blocks release. The matrix is
  reproducible from a clean checkout by someone who did not write the code.
- **Verification:** the recorded commands for every cell, run end to end.
- **Blockers:** every unsatisfied cell, including the M03 licence gate and the
  M12 RIFE assets if still outstanding.

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

## 4. Continuation instructions and live tracking

- [ ] **C01 — Resolve verification restriction and run the fresh isolated baseline.**
  The session-only rule override is active. Ten initial current-source hermetic
  suites passed, LiveOracle skipped, focused Anime4K passed, and the real Windows
  widget framebuffer produced the recorded pixel proof. Final isolated harness
  and full desktop regression acceptance remain open.
- [x] **C02 — Record private non-commercial scope and draft an unsent inference permission request.**
  Recorded in `native/permission-request.md`; the draft was not sent and grants
  no permission.
- [x] **C03 — Obtain and configure official native development SDKs without replacing installations.**
  Isolated official SDK acquisition and genuine MSVC/CUDA/TensorRT/GPU smoke
  evidence are recorded in `native/sdk-acquisition.md`. No driver, administrator
  install or existing runtime was replaced.
- [ ] **C04 — Implement legally independent player-side slices with explicit prerequisites.**
  Loader, bundles, recipes, planning, artifacts, inspection, preferences and
  status consumers have landed edits or active owners. Acceptance is tracked
  under the individual M tasks, never inferred from a worker handoff.
- [ ] **C05 — Record per-task observed evidence and precise blocker scope.**
  Status lines distinguish pending work, implemented-but-unverified edits and
  named Native/provenance prerequisites. Final verification records remain open.
- [x] **C06 — Use m5/GPT-6-Luna for scout jobs.**
  Bounded read-only source scouts completed and returned integration findings.
- [x] **C07 — Use m6/DeepSeek V4.1 Flash for deep implementation jobs.**
  Deep implementation slices use m6. Interrupted HTTP-400 worker edits remain
  unverified; fresh bounded correlation/status/preference owners have handed off
  their source without claiming runtime acceptance.
- [x] **C08 — Keep todos and tasks.md synchronized with observed progress.**
  All 52 original tasks remain tracked in both OMP's live list and this file.
  Every blocked task names its real prerequisite; implementation status is not an
  acceptance tick. C13–C22 track observed repair work rather than hiding it in
  pending parent tasks.
- [x] **C09 — Inspect VideoJaNai 2.1.0 manifests, all supplied asset pins and component provenance.**
  Recorded the 14 uploaded asset identities plus two generated source archive
  URLs. Six acquired component/metadata digests were independently recomputed;
  see `native/videojanai-acquisition.md` and `native/manifest/videojanai-release.json`.
- [x] **C10 — Verify SM120, PTX and TensorRT runtime compatibility without changing the NVIDIA driver.**
  Investigation found the pack's declared TensorRT 11.0 / CUDA 13.2 line differs
  from the owned 11.3.0.99 / 13.4.92 SDK. They remain separate; shared DLL names
  are not compatibility proof. No GPU execution or driver acceptance is claimed.
- [x] **C11 — Check component-rife against the required original RIFE variants and weight lineage.**
  All four exact 4.14 graphs match the owned files byte-for-byte. No new original
  weight lineage grant was found; M12 remains blocked, with no execution or Ready.
- [ ] **C12 — Preserve declared HD metadata and require real reference and GPU-binding proof for symbolic output batch.**
  Owner selected the verified-output policy after real ORT inspection exposed the
  four HD graphs' symbolic output batch. Declared identity stays unchanged;
  preparation eligibility requires measured N=1/C=3 FP16 output at each requested
  geometry. Native activation still requires M16's real GPU binding proof.
  Catalogue core contracts have landed in NativeCatalog.hpp/NativeRecipe.cpp:
  `ModelRecord.verifiedReferenceProfile` (evidence, outside content identity),
  `spatialCandidateRefusal` (strict rules, output batch -1 admitted only) and
  `spatialReferenceRefusal` (strict concrete run boundary matching every stated
  declared dimension/dtype); the symbolic batch is offered only once such a run
  proves it. Inspector and inventory integration is implemented. Parent CLI proof
  on original HD Balanced exits 0 at 64×64, retaining declared N=-1 and recording
  actual FP16 N=1/C=3 and 128×128 output. The other three original HD graphs and
  SD also exit 0 at 96×64 with measured FP16 N=1/C=3 and 192×128 output; HD
  declarations remain N=-1. Catalog import/reload verification remains open
  under C13, and M16 GPU-binding proof remains unsatisfied.
- [ ] **C13 — Repair and verify first-use owned inventory trust and backend-preserving reload/planning.**
  The rebuilt isolated CPU matrix passes, including fresh-root ownership,
  explicit backend support reload/re-import, declared HD metadata and actual
  reference proof. Model manifests use `mikai.native.model/2`; an empty
  provider list remains empty, never guessed. Real asynchronous panel imports,
  negative graphs, exact owned copies and lease-safe deletion pass. Its
  screenshot exposes stale missing-root diagnostics, tracked by C18.
- [x] **C14 — Repair and verify bounded Processing viewport with visible Save/Cancel at minimum size.**
  Windows English settings and Ukrainian RIFE probes both retain exactly
  560×600 logical pixels at DPR 2; inspected PNGs show Save/Cancel outside the
  vertical viewport. Save/Cancel/reload, exact threshold and byte-identical
  credential-free `.env` preservation pass. C16/C17 separately track observed
  shortcut text and horizontal RIFE overflow; M38 is not complete.
- [x] **C15 — Resolve observed native CPU regression failures and rerun the full isolated matrix.**
  `ctest --test-dir build-native-baseline -R mikai_tests --output-on-failure`
  passes in 25.48 s with actual owned ORT/model references enabled and no live
  API key. The fresh inspection-off `build-native-plain` matrix passes in
  14.55 s. Availability gates, relative artifact paths, exact numeric JSON
  boundaries, first-use inventory and explicit provider metadata are covered.
  Validation fixtures each violate one boundary; incidental pins and the
  undocumented duplicate-content bundle restriction are removed. LiveOracle
  remains a deliberate skip, not a pass; native GPU acceptance is unaffected.
- [x] **C16 — Repair and verify fully visible shortcut bindings in styled minimum-size settings.**
  Verified against the current tree: `native_settings_probe` passes with the
  native field holding the longest binding, every character visible and the
  clear action inside the field at the styled minimum size, and the inspected
  screenshot shows the full `Ctrl+Shift+J` text. The width now derives from
  the styled input's own font metrics plus the clear action, never a text
  pixel constant.
- [x] **C17 — Repair and verify Ukrainian RIFE controls without horizontal panning at minimum size.**
  Verified against the current tree in both languages: `rife_settings_probe`
  passes with no horizontal scroll range and every control and role label
  inside the viewport at 560x600, and the inspected Ukrainian screenshot shows
  wrapped controls, the exact request stored, and Save/Cancel visible. The
  exact request id lives verbatim in a disclosure row. Five width votes were
  removed along the way (combo caption reservation, RIFE id label, table
  headers/sections, source-hint and page-hint labels, recipe combo); tables now
  scroll internally and the panel reports its real content floor.
- [x] **C18 — Repair and verify honest lazy inventory and post-import diagnostics.**
  Verified against the current tree with the licensed ORT runtime named
  (`MIKAI_ORT_RUNTIME_DIR=.../runtimes/win-x64/native`): `native_assets_probe`
  passes 44/44, including a fresh owned store reading empty with no
  missing-root diagnostic, real fp16 import with byte-identical owned copy and
  distinct content identities for same-basename graphs, fp32/two-input graphs
  recorded as unsupported with the observed dtype/count reason and no
  inventory write, mutation refresh clearing the pre-import diagnostic, and
  populated screenshots showing models with a blank status. Ownership and
  corrupt-manifest refusals are untouched. The one-line read-path change makes
  the empty-root invariant local instead of relying on a clearing side effect.
- [x] **C19 — Resolve actual standard-player interaction failures and late probe access violation.**
  Re-run against the current source with the runtime loaded: one real
  application defect, four probe defects and one harness defect, now separated.

  **Application defect (fixed).** `PlayerDialog::openStream` parsed its
  `quality` label back into a number, so every quality press stored
  `currentHeight_ = QStringLiteral("480p").toInt() == 0`. The popover's current
  tick disappeared and the requested height was lost. `openStream` now takes
  the height as an `int` and derives the label from it, so the two cannot
  disagree; a press that matches no offered stream is logged and shown in the
  status line instead of being a silent no-op.

  **Probe defects (fixed).** The dub/subtitle pickers were driven with a
  variant list carrying no `kind` - the exact field the dock's two buttons
  filter on - so both pickers looked broken; the synthetic list now carries
  kinds, and the quality list carries one variant per height. The drag check
  guessed the corner from the *pointer* quadrant, which is wrong on a work area
  narrower than twice the float (640x480 work area, 480x270 float): it now
  asserts the rule the viewer sees, that a released drag lands on a real corner
  inside the work area. The parked-float check read the float before
  `WA_DeleteOnClose` had deleted the player, so it waits for that deletion now.

  **Harness defect (fixed).** The last stage closes the last window, and Qt's
  default `quitOnLastWindowClosed` ended `app.exec()` with 0 before the runner
  printed its verdict - a run with failures exited as if it had passed. The
  probe disables that and prints its verdict unconditionally; the same run now
  reports `RESULT: FAIL (3)` and exits 1.

  **Closed.** `interaction_probe` passes 149/149 (`RESULT: PASS`, exit 0) on a
  rebuilt current tree with the session attached: quality presses land on the
  running stream, dub/subtitle pickers open from kind-carrying variants, the
  drag lands on a real corner, the resume/ownership ladder holds, and the Esc
  close destroys the parked float.
  Current contract: an episode swap from a paused player restarts at 00:00 and
  stays paused (`pauseAfterLoad_` in `openStream` + `applyPendingPause()` on the
  engine's report); a quality switch keeps carrying its position and pause
  (`resume` unchanged). `interaction_probe` pins this (swap checks + the
  quality-reload checks), four consecutive runs green.

  **App defect found by the plain rerun (fixed).** The episode-swap stage was
  non-deterministic: 1 run in 4 failed all three swap checks while the others
  passed, against unchanged source. `openStream(..., resume = false)` resets the
  position but a freshly loaded file always comes up playing, so whether a swap
  from a paused player stayed paused depended on the load/mp4 timing race.
  `openStream` now records `pauseAfterLoad_` for a position-reset swap and
  re-asserts the pause once the new stream reports itself; the quality switch
  keeps the documented carry semantics (`resume` unchanged). Observed after the
  fix: three consecutive `interaction_probe` runs each `RESULT: PASS`, 149 ok /
  0 FAIL, with `position after swap: 0.00s` from a ~7.5 s pre-swap position.
- [x] **C20 — Update and verify Compare probe against player-selectable preset contract.**
  Contract update landed in the probe source: the chooser now asserts every
  leaf is an `availableModes()` preset (no loose-`.glsl` expectations), the
  off id resolves through `modeId(Mode::Off)`, the loose restore candidate is
  catalogue-selected rather than hand-typed, chain-internal files derive from
  the catalog instead of a hard-coded name, and category paths resolve through
  `categoryLabel()` rather than a wording copy. Pixel proof observed on the
  attached session against the 1080p fixture: `RESULT: PASS`, 30/30, noise
  floor 0.000 without re-seeking, chain effect 0.132/255 above the floor.
  Rebuilt and rerun on the current tree (same binary freshness as the
  PlayerDialog/MainWindow edits): two consecutive runs against the 1080p
  fixture each `RESULT: PASS`, 30/30, recorded in `.probe-out/cmp-oct2.log`
  and `.probe-out/cmp-oct2b.log`.
- [ ] **C21 — Release and verify active and parked PiP pools with their owning dialog.**
  Source inspection found a real leak: the destructor deleted the active alias,
  not the parentless pool retained after restore. Teardown now clears aliases,
  hides both GL surfaces and releases the pool once.
  Observed in a real window against the current source: re-entering PiP reuses
  the parked float instead of building another, the restore parks rather than
  destroys, closing the player with the float parked destroys it, and the
  Esc-ladder close destroys the float the first player had parked. The native
  half of this task still needs a real active native recipe.
- [x] **C22 — Repair real file-picker proof and verify signed-HLS visible recovery.**
  Actual file-picker QA timed out and forcibly rejected the modal; its
  “accepted” flag recorded an attempt, not acceptance. The real chooser driver
  now requires chooser-closed plus a canonical path match, and a failed picker
  no longer blocks the later phases. `external_media_probe` passes every phase
  on a fresh loopback fixture with the session attached (`RESULT: PASS`,
  exit 0): picker accept with canonical path, local 320x180 colored pixels,
  standalone compare capture, HTTP 320x180 pixels, failed-URL retry chip with
  no host, retry recovery of the same URL, and file-URL reopen. Two defects
  fixed along the way: the first-pump spurious `playingChanged(true)` hid the
  retry chip the 404 had just raised (`onEnginePlaying` now requires real
  video before dismissing a media error), and fixture `retryServed_` was
  process-global (a `/media.mp4` fetch re-arms it; Qt DLLs deployed beside the
  fixture binary so fresh servers start). HLS half passes 23/23 with stale
  403, visible failure, cache forget, fresh extraction and a real 640x360
  colored framebuffer with the retry cleared (inspected recovery PNG).
