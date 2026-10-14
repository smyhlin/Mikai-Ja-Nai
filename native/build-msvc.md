# Building the native shim on MSVC (M04)

The native AnimeJaNai shim is a **second, optional toolchain**. The GUI
(`mikai.exe`) stays MinGW/Qt6 and never links the shim; it loads
`aji.dll`/`aji_trt.dll`/`aji_dml.dll` at run time through the mpv filter's
`lib=` and speaks only the C ABI in `include/aji.h`.

Nothing in this document changes the default build. With
`MIKAI_WITH_NATIVE=OFF` — the default — the root `CMakeLists.txt` never calls
`add_subdirectory(native)`, and no file under `native/` is configured, compiled
or hashed.

## What is built

| Target | Kind | Needs |
|---|---|---|
| `aji` | SHARED, the dispatcher the filter loads as `lib=` | staged sources only |
| `aji_trt` | SHARED, TensorRT/CUDA backend | `AJI_TRT_ROOT`, `AJI_NVINFER` |
| `aji_dml` | SHARED, DirectML backend | `AJI_ORT_ROOT` + `AJI_DML_ROOT` |
| `mikai_native_prep` | EXE, the out-of-band preparation helper CLI (M15) | `nvinfer_11`, `nvonnxparser_11`, `CUDA::cudart`, `bcrypt`, `version` |
| `mikai_native_verify` | EXE, the native numerical/binding verifier | `nvinfer_11`, `CUDA::cuda_driver`, `CUDA::cudart`, `bcrypt` |
| `aji_harness`, `aji_kernel_test` | EXE, the upstream self-checks | `CUDA::cudart` |

The three DLLs are the default target; the two CLIs and the two self-checks are
`EXCLUDE_FROM_ALL` and are built by name.

### The two CLIs are out-of-band, and separate from the Qt app

`mikai_native_prep` and `mikai_native_verify` are separate processes that are
never on a decoder, render or frame path. They build in **out-of-band** targets
that link the TensorRT and CUDA libraries they actually use, and neither links
`aji` or `aji_trt`. The Qt application is unaffected: it still imports only
`Qt6*`, `KERNEL32`, `msvcrt` and `SHELL32`, and takes no TensorRT, CUDA, ORT or
DirectML dependency. That separation is the point of the split, and it is what
keeps the default MinGW/Qt build free of the native SDK.

### Release is a configure-time requirement under Ninja

`-G Ninja` is single-config: `cmake --build … --config Release` is accepted and
**silently does nothing**, leaving a Debug cache, and the resulting helper fails
to start with `0xC0000135` (missing Debug CRT). With Ninja, configure with
`-DCMAKE_BUILD_TYPE=Release`. A Visual Studio (multi-config) generator is
unaffected: there `--config Release` selects the configuration, as above.

## Prerequisites

- Visual Studio 2022 Build Tools, MSVC x64. CMake must select an **MSVC**
  generator; nvcc on Windows rejects a MinGW host.
- CUDA Toolkit with `nvcc.exe`.
- TensorRT 11.3 (`include/NvInfer.h` + `lib/nvinfer_11.lib`). TensorRT 10 is
  **not** a dependency, and the build refuses to fall back to it silently.
- Optionally the DirectML ONNX Runtime and DirectML packages.

On this machine the SDK trees are acquired under `F:\ZKS\mikai-native-sdks\` and
are described in [`README.md`](./README.md).
`F:\ZKS\mikai-native-sdks\sdk-env.cmd` sets `INCLUDE`, `LIB`, `LIBPATH` and
`PATH` for one shell session only — no `setx`, no registry, no admin, no driver
change. Either source it or pass the roots on the command line as below; the two
routes are equivalent.

## Configure and build (standalone, the documented route)

```bat
cmake -S native -B native/build -G "Visual Studio 17 2022" ^
  -DAJI_SOURCE_DIR=C:/Users/myhli/Downloads/janai/animejanai-inference-main ^
  -DAJI_TRT_ROOT=F:/ZKS/mikai-native-sdks/tensorrt-11.3.0.99/TensorRT-11.3.0.99 ^
  -DAJI_CUDA_ROOT=F:/ZKS/mikai-native-sdks/cuda-13.4.2 ^
  -DAJI_NVINFER=nvinfer_11 ^
  -DAJI_ORT_ROOT=F:/ZKS/mikai-native-sdks/ort-1.24.4 ^
  -DAJI_DML_ROOT=F:/ZKS/mikai-native-sdks/directml-1.15.4

cmake --build native/build --config Release
cmake --build native/build --config Release --target mikai_native_prep aji_harness aji_kernel_test
```

`-G Ninja` works too from the same environment, and is what the acquisition
smoke used — but it must be configured with `-DCMAKE_BUILD_TYPE=Release`; see
*Release is a configure-time requirement under Ninja* above.

`CMAKE_CUDA_ARCHITECTURES` may be passed explicitly; when it is not, the pinned
list `75-real;80-real;86-real;89-real;90-real;100-real;120-real;120-virtual`
applies. It is set **before** `project()` on purpose: CMake freezes the
architecture list while probing nvcc, and the upstream fallback is guarded by
`if(NOT DEFINED ...)` while CMake pre-defines the variable — so that fallback
never fires and omitting the flag silently yields a single-architecture binary
that dies on every other GPU.

### Options

| Option | Default | Meaning |
|---|---|---|
| `AJI_SOURCE_DIR` | *(required)* | Read-only pinned `animejanai-inference` tree |
| `AJI_TRT_ROOT` | *(required for `aji_trt`)* | TensorRT root with `include/` and `lib/` |
| `AJI_NVINFER` | `nvinfer_11` | Import library to link; the lib must exist in `AJI_TRT_ROOT/lib` |
| `AJI_CUDA_ROOT` | `$CUDAToolkit_ROOT` | CUDA root |
| `AJI_ORT_ROOT`, `AJI_DML_ROOT` | *(unset)* | Enable `aji_dml` when both are set |
| `AJI_BUILD_TRT` / `AJI_BUILD_DML` / `AJI_BUILD_HARNESS` | `ON` | Target selection |
| `AJI_STAGE_DIR` | `<build>/aji-src` | Where the read-only tree is staged |
| `AJI_RESTAGE` | `OFF` | Force a fresh copy on this configure |
| `AJI_PATCH_POLICY` | `strict` | See *The patch series* below |
| `AJI_REPRODUCIBLE` | `ON` | Adds `/Brepro` and `/utf-8` to the DLL links |

## The read-only source tree is never edited in place

`AJI_SOURCE_DIR` is input, not a work tree. On configure the tree is copied to
`AJI_STAGE_DIR` inside the build directory, the patch series is applied **to
that copy**, and every target compiles the staged files. The copy is redone
automatically when the patch series, its contents or the source location
changes, and on demand with `-DAJI_RESTAGE=ON`. Two markers inside the staged
tree record which series it was produced from and whether that series has been
applied to it, so re-configuring an unchanged tree does not try to apply the
same patch twice.

## The patch series

`native/patches/` holds the series. Only the files directly in that directory
are globbed, sorted and applied to the staged shim; `patches/notes/` and
`patches/mpv/` are subdirectories and are not applied by this build.

### Applied: `patches/0001-content-addressed-cache-and-prepared-only.patch`

One applicable unified diff against `src/aji_trt.cpp`. Its recorded base is the
historical `738ce3b3020b3950c48ce3122a08f32af531d4fa` baseline; the current
build input is the official v0.9.0 tag commit `c7f56baf966064a7855db2f82a2f1b0d0c6cfdb3`
plus authorized mutable local edits (`matchesPinnedCommit: false`), so treat the
`738ce3…` identity as the historical diff base, not the current tree identity.
It:

- derives the engine cache key from an **FNV-1a token over the ONNX bytes**
  (plus the byte length) instead of the basename, and keeps the existing CRC32
  over the settings string alongside it;
- adds `AJI_ENGINE_CACHE` to redirect engine writes out of `model_dir`;
- replaces `clean_stale_engines` with a no-op, so the process no longer deletes
  foreign `.engine` files from a model root it does not own;
- adds an optional `AJI_PREPARED_ONLY` env refusal of a live engine build.

**This is not the full M14 content-addressed cache.** It is an FNV token plus
the settings CRC, not a SHA-256 `buildKey` integrated across the dispatcher,
the backend and the manifest, and no recorded build key is published anywhere.

### Applicability and union build observed: `patches/0002-native-snapshot-and-control.patch`

The additive C status ABI v2 diff applies after `0001`. History: an earlier
`strict` configure **refused** it (`error: patch failed: src/aji_dml.cpp:2718`)
and discarded the staged tree (`.probe-out/native-abi-v2-current-build.log`),
and a later one applied `0001`+`0002` but hit MSVC
`C1075: '{': no matching token found` on truncated function tails at
`src/aji_dispatch.cpp(382)`/`src/aji_trt.cpp(2487)`
(`.probe-out/native-abi-v2-after-m8-build.log`). The **current** observed state
is that the strict native SDK union build, after the complete-`0002`
revision/epoch assignments, **succeeded** (6.81 s) and compiled/linked
`aji.dll`, `aji_trt.dll`, `mikai_native_prep.exe`, `mikai_native_verify.exe`
and `native_abi_probe.exe`. The ABI v2 probes then ran exit `0`:

- default: upstream ABI 8 unchanged, protocol/schema `2/2`, no-context/
  protocol/malformed-size refusals report `Unavailable` with canaries untouched
  (`.probe-out/native-abi-v2-default-union.log`);
- real context: independent decoder UUID
  `GPU-f76e436f-c519-8f5a-95f7-ce9481b54b5b`, real resize-only `64x64 -> 96x96`,
  ticket 1 completed, `drain=0 suspend=0 resume=0`, post-resume real-frame
  completion succeeded, while the native state stayed `Unavailable`, `effective`
  empty and prepared keys empty despite a legacy-active shim
  (`.probe-out/native-abi-v2-real-context-union.log`).

This is ABI/protocol/diagnostic evidence only: no NN fidelity, admission,
`Prepared`/`Active` or completed-lifecycle inference is proved, and nothing is
`Prepared` or `Active`.

### Managed prepared plan: `patches/0003-managed-prepared-plan.patch`

A managed prepared-plan C ABI/filter bridge diff. The latest strict native SDK
union now applies `0001`+`0002`+`0003` and **succeeds** (exit `0`, 23.69 s,
parent session bg235), compiling/linking `aji.dll`, `aji_trt.dll`, a new
`native_prepared_runtime_probe`, `mikai_native_prep.exe` (new `Describe`/O5
flags), `mikai_native_verify.exe` and `native_abi_probe.exe`. A later odd-420
crop-kernel correction is **source-only/unbuilt**. It establishes no `Prepared`
or `Active` state and no inference result.

### Not applied: `patches/notes/`

`patches/notes/0001-add-get-meta-for-status.patch`,
`0002-content-addressed-engine-cache.patch` and
`0003-prepared-only-mode.patch` are the earlier **specifications**. Their hunk
headers are placeholders (`@@ -...`) that no applier can consume. They are kept
as the record of what the three requirements were; they are not a build input,
and `AJI_PATCH_POLICY=strict` is satisfied without them.

### Separate: `patches/mpv/0001-get-meta-mikai-native.patch`

The diff applies against pinned fork
`63d127174d8f70336c82a22408929e2bc301edcb` and implements the M19 **Probe**
producer in `video/filter/vf_animejanai.c`. `GET_META` publishes exactly four
typed tags: `schema`, `protocol`, `revision` and cached JSON `snapshot`.
Both metadata layers use protocol/schema 1, independently of the additive
AJI C ABI v2. Unconfigured passthrough reports `Probe`; an unverified legacy
configured shim reports `NeedsPreparation`, never verified `Prepared`/`Active`.
No effective plan or inference counters are fabricated. Decoder-frame device
UUID/SM or D3D11 adapter LUID come from the adopted hardware context.

The current owned fork compiled, and `native_status_probe` exercised actual
non-copy NVDEC **CUDA** frames: 320×180 NV12, 8-bit, 24 fps, observed
`f76e436f-c519-8f5a-95f7-ce9481b54b5b`, SM 12.0. It passed **14/14** checks:
named absence, typed Probe facts, frozen revision stability, seek/source-reload
epoch replacement and removal clearing metadata. Evidence:
`.probe-out/native-producer-m8-build.log` and
`.probe-out/native-producer-m8-runtime.log`. The fork patch's applicability
was checked with `git apply --check` against the pinned tree and observed to
exit `0` in the parent session; the recorded
`.probe-out/native-producer-m8-patch-applicability.log` is **intentionally
empty** because a successful check prints nothing — the empty file is not an
absence of the observation.
The current producer fork cadence union build compiles exit `0` with `av_d2q`
and observed rotation/crop/color facts, and the updated 17780-byte
`patches/mpv/0001-get-meta-mikai-native.patch` is exported against base
`63d127174d8f70336c82a22408929e2bc301edcb`
(`.probe-out/native-producer-cadence-union-build.log`); no cadence counters are
claimed.

This run used a private confined `deps/` closure and its compatible compiler
runtime beside the probe; no installed runtime or application default changed.
Media mode uses a real OpenGL renderer and `hwdec=nvdec`: `vo=null` cannot
establish this hardware-frame proof, and unsupported `hwdec=cuda` fell back to
software decoding plus hardware upload. The smoke checks actual decoded CUDA
frames, not merely a GPU UUID. The probe accepts a confined sibling `deps/`
directory without relaxing `MpvApi` search rules.

This CMake project still stages only the inference tree. The mpv patch is a
separate fork build input. Managed prepared-plan attachment, authoritative
effective/cadence snapshots and suspend/resume lifecycle remain open; this
Probe proof does not complete M19 or establish Native readiness.

A patch that *is* applicable but fails to apply is a hard error under `strict`,
and under `warn` the staged tree is discarded rather than left half-patched — a
partially applied series would fail later as wrong behaviour rather than as a
build error.

## The preparation helper CLI

`mikai_native_prep` is a separate process that is never on a decoder, render or
frame path. It builds a plan **in-process through the TensorRT builder API**: it
selects one named CUDA device, parses one actual ONNX graph with `NvOnnxParser`,
binds one fixed shape set per real network input, and moves the serialized bytes
onto `--out`. **It launches no other executable and falls back to none** — there
is no `--trtexec` flag and no child process.

```text
mikai_native_prep --onnx <graph> --out <NEW raw engine> ^
  --shapes <name:d0xd1xd2xd3[,name:...]> --device-uuid <CUDA UUID> [--workspace-bytes <uint64>]
```

The four required flags may each be given once; `--workspace-bytes` is optional
and sets the TensorRT builder workspace bound (default 1 GiB). `--shapes` entries
are rank-4, positive, uniquely named, and must match the graph's real network
inputs — a count or extent that disagrees with the declared input is refused. A
`--device-uuid` is the usual dashed 16-byte form; `-`, `{` and `}` are ignored
and a leading `GPU-` is accepted, but it must resolve to exactly 32 hex digits.
The invariants it holds:

- it enumerates the CUDA devices itself and refuses a `--device-uuid` that is
  not one of them, rather than falling back to another adapter;
- a `--out` path that already exists is refused, never overwritten;
- the plan is written to a reserved scratch file and moved onto `--out` only
  after a complete serialization; every failure path discards the scratch and
  leaves `--out` exactly as it was found;
- it emits one JSON event per line on stdout, each written only after the
  transition it names has actually happened, and no event claims a plan is
  verified, prepared or active.

Exit `0` means only that `--out` received a non-empty plan. Codes `2` (usage),
`3` (preflight), `4` (setup), `5` (build) and `6` (publish) each name their
cause and leave `--out` unchanged.

**Those bytes are raw.** The helper does not deserialise, verify or publish them.
A raw plan is not a `Prepared` plan and not an `Active` one: only the native
verification step and the artifact store may publish one.

The helper explicitly disables TensorRT's default TF32 allowance and reads the
builder configuration back before serialization. `build-started` and
`raw-produced` report the measured flags, network flags, optimization level,
tactic sources and engine capability. These facts must agree with the artifact
identity; a caller must not substitute nominal defaults.

Observed on the original RIFE lite graph: `tf32Allowed=false`,
`builderFlags=0`, `networkFlags=1`, `stronglyTyped=true`, optimization level `3`,
tactic sources `3`, and capability `STANDARD`. The new raw engine contains
8,316,436 bytes. Its real GPU comparison against the unchanged CPU tensors
passes with maximum absolute error `0.00988769531` and RMSE `0.000313335947`.
It remains `manifestVerified=false` / `prepared=false`; this does not establish
a prepared recipe or live native processing. Evidence:
`.probe-out/prep-measured-flags-lite.log` and
`.probe-out/prep-measured-flags-lite-verify.log`.

A private Describe path is now implemented, and the bug that made it emit two
missing `--onnx` (`.probe-out/native-installed-prep-describe.log`) was fixed in
`native/tools/mikai_native_prep.cpp` (parse common: if `describe`, return before
the required NN flags; a describe parse failure returns a named structured
schema); the SDK rebuild took 5.03 s. The private `0.1.0-dev.1` tool
`Describe --json --deviceUuid` exits `0`
(`.probe-out/native-installed-prep-describe-corrected.log`), proving the **tool
environment/factory closure** — not an engine, NN or playback.
The current `native_prep_probe` passes all seven behaviors, including the
precision gate and mid-build cancellation
(`.probe-out/prep-flags-contract-probe.log`). Against the preserved pre-change
helper, the same probe fails only the precision gate
(`.probe-out/prep-flags-contract-before.log`).

### Exercised scope

The current Release build of the native tools and of the Qt tree both succeeded.
`native_prep_probe` (a root-project Qt probe, `EXCLUDE_FROM_ALL`, Windows-only,
no interactive session needed) drives the real in-process helper:

```text
native_prep_probe <helper.exe> <graph.onnx> <GPU-uuid> <named-input-shapes>
```

It reports `RESULT: PASS` over **seven** behaviours: a pre-existing `--out`
refuses without changing its bytes; a real corrupt-graph parse failure exits `5`
and leaves no engine-shaped file; a missing GPU UUID refuses rather than
building on another adapter; the helper emits `build-started` before the build
proceeds; the TF32-off/strongly-typed precision gate holds; cancelling mid-build
stops it and publishes no engine; and failure and cancellation leave prior
artifacts and the source bytes unchanged. There is no child `trtexec` to observe
any more — the cancellation check is now the helper's own progress event.

A separate real 64×64 build on the selected RTX 5090
(`GPU-f76e436f-c519-8f5a-95f7-ce9481b54b5b`, driver file `32.0.16.1692`, CUDA
driver API `13040`, TensorRT `110300`, 1 GiB workspace, linear I/O) exited `0`
and wrote **1 380 548 bytes** of raw plan, with the source graph's digest
unchanged by that run. The digests are in `.probe-out/prep-api-current.log`; the
probe result is in `.probe-out/prep-api-probe-current.log`.

`native_inspect` on CPU exports a real 64×64 reference for the same graph — fp16
input `1x3x64x64`, output `1x3x128x128`, declared output `N=-1` retained — and
the export passes (`.probe-out/m16-cpu-reference-export-current.log`). Exporting
a reference whose output contains a NaN or an infinity is refused with
`reference.output.nonFinite`: before that check the export wrote infinities, and
after it the same run refuses (`.probe-out/nonfinite-export-before/after.log`).

### Corrected RGB frame reference layout

`frameToFp16Tensor` now packs P6 interleaved RGB into planar FP16 NCHW, rather
than declaring NCHW over interleaved bytes. An actual original SD graph CPU export
at 64×64 measured **9 786 of 12 288** input components wrong before the fix and
**zero** wrong afterwards; the remaining maximum error is FP16 normalization
rounding (`0.0002431832107843146`). The graph and source PPM SHA-256 stayed
unchanged. Evidence: `.probe-out/frame-layout-before-proof.json`,
`.probe-out/frame-layout-after-proof.json` and the corresponding CPU-export logs.

This proves frame packing, not GPU numerical fidelity. Historical spatial
CPU/GPU comparisons consumed the same malformed tensor on both sides; their
provider-difference measurements remain internally byte-consistent, but cannot
be used as correctly packed RGB-frame fidelity proof. New fidelity checks must
use the corrected reference. The engine verifier's `0.02` / `0.005` bounds are
unchanged.

### The verifier now accepts the spatial engine on the corrected reference

`mikai_native_verify` binds a real engine, enqueues on the real GPU and compares
against the CPU reference. It reports its numerics **even when it fails**, and
the metrics-failure JSON carries the real measured values on a failing run, so a
rejection is diagnosable rather than silent.

Against the **spatial HD Performance** engine the verifier now **passes** on the
corrected planar reference: the same unchanged engine (`sha256 65918177…`)
against the unchanged original graph (`sha256 7b804d87…`) with input
`sha256 a4c9ec0c…` and output reference `sha256 c2b7613a…` measured
`maxAbsoluteError` `0.00610351562`, RMSE `0.000591099329`, `49152` finite and `0`
outside tolerance, bounds unchanged
(`.probe-out/numerical-boundary-spatial-verify.log`, exit `0`). The earlier
`7 of 8` result (`maxAbsoluteError` `0.02734375`) was measured against the
malformed interleaved tensor and is **historical for this engine/input
boundary**.

Against the **RIFE matrix** — all four graphs run a real CPU reference (one
genuine 64×64 PPM pair, 5 451 differing samples, source 11-channel packing,
`t = 0.5`) and all four were serialized through the real builder API outside any
playback path — the verifier splits against the unchanged `0.02`/`0.005` bounds,
with no overrides: **lite passes** (`maxAbsoluteError` `0.0122070312`, RMSE
`0.000512728931`), **lite ensemble passes** (`0.00341796875`, `0.000227462237`),
**full is rejected** (`maxAbsoluteError` `0.0561523438`, RMSE `0.00198529105`),
and **ensemble is rejected** (exit `9`; `maxAbsoluteError` `0.0358886719`, RMSE
`0.00104451097`). Every graph is preserved at dtype FP16 with input
`1x11x64x64` and output `1x3x64x64`; that packing is grounded in
`kernels.cu:500-551` (`[RGB_A RGB_B, t = .5, meshX meshY mulW mulH]`).

A CPU **optimisation diagnostic** on the same original spatial graph and input
measured `DISABLE_ALL` against `ENABLE_ALL` at RMSE `8.5556432276e-06`, so
provider optimisation is **not** the cause of the spatial rejection. The GPU
engine still differs from the `DISABLE_ALL` reference — `maxAbsoluteError`
`0.02734375`, RMSE `0.00215002525` — so that engine is **rejected unchanged**.
**No quality threshold is widened anywhere in this document.**

A RIFE **pass** covers raw engine bytes, bindings and numerics only —
`manifestVerified: false`, `prepared: false` — so it is **not** `Ready` or
`Active`. The bounds are **not** to be widened to turn a rejection into a pass.
The working hypothesis for the rejections is cross-engine FP16 math — different
kernels between the CPU reference provider and the TensorRT engine — but that
diagnosis is **unresolved**. M12 is partially satisfied and **not done**: with
the spatial HD Performance engine also rejected, no complete prepared recipe and
no native processing is claimed.

### The isolated owned core now loads

The pinned fork core built here loads as an isolated module with **exit `0`**
once the stage closure matches the Qt/GNU cohort: client and header API both
`131077`, mpv `v0.41.0-UNKNOWN`, and swap refusal, transport, correlation,
`pending 0` and error-end-file all `RESULT: PASS`. The earlier `Win127` failure
is **unresolved as to the missing symbol** — completing the stage closure fixed
that scenario, but no particular missing export has been identified, and no
claim is made about which one it was. Verifying the bundle candidate still
**aborts on a `GGML` initializer** after the load/unload-all-helpers sweep, with
the fix in flight and no PASS yet.

A **Native-off GUI surface** through the owned runtime does now **pass**
(exit `0`) after full candidate hash validation: the Native-kind candidate is
explicitly selected as `0.41.0-native.1`, a real 320×180 HEVC 24 fps video plays
with `contextReady` true, position `0.542 → 2.542` over 12 s, **921600/921600**
non-black samples and **290264/921600** motion, **0** decoder and **0** VO
drops, and **0** live `vf` chain entries with Native off; the inspected
`mpv_surface_smoke.png` (1280×720) is a visibly colored motion test pattern.
`widget_pixel_probe` on the same candidate also passes **21/21** with 0 skips and
exit `0` — a colored 640×360 PNG, GL 4.6, 32 live GL-logger messages,
0 `ErrorType`, `glGetError` 0, 30.44% motion; late events after close remain
unknown, and the 256×144 15 fps reopen refreshes the metrics and the `vf`-absence
check.
That proves **real Qt/OpenGL through the owned native runtime with Native off**.
It is **not** the native filter, **not** CUDA interop, **not** active inference
and **not** task completion, and it is not a bundle install.

Not exercised, and therefore not claimed: a GPU work lease, a Qt preparation
runner that drives this helper, or any recipe publication from these bytes.

## Reproducibility check

M04's acceptance is two consecutive clean builds producing byte-identical
`aji.dll`/`aji_trt.dll`. MSVC stamps the linker with a build time unless
`/Brepro` is passed, which `AJI_REPRODUCIBLE=ON` adds:

```bat
cmake --build native/build --config Release --clean-first
copy /y native\build\Release\aji.dll %TEMP%\aji-1.dll
copy /y native\build\Release\aji_trt.dll %TEMP%\aji_trt-1.dll

cmake --build native/build --config Release --clean-first
certutil -hashfile %TEMP%\aji-1.dll SHA256
certutil -hashfile native\build\Release\aji.dll SHA256
certutil -hashfile %TEMP%\aji_trt-1.dll SHA256
certutil -hashfile native\build\Release\aji_trt.dll SHA256
```

Matching digests close the acceptance; a mismatch is a finding to record with
its cause, not a difference to explain away.

### Observed on this machine

Two Release clean builds **in the same build directory** produced an identical
`aji.dll` and an identical `aji_trt.dll` (`aji_trt.dll` SHA-256
`b2774da4…0ba910` both times, `-DCMAKE_CUDA_ARCHITECTURES=120-real`, over the
unpatched staged tree). That is the repeat-build half of M04's acceptance.

Two builds in **different** build directories produced an identical `aji.dll`
but a differing `aji_trt.dll`. The 112 differing byte ranges sit in the CUDA
kernel mangled names nvcc mints per translation unit
(`…_GLOBAL__N__941d9034_…` versus `…_GLOBAL__N__cc03391c_…`) and in the PE
timestamp `/Brepro` derives from the contents. The plain C/C++ `aji.dll`, which
has no nvcc input, is bit-identical either way. The build is therefore
reproducible for a fixed build path, which is what the acceptance asks for, and
the residual variance traces to nvcc's per-translation-unit naming rather than
to an unpinned input.

A later pair, with `0001-content-addressed-cache-and-prepared-only.patch`
applied under `AJI_PATCH_POLICY=strict`, was byte-identical across two clean
builds of the same directory:

- `aji.dll` `8e3b793dafa3805c83ed55f961c4084c84607cd23594a66cbfd7ce7517356d6a` (807936 bytes)
- `aji_trt.dll` `dee280de372a74b2b0000dc2d467eb0198ffb421d4c8824cafb5120eb8b29414` (2414080 bytes)

A different build directory embeds that absolute path, so those DLLs differ.
The mpv-fork `GET_META` patch is not part of these DLLs.

## Wiring it through the root project

`MIKAI_WITH_NATIVE=ON` calls `add_subdirectory(native)` from the root
`CMakeLists.txt`, which is only viable with an **MSVC** generator. Under the
default MinGW/Qt generator it stops at configure time with an explanation
rather than producing a mixed-toolchain tree:

```bat
cmake -S . -B build-native -G "Visual Studio 17 2022" -DMIKAI_WITH_NATIVE=ON ^
  -DCMAKE_PREFIX_PATH=<qt6-msvc> ^
  -DAJI_SOURCE_DIR=... -DAJI_TRT_ROOT=... -DAJI_NVINFER=nvinfer_11
```

The root application build obtains its mpv headers from the pinned developer
archive — `MIKAI_DIST_MPV_ARCHIVE` names an optional local copy, otherwise
CMake fetches the pinned release and verifies its SHA-256 — so no
`MPV_INCLUDE_DIR` (or any other mpv header variable) is passed. Its own
release-compiled objects use `-march=x86-64`, the SSE2 baseline, avoiding
`-march=native` and any newer baseline; that is a statement about these objects
only — the Qt/MinGW runtime, the packaged mpv module and its dependency closure,
and the supported Windows version remain separate prerequisites. The packaged
module additionally imports `vulkan-1.dll`, so the target machine needs the
Windows Vulkan loader in the system directory; the loader path reads the module's
PE import table before `LoadLibraryExW` and refuses the module by name when a
required system driver is absent — nothing is copied into the bundle, no
dependency is added and no substitute module is tried. This optional MSVC native
project builds a separate, project-patched Native core from its older base; it is
not the official API DLL packaged by the root app.
The standalone route is preferred: one toolchain per build directory.

## Effect on the MinGW/Qt build

None. The default configuration never enters this directory:

- `MIKAI_WITH_NATIVE` is `OFF` by default, so `add_subdirectory(native)` is not
  reached and no CUDA compiler is probed.
- The native targets (`aji`, `aji_trt`, `aji_dml`, `mikai_native_prep`,
  `aji_harness`, `aji_kernel_test`) are defined only inside `native/`, so the
  default build's target list, link line and binary are unchanged.
- `native/tools/bundle_tool.cpp` and `native/tools/manifest_check.cpp` belong to
  the **root** project and are unaffected by anything here; the native-only tool
  is `native/tools/mikai_native_prep.cpp`, which this project builds.
- Verified: configuring the root project with the default MinGW generator and no
  `MIKAI_WITH_NATIVE` produces a tree whose target list contains none of the
  native targets.

## Licensing note

This project's own native integration code is covered by the **owner grant of
2026-10-04** (unrestricted OpenSource use of this project's own integration and
its recorded decisions); that grant does not relicense third-party code. The
**third-party** shim source's upstream terms are **not verified**: its
repository declares no licence and its GitHub licence field is still `null`, and
no upstream grant was located. The project records its decision to use it
(`manifest/components.json#escalations[id=ESC-1]`), and a shipped notice must
not claim an upstream MIT licence. Nothing here changes that record, and the
build never fetches the source: it consumes a local checkout the operator
already has.