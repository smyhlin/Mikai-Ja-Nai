# VideoJaNai 2.1.0 component acquisition

Evidence only. No component was installed, executed, added to a runtime bundle, or used to replace a driver or SDK.

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

The package builder obtains these files from [vs-mlrt v16.test1](https://github.com/AmusementClub/vs-mlrt/releases/tag/v16.test1), whose notes declare TensorRT **11.0** and CUDA **13.2.1**. The acquired CUDA DLL's version resource identifies runtime **13.2.75**. The TensorRT DLLs have no precise version resource; an exact patch/build number was not established without execution.

Every corresponding runtime and builder-resource file differs in size/hash from the independently acquired **TensorRT 11.3.0.99 / CUDA 13.4.92** SDK. A shared `_11.dll` name does not prove compatibility. **Do not mix the pack's runtime/resources with the 11.3 headers, libraries or serialized engines.** No GPU execution, engine build, source-to-binary reproduction or runtime compatibility is claimed for this pack.

The component archives carry no license/notice files. The application builder generates notices referring to the NVIDIA TensorRT SLA and CUDA EULA. Those notices do not expand NVIDIA's grant; the existing `trtexec` redistribution restriction remains. No redistribution is approved here.

## Exact RIFE 4.14 graph identity

The RIFE archive contains 47 ONNX graphs. Only the four required 4.14 combinations were compared with `F:/ZKS/mikai-native-assets/rife/onnx/`; all four have identical size and SHA-256. No newer graph was substituted.

| Graph | Bytes | SHA-256 |
| --- | ---: | --- |
| `rife_v4.14.onnx` | 10,778,647 | `a64b9cd0fb463d620688394437f35f02e33c0658c39ccce2b9637899fb520cbe` |
| `rife_v4.14_ensemble.onnx` | 10,785,406 | `ec64f92e31a1b61c034b4de6f1afc1c2aeaf478b93dbf0a6ce2e69a327385b53` |
| `rife_v4.14_lite.onnx` | 5,986,311 | `948019ffb5e7e54ac6b89a13542da8d100af9b4ade7374551c5f595ac3ff2ce7` |
| `rife_v4.14_lite_ensemble.onnx` | 5,993,070 | `9486d87ca17a83a1594ac54b254b6fb1be6fa52f5dd11a31e4a602d124ea8b1b` |

The release manifest points to `models-rife-fp16-1`; this confirms the acquisition route, **not the original-weight lineage**. The [converter](https://github.com/the-database/animejanai-inference/blob/v0.6.0/tools/convert_rife_fp16.py) and [model release](https://github.com/the-database/animejanai-inference/releases/tag/models-rife-fp16-1) identify vs-mlrt `v1/video_player` graphs but do not affirm the source weights for these specific 4.14 files. The open vs-mlrt-to-hzwer hop in [rife-acquisition.md](./rife-acquisition.md) remains open. These graphs were not executed and are not a Ready capability.

## Inference implementation grant

The release references `aji v0.6.0`, tag commit `b924a85642705e01e07854a0182f5a16780114d2`. That tag's `include/aji.h` declares **ABI 7**, not the project's existing ABI-8 source pin. It must not be silently substituted.

No explicit source/binary grant was located in that tagged repository, release body or generated aji notice. VideoJaNai's root GPL-3.0 text does not explicitly identify the independently downloaded aji source/binaries as covered. No source-to-binary attestation was located. The inference grant blocker and unsent [permission request](./permission-request.md) therefore remain unchanged.
