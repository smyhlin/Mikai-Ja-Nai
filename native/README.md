# `native/` — Native Enhancement Provenance, Manifests & Component Store

This directory contains the provenance manifests, license notices, standalone CLI verification tools, patch series, and optional MSVC + CUDA + TensorRT build configuration for the **AnimeJaNai** and **Prepared Native** video enhancement tracks.

---

## 1. Architectural Overview: Two Enhancement Tracks

The application supports two distinct neural enhancement contracts alongside the built-in **Anime4K** GLSL shaders:

1. **AnimeJaNai Official ABI-8 Runtime (`AnimeJanaiRuntime`, `AnimeJanaiInstallation`, `ComponentStoreTab`):**
   - Uses the `vf_animejanai` filter compiled into the `the-database/mpv` core, paired with the `v0.9.0` ABI-8 dispatcher (`aji.dll` + `aji_trt.dll` / `aji_dml.dll` on Windows; `libaji.so` + `libaji_trt.so` on Linux) and the `3.7.0` TensorRT runtime/builder-resource packs (`component-trt-*`).
   - Engines are built on first use via `trtexec` (`trtexec.exe` on Windows, `trtexec` on Linux) inside the app-owned `<nativeRoot>/animejanai/` directory.
2. **Prepared Native Bundle Track (`mikai_native_support`, `NativePlaybackController`, `NativeBuilder`):**
   - Uses a versioned, manifest-verified bundle under `<nativeRoot>/runtime/<bundle-id>/` (`manifest.json` schema `mikai.native.bundle/1`) together with out-of-process preparation tools (`mikai_native_prep`, `mikai_native_verify`, `native_inspect`) that build and numerically verify TensorRT plans against an ONNX Runtime CPU reference before admitting them into playback.

---

## 2. Pinned Release Manifests (`native/manifest/`)

All external runtime dependencies, model graphs, and source revisions are pinned by cryptographic SHA-256 digest:

| Manifest File | Schema / Purpose |
|---|---|
| `native/manifest/sources.json` | (`mikai.native.sources/1`) Pinned upstream git revisions, trees, and release asset digests for `mpv-fork`, `animejanai-inference` (`v0.9.0`), `tensorrt-11.3-sdk` (`11.3.0.99` / CUDA `13.4`), `onnxruntime-directml` (`1.24.4`), `directml` (`1.15.4`), 5 spatial ONNX models, and RIFE `v4.14` checkpoints. |
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
| `aji-v0.9.0` | `aji-windows-x64.zip` (`657,798` B)<br>`22828c41439ac2b165a18294ba3e15257e8df617ab1d7ab6fb0f96e3915c5586` | `aji-linux-x64.tar.zst` (`242,674` B)<br>`02dc66fb49a49c757bc3edb6153321150627a26b059fe92da6e063b31ee6bdf9` |

---

## 3. Rights & Redistribution Boundaries (`ESC-1..6`)

- **No third-party binaries or model graphs are vendored in this repository or shipped by `cmake/dist.cmake`.**
- `cmake/dist.cmake` explicitly strips any `components`, `assets`, `models`, or `records` directories from `dist-mikai/native/` and verifies their absence before completing.
- **Escalation summary (`native/manifest/components.json`):**
  - `ESC-1`: Upstream `the-database/animejanai-inference` carries no repository license file (`license: null`). Local user-initiated acquisition into `<nativeRoot>/components/` is supported; public redistribution of compiled `aji*` binaries remains `blocked`.
  - `ESC-2`: The 5 spatial ONNX models (`models-2x_AnimeJaNai-1`) are licensed under **CC BY-NC-SA 4.0** (local non-commercial use permitted; bundle redistribution `blocked`).
  - `ESC-3`: RIFE `v4.14` weight tensors trace byte-for-byte (128/128 FP32→FP16 tensors) to `hzwer/Practical-RIFE` (MIT), while the ONNX exporter wrapper metadata is unattributed; graphs are acquired per-machine into `<nativeRoot>/components/rife/3.7.0`.
  - `ESC-4` *(Resolved)*: DirectML (`Microsoft.AI.DirectML 1.15.4`) and ONNX Runtime DirectML (`1.24.4`) licenses are verified (`LICENSE.txt` + `LICENSE-CODE.txt`).
  - `ESC-5`: `trtexec` is an NVIDIA SDK developer tool; the Prepared Native helper (`mikai_native_prep`) uses the in-process `nvinfer1` C++ builder API without invoking `trtexec`, while the official AnimeJaNai config path extracts `trtexec` only into the user's local component store.
  - `ESC-6`: Tracks GPL/LGPL complete-corresponding-source obligations for `mpv`.

---

## 4. Verification & Maintenance Tools

- **`mikai_manifest_check`** (`native/tools/manifest_check.cpp`): Validates `native/manifest/sources.json` and `native/manifest/components.json` schemas, git SHA pins, ABI symbol counts, and digest inventories.
- **`mikai_bundle`** (`native/tools/bundle_tool.cpp`): Generates, inspects, and verifies `<nativeRoot>/runtime/<bundle>/manifest.json` against on-disk PE/ELF binaries.
- **`native_inspect`** (`native/tools/native_inspect.cpp`, enabled when `MIKAI_WITH_MODEL_INSPECTION=ON`): Inspects ONNX model inputs/outputs and executes CPU reference inference via ONNX Runtime.
- **Standalone MSVC + CUDA Build (`native/build-msvc.md`):** Instructions for compiling `aji`, `aji_trt`, `aji_dml`, `mikai_native_prep`, and `mikai_native_verify` with MSVC, CUDA 13.4, and TensorRT 11.3.0.99.
