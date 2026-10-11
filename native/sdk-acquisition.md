# `native/sdk-acquisition.md` — native SDK acquisition record

Everything the optional MSVC + nvcc + TensorRT native build (M04) needs, obtained
**without an installer, without admin rights, without a driver change, and without
Python**. Machine-readable companions live in
`F:/ZKS/mikai-native-sdks/_meta/`.

**Nothing here is on the Qt/MinGW build path.** A machine without this directory
configures and builds the application unchanged.

> **Scope of this record — these are ACQUIRED.** It documents what was
> fetched, from which authoritative URL, at what size and digest, and what was
> extracted where. Every SDK here is **acquired**: present on disk,
> digest-verified, and laid out as its consumers expect.
>
> The boundary this record draws is *acquisition versus use*, not *acquisition
> versus configuration*. The SDKs have since been compiled against: the
> integration parent built the native project's own tools (`mikai_native_prep`
> and `mikai_native_verify`, Release) and produced real TensorRT plans on a
> GPU (spatial and all four RIFE graphs), and the pinned mpv fork core builds —
> its isolated core now **loads** (exit `0`) once the stage closure matches the
> Qt/GNU cohort, the original `Win127` missing symbol being unresolved. Two RIFE
> engines (lite and lite ensemble) verify numerically against the CPU reference,
> while the spatial HD Performance engine and RIFE full and ensemble are
> **rejected** on `maxAbsoluteError`; a verify pass is still `prepared: false`,
> so nothing is `Ready` or `Active`, and no native bundle is validated,
> installed or shipped. See
> [§6 What is still not established](#6-what-is-still-not-established).

## 1. Location

Root: **`F:\ZKS\mikai-native-sdks\`** — outside the repository, outside every
tracked source tree, so nothing here can be committed or accidentally vendored.
`F:` had 616 651 403 264 bytes (≈574 GiB) free at acquisition time.

**Licence position (2026-10-04):** The project's own integration is covered by the owner's unrestricted grant (recorded at `../LICENSE`, The Unlicense). NVIDIA SDK components (TensorRT/CUDA) are third-party and remain governed by NVIDIA's own licence terms; no relicensing is implied. The same holds for every other third-party component, including the upstream AnimeJaNai inference source, whose terms remain not verified (`manifest/components.json`). This record preserves all original digests, pins and provenance tables unchanged.

```
F:\ZKS\mikai-native-sdks\
  _downloads\    2.5 GiB  the original archives (re-verifiable source of truth)
  _meta\         NVIDIA's redistrib manifest, the plan JSON, all SHA-256 digests
  tensorrt-11.3.0.99\   2.3 GiB  61 files
  cuda-13.4.2\          1.6 GiB  2 495 files
  winsdk\               915 MiB  6 650 files
  ort-1.24.4\           ORT DirectML 1.24.4 (headers + win-x64 runtime)
  directml-1.15.4\      DirectML headers + win-x64 runtime
```

`_downloads/` is deliberately retained. Every digest below is re-checkable
against it at any time, and `_meta/artefacts.sha256.json` is the index.

## 2. TensorRT 11.3.0.99 — the pinned SDK

The exact artefact named in `native/manifest/sources.json`
(`sourceId: tensorrt-11.3-sdk`) was downloaded from the recorded URL.

| | |
|---|---|
| URL | `https://developer.nvidia.com/downloads/compute/machine-learning/tensorrt/11.3.0/zip/TensorRT-Enterprise-11.3.0.99-Windows-amd64-cuda-13.4-Release-external.zip` |
| Redirect | `302` → `https://developer.download.nvidia.com/.../TensorRT-Enterprise-11.3.0.99-Windows-amd64-cuda-13.4-Release-external.zip` |
| Response | `200`, `Content-Type: application/zip`, `Last-Modified: Fri, 04 Sep 2026 17:57:57 GMT` |
| Authentication | **none** — the whole chain is anonymous |
| Bytes | **1 985 279 435** (matches the recorded `Content-Length` exactly) |
| SHA-256 | `8ffd112d7bab97bc81eed818358b85df8c317bcdfc3cd3c731c220df7ca737eb` |
| Extracted to | `F:\ZKS\mikai-native-sdks\tensorrt-11.3.0.99\TensorRT-11.3.0.99\` |

**The SHA-256 is self-measured, not an upstream pin.** NVIDIA publishes no
`.sha256`, `.sha256sum` or `.md5` beside this asset — all three return `404`.
The digest is a *reproducibility baseline for this acquisition*, so a future
re-fetch can be compared against it; it is **not** evidence that the bytes
match some independently published value. This is the same status the manifest
already assigns to its other self-measured digests.

### Layout, verified against what `aji_trt` needs

- `include/` — `NvInfer.h`, `NvInferRuntime.h`, `NvInferRuntimeBase.h`,
  `NvInferPlugin.h`, `NvInferVersion.h`, `NvOnnxParser.h`, `NvOnnxConfig.h`, `impl/`.
- `lib/` — `nvinfer_11.lib`, `nvinfer_lean_11.lib`, `nvinfer_dispatch_11.lib`,
  `nvinfer_plugin_11.lib`, `nvinfer_vc_plugin_11.lib`, `nvonnxparser_11.lib`.
- `bin/` — the matching DLLs, `trtexec.exe`, and per-SM builder resources
  `nvinfer_builder_resource_sm{75,80,86,89,90,100,120}_11.dll` plus
  `..._ptx_11.dll`.
- `cmake/` — `TensorRT-EnterpriseConfig.cmake` and its targets file.
- `doc/Acknowledgements.txt` — **327 505 bytes, 5 717 lines**.

Three independent confirmations that this is the right pin:

1. `include/NvInferVersion.h` defines `TRT_MAJOR_ENTERPRISE 11`,
   `TRT_MINOR_ENTERPRISE 3`, `TRT_PATCH_ENTERPRISE 0`,
   `TRT_BUILD_ENTERPRISE 99` — the version is asserted **by the SDK itself**,
   not merely inferred from the filename.
2. The per-SM builder resource set is `75, 80, 86, 89, 90, 100, 120`, exactly
   the `CMAKE_CUDA_ARCHITECTURES` list pinned in `sources.json` and in the
   upstream `CMakeLists.txt`. The closure is self-contained on every target GPU.
3. `doc/Acknowledgements.txt` is **327 505 bytes, SHA-256
   `175f0eff1102f77a9c84973e5e0cd2b5284b63abd4db379108b4d303189e9844`** — measured
   here, and identical to the `TensorRT_Acknowledgements.txt` in the reference
   package at that same digest. Byte-identity is therefore *measured*, not inferred
   from matching size and line count; it corroborates package identity from an
   independent copy.

`AJI_NVINFER` must name `nvinfer_11` — `nvinfer_11.lib` is what exists in this
TensorRT 11.3 tree, and `native/CMakeLists.txt` defaults the cache variable to
`nvinfer_11` and refuses to fall back to TensorRT 10 silently; the configure
check requires `${AJI_TRT_ROOT}/lib/${AJI_NVINFER}.lib` to exist.

`trtexec.exe` **was** extracted, because it ships in this package. That does
not resolve **`ESC-5`**: `trtexec.exe` is a developer tool and its
redistributability is unverified. It is a build-time convenience only and must
stay out of any shipped bundle. The project's private prepared helper avoids
invoking it (in-process builder API), which resolves ESC-5 for **that path
only**; the official upstream-config first-build path still requires and
receives `trtexec.exe`, and no NVIDIA developer-tool clearance has been
obtained, so `components.json` records that path as unresolved. (`ESC-6` is a
different finding — the mpv fork's GPL/LGPL obligations — and has nothing to do
with `trtexec`.) See `native/manifest/components.json#ESC-5`.

### Terms

TensorRT SLA (v. May 24, 2021) + TensorRT Supplement (v. February 25, 2021).
Supplement §2 makes the **runtime `.dll` files** distributable, subject to
SLA §1.2.1.x — material additional functionality in the application, the
redistributable portions accessed only by the application, the notice *"This
software contains source code provided by NVIDIA Corporation."* accompanying
the distribution, and the SDK not thereby becoming subject to an open-source
licence (SLA §1.2.5).

For **private local development** — which is the only use here — none of these
distribution conditions are engaged. Any future *shipping* decision remains
`components.json`'s to make, and `Acknowledgements.txt` must then travel verbatim
with the bundle. Reference package extraction for development is within the SLA.

## 3. CUDA 13.4 — flavour pinned, never inferred

The manifests' rule is explicit: *the CUDA minor is part of the TensorRT
artefact name and is not derivable from the TensorRT version.* `13.4` was taken
from the pinned artefact name and then confirmed upstream.

**Version resolution was taken from NVIDIA's own machine-readable manifest, not
inferred from the TensorRT version or from any local file:**

- Manifest: `https://developer.download.nvidia.com/compute/cuda/redist/redistrib_13.4.2.json`
  (`release_label: "13.4.2"`, `release_date: "2026-09-16"`, 39 components).
- Toolkit **13.4.2** is the current CUDA 13.4 release per NVIDIA's archive
  listing; Windows component set is **13.4.92**.

Each component's exact archive name, SHA-256 and byte size was read from that
manifest into `_meta/cuda-13.4.2-windows-plan.json`, and every download was
verified against it.

### Packages obtained (14 of 32 Windows components)

All `windows-x86_64`, all SHA-256 **verified against NVIDIA's manifest — 14/14
match, 0 size mismatches**.

| Component | Version | Bytes | SHA-256 (prefix) |
|---|---|---|---|
| `cuda_nvcc` | 13.4.92 | 32 991 192 | `23950047aa540599…` |
| `cuda_crt` | 13.4.92 | 166 601 | `a38e3aa75e259128…` |
| `cuda_cudart` | 13.4.92 | 2 737 397 | `621a70c4778287b1…` |
| `cccl` | 13.3.4.3.1 | 4 034 377 | `17d0ca1592c0dfc9…` |
| `cuda_profiler_api` | 13.4.92 | 21 250 | `3417fa26b4471c53…` |
| `cuda_nvml_dev` | 13.4.92 | 189 442 | `99a96d9c79c75d1a…` |
| `cuda_nvdisasm` | 13.4.92 | 5 455 076 | `42a08d81ea1b70ff…` |
| `cuda_nvprune` | 13.4.92 | 10 916 602 | `a3683209925c1da2…` |
| `cuda_cuobjdump` | 13.4.92 | 6 522 410 | `ce9b95893719e0b0…` |
| `cuda_nvtx` | 13.4.92 | 153 681 | `a3bd5041778b63a9…` |
| `libnvfatbin` | 13.4.92 | 2 342 203 | `2b70bd761095296e…` |
| `libnvjitlink` | 13.4.92 | 281 015 727 | `7cef0ee32d2221d0…` |
| `libnvptxcompiler` | 13.4.92 | 31 091 629 | `1e9d858c2f16e18b…` |
| `libnvvm` | 13.4.92 | 60 031 433 | `64fa39ad6332d6c4…` |

Selection is driven by the link line in the pinned upstream `CMakeLists.txt`
(`target_link_libraries(aji_trt PRIVATE ${AJI_NVINFER} CUDA::cuda_driver
CUDA::cudart)`, `find_package(CUDAToolkit REQUIRED)`), not by convenience.
`libnvjitlink` is the 268 MiB line item and is not optional: nvcc needs it to
link for `sm_100`/`sm_120`, two of the pinned architectures.

**`cuda_nvrtc` was deliberately omitted** (would have been 305 150 000+ bytes).
NVRTC is *runtime* source compilation; `kernels.cu` is compiled ahead of time
by nvcc, and nothing in the pinned source calls `nvrtcCompileProgram`.
`sources.json` already records that the kernels use only
`cudaMalloc`/`cudaMemcpy` and the driver API. Recording the omission is
preferable to silently shipping 305 MiB of unused runtime.

### Merged tree

`F:\ZKS\mikai-native-sdks\cuda-13.4.2\`, merged from the per-component archives
into the layout CMake and nvcc expect:

```
bin/    nvcc.exe, cudafe++.exe, ptxas.exe, nvlink.exe, fatbinary.exe,
        cuobjdump.exe, nvdisasm.exe, nvprune.exe, bin2c.exe, nvcc.profile,
        crt/, x64/cudart64_13.dll
include/  cuda.h, cuda_runtime.h, cuda_runtime_api.h, cuda_fp16.h, crt/, cccl/, nv/
lib/x64/  cudart.lib, cuda.lib, cudadevrt.lib
nvvm/     bin/, include/, lib/x64/nvvm.lib, libdevice/
LICENSE-CUDA-EULA.txt   68 070 bytes  (NVIDIA CUDA Toolkit EULA)
LICENSE-CCCL.txt        22 452 bytes  (Thrust/libcu++ Apache-2.0 + LLVM exceptions)
```

Both licence files are kept under distinct names. A naive merge silently loses
one, because **every** NVIDIA component and cccl each ship a top-level file
called `LICENSE` — different documents, same name. Losing the CUDA EULA to
cccl's Apache-2.0 text would have been a rights-record defect, not a cosmetic
one.

### Verified, by execution

```
$ nvcc.exe --version
nvcc: NVIDIA (R) Cuda compiler driver
Cuda compilation tools, release 13.4, V13.4.92
Build cuda_13.4.r13.4/compiler.38855100_0
```

This is the first and only executable run recorded here. It confirms the
compiler loads and reports the pinned flavour. It is **not** a compile test.

### Host-compiler compatibility — resolved by build (§4b)

`sources.json` pins `-DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler`, recorded
because "CUDA 13.x nvcc does not recognise recent MSVC toolsets as supported
host compilers", and upstream needed it on a VS 18 / 14.51.36231 machine.

For **this** pairing the flag appears unnecessary, on the reading of the header's
`#error` guard. At the time of acquisition no `.cu` file had been compiled, so
this was a hypothesis, not a proof. **[§4b](#4b-observed-configuration-record-integration-parent-2026-10-01)
subsequently confirmed it by build** — the reasoning below is preserved as it
stood before that evidence:

- `cuda-13.4.2/include/crt/host_config.h:162` gates on
  `#if _MSC_VER < 1920 || _MSC_VER >= 1960` → *"Only the versions between 2019
  and 2026 (inclusive) are supported"*.
- MSVC 14.44.35207 is `_MSC_VER 1944`, **inside** `[1920, 1960)`.

That preprocessor guard therefore does not fire, so CUDA 13.4.92 accepts MSVC
19.44 as a *supported* host compiler. **This was an inference from the header's `#error` condition.** It is
now **confirmed by an observed compile and link** (§4b), which succeeded across
all pinned architectures with the flag absent from the generated build rules.
The pin in `sources.json` still carries the flag and remains authoritative until
the parent re-evaluates it.

### Terms

NVIDIA CUDA Toolkit EULA (`LICENSE-CUDA-EULA.txt`). For private local
development the SDK is used under the EULA's development grant; the CUDA EULA at
§1.1.6 permits use to develop and test applications released under OSI-approved
open-source licences. Redistribution of `cudart64_13.dll` is governed by the CUDA
Supplement §2.6 Attachment A, which lists `cudart.dll`, `cudart_static.lib`,
`cudadevrt.lib` and expressly permits *"certain variations of these files that
have version number or architecture specific information embedded in the file
name"* — `cudart64_13.dll` being exactly such a variation. `components.json`
already records this as `runtime.cuda-cudart`, decision `external`.

## 4. Windows SDK 10.0.26100.8249 — obtained without admin

This was the one genuinely missing piece: `C:\Program Files (x86)\Windows Kits\10`
had only `Catalogs`, `Redist/ucrt/DLLs` and `UnionMetadata/Facade` — no
`Include/`, no `Lib/`, hence no `kernel32.lib`, which is what an MSVC link
needs.

It was resolved through **NuGet**, which requires no installer and no elevation.
All three packages were downloaded anonymously from `api.nuget.org`.

| Package | Bytes | SHA-256 |
|---|---|---|
| `Microsoft.Windows.SDK.BuildTools.10.0.26100.8249` | 22 566 469 | `1628c77d21ed187c4db998b37b18e267a7f092ae755589e21110c14260b14960` |
| `Microsoft.Windows.SDK.CPP.10.0.26100.8249` | 160 432 822 | `f8787b2f6678164ae789bdca6247e696c2a0f529a39ceb969d6ef3d69a987131` |
| `Microsoft.Windows.SDK.CPP.x64.10.0.26100.8249` | 52 951 912 | `dba411916afdd47c99fecdf7d5558df7679d291dc338746590ae9adf5fd0511b` |

`Microsoft.Windows.SDK.BuildTools` alone is **not** sufficient — it contains only
packaging/deployment tooling (192 files: `PackageEditor.exe`, `DeployUtil.exe`,
signing manifests). It has no `windows.h` and no `kernel32.lib`. The C++
headers and import libraries are in `Microsoft.Windows.SDK.CPP` and its `.x64`
companion. Both were needed; the first download alone would have looked like
success while supplying nothing a link could use.

Note these digests are also **self-measured** — NuGet signs its packages, and
the extracted trees carry `.signature.p7s`, but no upstream SHA-256 is published
on the flat-container API for comparison here.

### Extracted to `F:\ZKS\mikai-native-sdks\winsdk\`

Headers are version-nested: `c\Include\10.0.26100.0\{um,shared,ucrt,cppwinrt,winrt}`.
A CMake/MSVC configuration must point at that version directory, **not** at
`c\Include` — pointing one level too high finds nothing.

Verified present:

- `c\Include\10.0.26100.0\um\windows.h` — **1 515** `um` headers
- `c\Include\10.0.26100.0\shared\` — **241** headers, incl. `ntdef.h`
- `c\Include\10.0.26100.0\ucrt\` — **61** headers, incl. `corecrt.h`, `stdlib.h`,
  `io.h`, `process.h`
- 3 633 headers total under `c\Include\`
- `c\um\x64\` — **453** import libraries, incl. `kernel32.lib`, `user32.lib`
- `c\ucrt\x64\ucrt.lib`
- `c\bin\10.0.26100.0\{x64,x86,arm64}\`

`process.h` is in `ucrt/`, not `um/` — that is correct for this SDK generation
and is not a missing file.

### Terms

Microsoft Visual Studio licence; **build-time only, not redistributable**. The
Windows SDK must never enter a shipped bundle. That is consistent with
`sources.json#msvc-toolchain`, which already records it as not redistributable.

## 4a. ONNX Runtime 1.24.4 (DirectML) and DirectML 1.15.4

Both acquired as NuGet packages from `api.nuget.org`, no installer, no
elevation, same isolated root. These are the run-time engines behind
`aji_dml.dll`; the ORT C API headers let M11 be built and unit-tested
**without** the M05 native bundle and without the unresolved `ESC-1` rights
grant, because nothing here touches the unlicensed inference shim.

| Package | Bytes | SHA-256 |
|---|---|---|
| `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 | 12 458 649 | `57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681` |
| `Microsoft.AI.DirectML` 1.15.4 | 202 292 617 | `4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9` |

**The DirectML digest and byte count reproduce the values already recorded in
`sources.json#sources[id=directml]` and `components.json#ESC-4`, exactly.**
That is an independent confirmation of provenance: this download is the same
artefact the earlier provenance research examined, not a look-alike.

### Layout — verified, with one correction to the manifest

`sources.json#requiredLayout` names `onnxruntime.dll.lib` and
`DirectML.dll.lib`. **Both packages actually ship `.lib`, not `.dll.lib`.**
Anyone pointing CMake at the recorded filenames will not find them. Measured:

| | Path | Bytes | SHA-256 |
|---|---|---|---|
| ORT headers | `ort-1.24.4\build\native\include\` | 13 headers | — |
| ORT import lib | `ort-1.24.4\runtimes\win-x64\native\onnxruntime.lib` | 2 834 | `281d55c95107ad75…` |
| ORT runtime | `ort-1.24.4\runtimes\win-x64\native\onnxruntime.dll` | 17 328 152 | `e7eedec6a6f26dc3…` |
| ORT EP shared | `…\onnxruntime_providers_shared.dll` | 22 040 | `265c8daf29637cb2…` |
| DML headers | `directml-1.15.4\include\` | `DirectML.h`, `DirectMLConfig.h` | — |
| DML import lib | `directml-1.15.4\bin\x64-win\DirectML.lib` | 1 956 | `36ec253c692a97a9…` |
| DML runtime | `directml-1.15.4\bin\x64-win\DirectML.dll` | 18 527 776 | `9c9e6d822561c6c4…` |

Both runtime DLLs are **byte-identical to the pins already recorded** from the
reference package: `onnxruntime.dll` at `e7eedec6…`/17 328 152 and
`DirectML.dll` at `9c9e6d82…`/18 527 776. The NuGet artefacts are therefore
confirmed as the provenance of the reference package's copies, and nothing was
borrowed from the reference package, the Conda environment, or any other tree.

`onnxruntime_c_api.h` declares `#define ORT_API_VERSION 24`, matching
`sources.json`'s `ORT_API_VERSION 24` requirement. The package provides both
`cpu_provider_factory.h` (CPU execution provider) and `dml_provider_factory.h`
(`OrtDmlApi` with `OrtSessionOptionsAppendExecutionProvider_DML`), so the
CPU-capable DirectML-flavoured provider is covered. The nuspec declares
`Microsoft.AI.DirectML` `1.15.4` as a dependency, matching `sources.json`'s
`declaredDependency` exactly.

Also verified present for `aji_dml`'s link line
(`d3d12 d3d11 dxgi d3dcompiler`): `d3d12.lib`, `d3d11.lib`, `dxgi.lib`,
`dxguid.lib` and `d3dcompiler.lib` are all in
`winsdk\c\um\x64\`. `d3dcompiler_47.lib` does **not** exist — the correct
name is `d3dcompiler.lib`; `d3dcompiler_47.dll` (the runtime) ships in
`winsdk\c\bin\10.0.26100.0\x64\`.

### Terms

- **ONNX Runtime** — **MIT**, `Copyright (c) Microsoft Corporation`
  (`ort-1.24.4/LICENSE`, and `ThirdPartyNotices.txt`). Cleared for both use
  and redistribution; already recorded in `components.json`.
- **DirectML** — **two licences, both reproduced at their manifest digests**:
  `LICENSE.txt` (10 439 bytes, `a05138e3a085ff60…`) governs the binary as
  Microsoft Software License Terms and grants distribution at §1(a) for
  applications running on Windows/Xbox; `LICENSE-CODE.txt` (1 093 bytes,
  `903df5512f7d0260…`) is MIT and governs the code. This is `ESC-4`, resolved.
  The redistribution grant is conditioned on Windows/Xbox and on not
  stand-alone redistribution — both satisfied by embedding, which is how a
  bundle would use it.

Nothing from the reference package or the `animeinterp314` Conda environment
was copied. These are the official NuGet artefacts, independently downloaded
and digest-verified.

## 4b. Observed configuration record (integration parent, 2026-10-01)

**Scope:** toolchain only. This does not move M04/M05 or any runtime gate.

The integration parent configured and built a **standalone CMake consumer
project** under this same isolated root, then ran it. This section records what
that build and run observed. It is deliberately narrow about what it proves.

Every entry below is either **quoted from the parent's report** or **read back
from the generated build tree** (`CMakeCache.txt`, `build.ninja`) and the
compiled binary. Nothing here was compiled, linked, executed or syntax-checked
by the acquisition task, which performed no build of any kind.

**Environment:** `F:\ZKS\mikai-native-sdks\sdk-env.cmd` — a scoped,
process-local entry point. It sets `INCLUDE`, `LIB`, `LIBPATH` and `PATH`
for the session only. **No `setx`, no registry write, no global environment
change, no admin, no driver or runtime replacement.** It is retained as a
reproducible toolchain entry point; `_compile-smoke\` is throwaway.

### Observed toolchain

| | Observed |
|---|---|
| Generator | **Ninja** (VS-bundled) |
| C++ compiler | `...\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe` — CMake reports **19.44.35222.0** |
| CUDA compiler | `F:/ZKS/mikai-native-sdks/cuda-13.4.2/bin/nvcc.exe` — CMake reports **NVIDIA 13.4.92** |
| Project languages | `project(mikai_sdk_smoke LANGUAGES CXX CUDA)` |
| Architectures | `75-real;80-real;86-real;89-real;90-real;100-real;120-real;120-virtual` |

All eight `--generate-code` variants were emitted, as recovered from the
generated build rules:
`compute_75/sm_75`, `80/sm_80`, `86/sm_86`, `89/sm_89`, `90/sm_90`,
`100/sm_100`, and `120` **both** `sm_120` and `compute_120` (virtual/PTX).

### Observed result

- **Compile and link succeeded for every architecture**, with **no
  `-allow-unsupported-compiler`**. The flag appears **zero** times in the
  generated build rules, so its absence is not merely an omitted argument — nvcc
  accepted MSVC 19.44.35222.0 as a supported host compiler. **This closes the
  question in §3 and §6.**
- Link closure resolved from the isolated trees:
  `cudart.lib`, `cuda.lib`, `cudadevrt.lib`, `nvinfer_11.lib`, and
  `kernel32.lib`/`user32.lib`/`gdi32.lib`/`advapi32.lib` from the
  **NuGet-supplied Windows SDK**. `kernel32.lib` resolving at all is the
  concrete proof that the SDK gap §4 set out to close is closed.
- Runtime smoke, observed verbatim:
  `SDK smoke PASS: Win32 linked, TensorRT110300, GPU NVIDIA GeForce RTX5090 SM12.0, CUDA kernel values 42 43 44 45`

  `110300` is the runtime/header `getInferLibVersion()` agreeing with
  `NV_TENSORRT_VERSION`, and `42 43 44 45` are the values a real CUDA kernel
  wrote to device memory and read back — so this is a genuine device round-trip,
  not a compile-only result.

### What this does and does not prove

**Proves:** SDK discovery and configuration; nvcc driving `cl.exe`; successful
compile **and** link across all pinned architectures; `nvinfer_11` linking;
CUDA **driver** and kernel execution on this GPU; `libnvjitlink` adequacy for
`sm_100`/`sm_120`.

**Does NOT prove:** anything about the native project. No `kernels.cu`,
`aji_trt`, `aji_dml` or any ONNX inference was compiled or run. No model was
loaded, no engine built, no TensorRT builder API exercised, and no DirectML or
ORT provider was invoked at run time. **This is a toolchain smoke, not a
capability result**, and it must not be read as moving M04, M05 or any runtime
gate.

## 5. The environment, assembled

Nothing was installed into `Program Files`, `Windows Kits`, or
`C:\Program Files\NVIDIA GPU Computing Toolkit`. The pre-existing **CUDA v13.2
stub** was left exactly as found and is *not* referenced. The ambient Conda
TensorRT in `animeinterp314` remains research residue and is **not** a
dependency.

To configure M04, the three roots are:

| Purpose | Value |
|---|---|
| TensorRT | `F:\ZKS\mikai-native-sdks\tensorrt-11.3.0.99\TensorRT-11.3.0.99` |
| CUDA | `F:\ZKS\mikai-native-sdks\cuda-13.4.2` |
| Windows SDK headers | `F:\ZKS\mikai-native-sdks\winsdk\c\Include\10.0.26100.0` |
| Windows SDK libs | `F:\ZKS\mikai-native-sdks\winsdk\c` (`um\x64`, `ucrt\x64`) |
| MSVC (pre-existing) | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207` |
| ORT headers | `F:\ZKS\mikai-native-sdks\ort-1.24.4\build\native\include` |
| ORT native | `F:\ZKS\mikai-native-sdks\ort-1.24.4\runtimes\win-x64\native` |
| DirectML headers | `F:\ZKS\mikai-native-sdks\directml-1.15.4\include` |
| DirectML native | `F:\ZKS\mikai-native-sdks\directml-1.15.4\bin\x64-win` |

`CUDAToolkit_ROOT` → the CUDA root; `AJI_TRT_ROOT` → the TensorRT root;
`-DAJI_NVINFER=nvinfer_11`. `CMAKE_CUDA_ARCHITECTURES` must always be passed
**explicitly** — the upstream fallback is guarded by
`if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)`, and CMake pre-defines that variable
during `project()`, so the fallback never fires and omitting it silently yields a
single-architecture binary.

The MSVC side is already complete without this task's work: `cl.exe` present,
76 x64 libraries including `libcmt.lib`/`libcmtd.lib`, STL headers present, VC
redist `14.44.35112` present. **The Windows SDK was the only genuinely missing
piece, and it is now closed.**

## 6. What is still not established

Stated plainly, because a download is not a configuration:

- **The native project itself is still unbuilt.** No `kernels.cu`, `aji_trt`,
  `aji_dml`, or any ONNX inference was compiled or run. The §4b smoke is a
  standalone toolchain consumer: it proves the SDK works, not that this project's
  targets build. **No native capability, model load, or engine build is claimed.**
- **`CMAKE_CUDA_ARCHITECTURES` coverage is now proven** (§4b): all eight
  `--generate-code` variants emitted and built, including `sm_120` SASS and
  `compute_120` PTX, and a device round-trip on SM 12.0.
- **`-allow-unsupported-compiler`: RESOLVED — not needed.** §4b records an
  observed all-architecture compile and link that succeeded with the flag absent
  from the generated build rules entirely. The §3 header reading is now confirmed
  by a real build. `sources.json` still carries the flag; re-evaluating that pin is
  the parent's call, not this record's.
- **The Windows SDK nupkg route is non-canonical.** It is a valid, unsigned-
  installer-free copy of SDK 26100.8249, but it is *not* what the VS installer
  would place under `Program Files (x86)\Windows Kits\10`, and it does not
  register with `vswhere`. Any tooling that discovers the SDK through the
  installer rather than through explicit paths will not see it.
- **No driver or runtime was installed or replaced.** The CUDA *driver*
  requirement for TensorRT 11.3 on this GPU is unexamined.
- **The inference-source copyright grant is still outstanding.** Nothing here
  touches it: these are build-time tools. The upstream repository still declares
  no licence and its licence field is still `null`, no upstream grant has been
  obtained, and ESC-1 remains **open**; `manifest/components.json` records the
  upstream terms as "upstream terms not verified". The owner's 2026-10-04 grant
  covers this project's own integration code only and does not supply the
  upstream grant. `sources.json#patchSeries` is still `PENDING`.
- **Redistribution positions are unchanged.** TensorRT/CUDA remain `external`,
  and `trtexec.exe` is not shipped: the private prepared helper builds in-process
  and never invokes it, so **ESC-5 is avoided by architecture on that path only**
  rather than by permission. The official upstream-config first-build path still
  invokes `trtexec.exe` and its developer-tool redistribution question remains
  unresolved in `components.json`.
  The CC BY-NC-SA spatial graphs (`ESC-2`) are usable locally without any
  ShareAlike condition attaching, but redistribution of those five files is
  **not cleared** under the recorded scope: the §3 Attribution/ShareAlike
  conditions remain, and no rights-owner decision on a shipped bundle's
  NonCommercial status is recorded.

### If a canonical installed SDK is wanted instead

Would require **elevation**, and is therefore out of scope for this task —
recorded for the owner, never executed:

- **Component ID** `Microsoft.VisualStudio.Component.Windows11SDK.26100`
  (VS 2022 Build Tools channel `VisualStudio.17.Release`; this machine is
  `17.14.26+36930.0`).
- **Command** (requires an elevated prompt):
  `vs_BuildTools.exe --quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Component.Windows11SDK.26100`
- **Why it needs elevation**: it writes to `Program Files (x86)\Windows Kits\10`,
  registers the SDK with the VS installer, and may require a reboot. It is also
  the route that could disturb the existing BuildTools installation.

The current no-admin route is preferred precisely because it is isolated and
reversible: deleting `F:\ZKS\mikai-native-sdks\` leaves the machine as it was.

## 7. Re-verifying

```
# TensorRT: 1 985 279 435 bytes, digest self-measured, no upstream pin exists
certutil -hashfile _downloads\TensorRT-Enterprise-11.3.0.99-Windows-amd64-cuda-13.4-Release-external.zip SHA256

# CUDA: 14/14 verified against NVIDIA's own manifest
curl -sO https://developer.download.nvidia.com/compute/cuda/redist/redistrib_13.4.2.json
# compare each sha256 in _meta/cuda-13.4.2-windows-plan.json to the archive

# Windows SDK / ORT / DirectML: self-measured; NuGet-signed (.signature.p7s).
# The DirectML 1.15.4 nupkg digest reproduces the value already recorded in
# native/manifest, as do onnxruntime.dll and DirectML.dll inside these packages.
```

`_meta/artefacts.sha256.json` indexes all **20** downloaded archives with byte
size and SHA-256. The 18 CUDA/TensorRT/Windows-SDK digests were each computed
twice in independent passes and reproduced identically; the two added later
(ORT, DirectML) were verified against the digests already recorded in
`native/manifest` instead.

## 8. Relationship to the shared files

Per the task boundary this record **does not modify** `plan.md`, `tasks.md`,
`native/manifest/*`, or the root `CMakeLists.txt`. It is a new, standalone
document. Three findings above are candidates for the integration parent to
weigh, and are deliberately *not* applied here:

1. `-DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler` is now **observed
   unnecessary**: §4b's all-architecture compile and link succeeded with the flag
   absent from the generated rules. The pin may be dropped, subject to the
   parent's review.
2. `sources.json#acquisition.knownHostCompilerCaveat` describes a constraint that
   the pinned flavour no longer imposes on this machine.
3. `sources.json#acquisition.windowsNetworkInstaller` names
   `cuda_13.3.0_windows_network.exe`. The pinned artefact flavour is **13.4**;
   if a full toolkit install is ever approved, the version to install is
   **13.4.2**, not 13.3.
4. `sources.json` `requiredLayout` for both ORT and DirectML names `*.dll.lib`,
   but the packages ship plain `.lib` (`onnxruntime.lib`, `DirectML.lib`).
   Measured in §4a; a CMake path using the recorded names will not resolve.
5. `aji_dml`'s link line names `d3dcompiler`; the SDK ships the import library
   as `d3dcompiler.lib` (not `d3dcompiler_47.lib`). Runtime `d3dcompiler_47.dll`
   is in `winsdk\c\bin\10.0.26100.0\x64\`.
