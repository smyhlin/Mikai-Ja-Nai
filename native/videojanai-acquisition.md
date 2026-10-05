# VideoJaNai 2.1.0 component acquisition

Evidence only. No component was installed, added to a runtime bundle, or used to replace a driver or SDK. The pack's own `trtexec.exe` and its SM120 builder resource were later run from a scratch copy outside the repo for the compatibility measurements recorded below; that run installed nothing into any application or SDK and did not change the GPU driver.

## Release identity

- [Release](https://github.com/the-database/VideoJaNai/releases/tag/2.1.0), published 2026-06-18; API release ID `341143839`.
- Repository tag resolves to `68f6060c3971dfc0f2423b5702f2d259013eca60`. The release is **not immutable**; recorded digests, not the mutable URL, identify downloaded bytes.
- [Release API](https://api.github.com/repos/the-database/VideoJaNai/releases/tags/2.1.0) has 14 uploaded assets. The page's 16 entries include two generated source archives, for which no publisher digest or byte size is provided.
- `manifest.json` and `packs.json` are release assets, not files in the tagged tree. The package assembly is [BuildVideoJaNai/Program.cs](https://github.com/the-database/VideoJaNai/blob/2.1.0/BuildVideoJaNai/Program.cs).

## Acquired bytes

Owned root: `F:/ZKS/mikai-native-assets/videojanai-2.1.0/`. Archive downloads are in `_downloads/`; metadata is in `_meta/`; extraction is in separate `extract/<component>/` directories. This is neither another application's installation nor a build/runtime dependency.

The integration parent independently recomputed all six SHA-256 values below. Metadata was hashed from `_meta/`; archives were streamed from `_downloads/`. Sizes and hashes match the publisher's API digests and the supplied pins.

| Asset | Bytes | SHA-256 |
| --- | ---: | --- |
| `manifest.json` | 355 | `e512ed44080a603e17dfe1ad3dbf268565a9891f96b7992f0dae48d20078bf5f` |
| `packs.json` | 4,536 | `a707cc740de288bdd71cc8b82e783c826c65e3dc9001ad933b7b6b0251fefad9` |
| `component-trt-runtime.7z` | 180,144,212 | `c7dfcb3f3bc35285dcf62381f58ba30f882083778e28930bbd0a1fcc7a01df0b` |
| `component-trt-sm120.7z` | 225,025,865 | `e419ff91f643a2445ddd48408f84aa0451767d074a45f261085460ee327ee4ac` |
| `component-trt-ptx.7z` | 207,044,239 | `32e0c8e162de6c786e49b2cc709a4cfa855f83f1633f7dc4d8b5da6b8b501b55` |
| `component-rife.7z` | 462,444,035 | `3bca5476a36038904f283b40f20a0c0eb9fe464f551b5dd680da4a07750decd9` |

No setup executable or complete application package was downloaded. Other SM packs were inventoried from the API, not acquired: the development GPU is SM120. The acquisition record is `_meta/acquisition.json`; archive-member/path audits, binary imports, per-file hashes and terms evidence are retained beside it. Its inspection tooling is not part of the application or SDK dependency closure.

## TensorRT role and version

`packs.json` names these userspace components:

- Runtime: `cudart64_13.dll`, `nvinfer_11.dll`, `nvinfer_plugin_11.dll`, `nvonnxparser_11.dll`, `trtexec.exe`.
- SM120: `nvinfer_builder_resource_sm120_11.dll`.
- PTX: `nvinfer_builder_resource_ptx_11.dll`.

The SM/PTX files are **TensorRT builder resources**, not NVIDIA display drivers, ONNX weights or prepared `.engine`/`.plan` artifacts. The inspected archives contain no display-driver installer, `.sys`, `.inf` or `.cat` files.

The package builder obtains these files from [vs-mlrt v16.test1](https://github.com/AmusementClub/vs-mlrt/releases/tag/v16.test1), whose notes declare TensorRT **11.0** and CUDA **13.2.1**. The acquired CUDA DLL's version resource identifies **CUDA 13.2.75** (`CUDART64_132_75.DLL`, FileVersion `6,14,11,13020`). No TensorRT DLL or `trtexec.exe` in either closure carries a PE version resource, so `trtexec`'s own banner and plan header are the authoritative version source: the pack's `trtexec.exe` reports `TensorRT v110000 b114`, and `--getPlanVersionOnly` on a plan that same `trtexec.exe` built reports **"Plan was created with TensorRT version 11.0.0.114"**. The pack's exact line is therefore **TensorRT 11.0.0.114 / CUDA 13.2.75**, measured — superseding the earlier "patch/build number not established without execution" statement.

The project's independently owned SDK is **TensorRT 11.3.0.99 / CUDA 13.4.92** (its `trtexec.exe` reports `TensorRT v110300 b99`; `NvInferVersion.h` gives `TRT_BUILD_ENTERPRISE 99`; its `cudart64_13.dll` is `CUDART64_134_92.DLL`, FileVersion `6,14,11,13040`). That release is **newer, not equivalent**: no corresponding runtime or builder-resource file is byte-identical, and a shared `_11.dll` name does not prove compatibility.

Every corresponding runtime and builder-resource file differs in size/hash from the project's SDK (pack `nvinfer_11.dll` 375,759,984 / `6a5b087aea7209e5368e12494caa7e86bb58666e8a25e4edf533c483a576e005` vs project 401,707,120 / `ab5148bc…`; pack `nvinfer_builder_resource_sm120_11.dll` 262,534,256 / `548f80dc…` vs project 269,391,472 / `7b616872…`). The pack's binaries were exercised as a **matched official runtime + official SM120 builder resource** from a scratch copy (payloads byte-identical to this extraction), with **no driver change** (driver **616.92**, unchanged; nothing installed or replaced):

- **Build PASSED** on the RTX 5090 (compute capability 12.0): `trtexec.exe --onnx=… --saveEngine=… --skipInference --noTF32 --builderOptimizationLevel=3` produced a 1.32 MiB engine.
- **GPU inference PASSED**: real execution via `--loadEngine` at 8,983.05 qps, "Engine deserialized in 0.0159622 sec".
- The **SM120 builder resource is a hard requirement** on this GPU, and the **PTX resource does not substitute for it**. A closure holding the six matched official runtime files plus `nvinfer_builder_resource_ptx_11.dll`, with no SM resource and no PTX-JIT fallback flag, fails at `createInferBuilder` with `Error Code 6: API Usage Error (Unable to load library: nvinfer_builder_resource_sm120_11.dll: 126)` (`libLoader.cpp:302`): builder creation fails, no engine is built and no inference runs. The runtime does **not** fall through to a PTX-only path for SM 120 with this release.

The two closures are **not interchangeable**:

- **Serialized engines do not cross in either direction.** A project 11.3.0.99 engine fails to deserialize under the pack's 11.0.0.114 runtime (`Serialization assertion stdVersionRead == kSERIALIZATION_VERSION failed`; serialized version 244 vs current 243), and a pack-built engine fails identically under the 11.3.0.99 runtime (244 vs 243). Header-only `--getPlanVersionOnly` reads succeed both ways but do **not** prove engine interchangeability.
- **Builder resources are version-locked.** The project 11.3.0.99 runtime with the pack's SM120 resource, and the pack runtime with the project's SM120 resource, both fail `createCaskKernelLibraryImpl` / `Assertion validateCaskKLibSize(buffer.size) failed. Klib 0 failed validation` and yield an empty engine. Only a **same-release pairing** builds.

**This is not a general compatibility approval and no mixed closure was installed.** The pack works only as its own matched pair on this GPU and driver; it must not be mixed with the 11.3 headers, libraries, builder resources or serialized engines. No source-to-binary reproduction is claimed.

The component archives carry no license/notice files. The application builder generates notices referring to the NVIDIA TensorRT SLA and CUDA EULA. Those notices do not expand NVIDIA's grant; the existing `trtexec` redistribution restriction remains. No redistribution is approved here.

**Licence position (2026-10-04, in-session):** The project's own integration is covered by the owner's unrestricted grant (recorded at `../LICENSE`, The Unlicense). Third-party components (NVIDIA TensorRT/CUDA runtime/resources, VideoJaNai release assets) are not relicensed by it and remain governed by their upstream terms as recorded. All original digests, pins and provenance tables are preserved unchanged.

## Official AnimeJaNai 3.5.0 comparison cohort

The compatibility measurements above were taken from payloads extracted from the **official 3.5.0** package (`F:/ZKS/mikai-native-sdks/animejanai-official/c10-trt/`, `package_version` **3.5.0**). That 3.5.0 pack is published as [mpv-AnimeJaNai 3.5.0](https://github.com/the-database/mpv-AnimeJaNai/releases/tag/3.5.0) (release ID `341404556`); a VideoJaNai `3.5.0` tag does not exist. It is a **comparison cohort, not the same release** and not the same `.7z` containers as this 2.1.0 acquisition:

| Container | 3.5.0 bytes | 3.5.0 SHA-256 | 2.1.0 bytes / SHA-256 (unchanged pin) |
| --- | ---: | --- | --- |
| `component-trt-runtime.7z` | 180,148,514 | `54a48e5fc88a4230d2d6e772b71b6821654be96dbd21c3f6d967f06aa58378bf` | 180,144,212 / `c7dfcb3f3bc35285dcf62381f58ba30f882083778e28930bbd0a1fcc7a01df0b` |
| `component-trt-sm120.7z` | 225,025,865 | `e419ff91f643a2445ddd48408f84aa0451767d074a45f261085460ee327ee4ac` | 225,025,865 / `e419ff91f643a2445ddd48408f84aa0451767d074a45f261085460ee327ee4ac` |
| `component-trt-ptx.7z` | 207,044,239 | `32e0c8e162de6c786e49b2cc709a4cfa855f83f1633f7dc4d8b5da6b8b501b55` | 207,044,239 / `32e0c8e162de6c786e49b2cc709a4cfa855f83f1633f7dc4d8b5da6b8b501b55` |

The 3.5.0 runtime container digest (`54a48e5f…`) is **not** a 2.1.0 pin and must never be substituted for the 2.1.0 container digest (`c7dfcb3f…`); the SM120 and PTX containers are byte-identical across both releases. However, every extracted payload (all six runtime files plus `nvinfer_builder_resource_sm120_11.dll` and `nvinfer_builder_resource_ptx_11.dll`) hashes **byte-identically** to this 2.1.0 extraction. The measured `11.0.0.114 / 13.2.75` versions therefore apply to the **byte-identical payload** and only when the exact payload digest matches; they do not re-identify the 2.1.0 containers, whose pins above are untouched.

## Builder-resource payloads

The 2.1.0 `component-trt-sm120.7z` contains **exactly one** file, `nvinfer_builder_resource_sm120_11.dll` (262,534,256 bytes / `548f80dc63a38a9f60e1a29e8268353861306cb02ec22cb04bee3bea4d5b29a2`). It is a **stub loader** carrying an opaque `.rsrc` payload (262,402,936 of its 262,534,256 bytes; `.text` 68,096, `.rdata` 41,314, `.data` 7,764, `.pdata` 4,860). It is **not** a tactics/timing cache and **not** a prepared engine: no separate cache or `.engine`/`.plan` file ships in the pack.

`KERNEL32.dll` is its only import; it has no `nvcuda`/`cudart` dependency. Printable strings `ptx` and `sm_89` are present, but strings inside a binary **cannot** establish that SASS, PTX or JIT was compiled, distributed or executed — the `.rsrc` blob is opaque and no disassembly or symbol proof was taken. The SASS/PTX/JIT path is therefore **unverified** and is not recorded as a confirmed execution path; only an actual `cuobjdump`/function-execution check could settle it, which this acquisition did not perform.

The 2.1.0 `component-trt-ptx.7z` likewise contains exactly one file, `nvinfer_builder_resource_ptx_11.dll` (241,266,800 bytes / `6e24c053713d7b927bf6ee897c35cd5e48690cb6d7ebb237943c14cd4e91ac40`), byte-identical to the 2.1.0 extraction. It is an AMD64 PE32+ stub whose only import is `KERNEL32.dll`, with a 241,135,552-byte opaque `.rsrc` and no version resource. Its `.rsrc` contents are likewise **unverified** — no SASS/PTX claim is made. PTX-only sufficiency on SM 120 was measured and **refused** (probe log `.probe-out/c10-ptx-only-build.log`, `EXIT 1`, TensorRT 11.0.0 b114 on the RTX 5090, cc 12.0): the PTX resource alone does not let the runtime create a builder, which is a precise *missing-SM120-resource* refusal — not a claim that the PTX payload has no content or that PTX cannot execute at all, and no SM resource was renamed or mixed to force a result.

## Exact RIFE 4.14 graph identity

The RIFE archive contains 47 ONNX graphs. Only the four required 4.14 combinations were compared with `F:/ZKS/mikai-native-assets/rife/onnx/`; all four have identical size and SHA-256. No newer graph was substituted.

| Graph | Bytes | SHA-256 |
| --- | ---: | --- |
| `rife_v4.14.onnx` | 10,778,647 | `a64b9cd0fb463d620688394437f35f02e33c0658c39ccce2b9637899fb520cbe` |
| `rife_v4.14_ensemble.onnx` | 10,785,406 | `ec64f92e31a1b61c034b4de6f1afc1c2aeaf478b93dbf0a6ce2e69a327385b53` |
| `rife_v4.14_lite.onnx` | 5,986,311 | `948019ffb5e7e54ac6b89a13542da8d100af9b4ade7374551c5f595ac3ff2ce7` |
| `rife_v4.14_lite_ensemble.onnx` | 5,993,070 | `9486d87ca17a83a1594ac54b254b6fb1be6fa52f5dd11a31e4a602d124ea8b1b` |

The release manifest points to `models-rife-fp16-1`; this confirms the acquisition route, **not the original-weight lineage** — the weight-data lineage question was instead closed by direct comparison, not by this release metadata. In [rife-acquisition.md](./rife-acquisition.md) §5.1 the stored weight data of all four graphs was compared against the author's own MIT-licensed v4.14 / v4.14.lite checkpoints: **128/128 initializer names and shapes matched with FP32→FP16 round-to-nearest-even bytes identical**, across all four comparisons, so the stored weights *are* the author's weights. The vs-mlrt-to-hzwer hop **for weight data is closed by that evidence**; the [converter](https://github.com/the-database/animejanai-inference/blob/v0.6.0/tools/convert_rife_fp16.py) and [model release](https://github.com/the-database/animejanai-inference/releases/tag/models-rife-fp16-1) identify vs-mlrt `v1/video_player` graphs but still name no exporter and affirm no rights over the files as artefacts. What remains open is exporter identity, the non-weight graph material, and the rights basis for redistributing the graphs as whole artefacts. These graphs were not executed and are not a Ready capability.

## Inference implementation grant

The release references `aji v0.6.0`, tag commit `b924a85642705e01e07854a0182f5a16780114d2`. That tag's `include/aji.h` declares **ABI 7**, not the project's existing ABI-8 source pin. It must not be silently substituted.

No explicit source/binary grant was located in that tagged repository, release body or generated aji notice. VideoJaNai's root GPL-3.0 text does not explicitly identify the independently downloaded aji source/binaries as covered. No source-to-binary attestation was located. The inference grant blocker and unsent [permission request](./permission-request.md) therefore remain unchanged.
