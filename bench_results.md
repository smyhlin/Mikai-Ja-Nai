# Benchmark results

Measured results from **Settings → Benchmark**, grouped by machine and model. Compare matching input resolutions, backends, and settings—not GPU names alone. Only observed RTX 5090 results are included; RTX 3080 and RTX 2070 reports will be added when supplied.

**Scope:** all 31 non-RIFE models—23 Anime4K presets, 3 AnimeJaNai profiles, and 5 Spatial graphs—at all five bundled input resolutions, plus baseline. **RIFE is excluded by request.**

> **Workload limitation:** the bundled clips are synthetic black-frame H.264 fixtures, not anime footage. These measurements describe this reproducible fixture workload; they are **not a guarantee of real-scene playback performance**. Black frames omit motion, scene changes, and real-scene complexity. A runtime refusal is not evidence that a GPU lacks performance or VRAM.

## Reading the numbers

- Values are **rendered output frames per second**, not milliseconds, GPU utilization, engine-build speed, or an image-quality score. Higher means more throughput in this workload.
- Each column names the **input** dimensions. The OpenGL framebuffer is twice as wide and twice as high:

  | Input | Render target |
  | --- | --- |
  | 480 × 360 | 960 × 720 |
  | 640 × 480 | 1280 × 960 |
  | 768 × 576 | 1536 × 1152 |
  | 1280 × 720 | 2560 × 1440 |
  | 1920 × 1080 | 3840 × 2160 |

- Output throughput must exceed the source frame rate to sustain playback without interpolation. The fixtures are 23.976 fps. Allow additional headroom for real footage, display presentation, other GPU workloads, and thermal limits; these short synthetic runs do not establish a sustained real-world limit.
- A failed row retains its reason rather than being converted into zero fps. Missing components, unsupported models, startup errors, and inference failures are different from a measured low frame rate.
- No peak-VRAM or power measurements were collected. Installed VRAM is a machine specification, **not** a per-model minimum requirement.
- Each model is tested independently. Interpolation and combined upscaling/interpolation pipelines are outside this report.

## Measurement method

The real application benchmark runs decode/filter/render through libmpv into an offscreen OpenGL framebuffer. Playback is unthrottled (`untimed=yes`), with audio, cache, and frame dropping disabled. New video output frames count; redraws and repeated render requests do not. The first eligible frame starts the clock without incrementing the counter, followed by a three-second sampling window.

TensorRT engine preparation finishes before eligible measurement starts. A build during measurement resets the sample. Compilation is therefore **excluded** from fps; a fast row can still require a slow first-use build. Engines are GPU-specific, so do not copy an RTX 5090 engine to another GPU as a substitute for compiling there.

The five bundled clips have 2,158 H.264 frames each, a frame rate of `2997/125`, and a duration of about 90 seconds. They loop if needed. Each published cell is one completed sample, not an average of repeated trials; small differences between cells or machines can be run-to-run noise.

The RTX 5090 matrix combines the completed spatial/shader capture with five remeasured Spatial Performance cells that had been invalidated by an OpenGL surface failure. The dedicated rerun's baseline checks rendering but does not replace the original baseline or get averaged into it. No estimates or RIFE measurements are included.

Native rows request NVDEC hardware decoding; baseline and Anime4K rows do not request hardware decoding. Cross-group fps therefore includes different decode paths. Built-in AnimeJaNai profiles choose resolution/FPS-dependent chains, while standalone Spatial rows request the named graph directly.

The probe isolates QSettings, selecting the default TensorRT backend. It still uses and updates the deployed `native/` runtime, model, engine, configuration, and statistics directories. Building GPU-specific caches beside the executable is expected.

## RTX 5090

| Setting | Observed value |
| --- | --- |
| Capture dates (UTC) | 2026-10-15; five replacement cells completed 2026-10-16 |
| GPU | NVIDIA GeForce RTX 5090 |
| Driver | 616.92 |
| GPU memory reported by NVIDIA | 32,607 MiB |
| Compute capability | 12.0 |
| CPU | AMD Ryzen 7 5800X3D, 8 cores / 16 threads |
| System RAM | 47.9 GiB reported |
| OS | Windows x64, build 10.0.26200.9457 |
| Application source | `8a71e08`; non-RIFE sampling math unchanged from `3321b08` |
| Qt | 6.12.0 |
| Native runtime | AJI v0.9.0, TensorRT 11.3 component closure |
| Graphics | `QT_QPA_PLATFORM=windows`, `QT_OPENGL=desktop`; diagnostic renderer: `NVIDIA GeForce RTX 5090/PCIe/SSE2` |
| Run environment | English labels, isolated probe settings, one benchmark workload at a time |

Qt's automatic graphics selection used Mesa software rendering in one diagnostic run. Those failed attempts are excluded from the GPU results. The hardware renderer was checked explicitly before the published run.

**Coverage:** 160 unique model/clip pairs, including five baseline samples. All 160 final cells have positive measured fps. Five environmental failures were remeasured successfully rather than labeled GPU/model incompatibilities. Numbers below are fps.

### Baseline

| Model | 480 × 360 | 640 × 480 | 768 × 576 | 1280 × 720 | 1920 × 1080 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline (no model) | 1953.5 | 1495.1 | 1401.7 | 1342.3 | 1053.3 |

### Anime4K

| Model | 480 × 360 | 640 × 480 | 768 × 576 | 1280 × 720 | 1920 × 1080 |
| --- | ---: | ---: | ---: | ---: | ---: |
| A (Fast) | 405.6 | 388.5 | 397.5 | 400.5 | 378.1 |
| B (Fast) | 406.2 | 411.7 | 403.4 | 399.7 | 378.2 |
| C (Fast) | 563.0 | 566.0 | 565.1 | 549.9 | 497.9 |
| A (High Quality) | 230.9 | 230.8 | 268.0 | 367.4 | 321.6 |
| B (High Quality) | 369.2 | 373.5 | 371.4 | 350.5 | 324.6 |
| C (High Quality) | 552.2 | 561.9 | 535.1 | 529.1 | 469.9 |
| A+A (Maximum) | 320.0 | 318.5 | 314.3 | 305.6 | 249.2 |
| B+B (Maximum) | 291.8 | 313.4 | 316.3 | 315.2 | 247.5 |
| C+A (Maximum) | 458.3 | 459.6 | 323.9 | 357.1 | 342.4 |
| 3D Graphics | 961.6 | 1111.6 | 891.7 | 866.5 | 660.8 |
| Old Film Restore | 362.2 | 454.8 | 529.7 | 530.6 | 507.2 |
| Deblur | 607.5 | 615.3 | 596.6 | 548.5 | 454.8 |
| Noise Reduction · Bilateral Mean | 1812.2 | 1848.1 | 1619.5 | 1620.9 | 1279.2 |
| Noise Reduction · Bilateral Median | 1661.2 | 1594.2 | 1612.0 | 1501.4 | 1214.9 |
| Noise Reduction · Bilateral Mode | 1696.0 | 1626.9 | 1470.8 | 1483.8 | 1182.4 |
| Thin Lines · VeryFast | 976.5 | 1062.4 | 1009.6 | 530.9 | 701.7 |
| Thin Lines · Fast | 1140.1 | 1006.1 | 1119.8 | 995.0 | 906.7 |
| Thin Lines · HQ | 1109.5 | 1135.6 | 1132.6 | 753.5 | 629.9 |
| Texture Enhancement · DTD x2 | 713.4 | 608.2 | 688.9 | 668.2 | 604.4 |
| Texture Enhancement · DoG x2 | 1512.5 | 1506.9 | 1472.7 | 1324.2 | 1080.6 |
| Darken · VeryFast | 1242.9 | 1199.2 | 1121.3 | 1020.6 | 920.0 |
| Darken · Fast | 879.2 | 846.0 | 996.3 | 901.9 | 724.8 |
| Darken · HQ | 939.3 | 922.3 | 943.4 | 932.0 | 1019.5 |

### AnimeJaNai

| Model | 480 × 360 | 640 × 480 | 768 × 576 | 1280 × 720 | 1920 × 1080 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Quality | 739.1 | 481.9 | 350.2 | 385.3 | 153.0 |
| Balanced | 740.3 | 480.9 | 348.6 | 383.9 | 153.0 |
| Performance | 734.2 | 478.8 | 348.0 | 626.2 | 280.2 |

### Spatial

| Model | 480 × 360 | 640 × 480 | 768 × 576 | 1280 × 720 | 1920 × 1080 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx | 1174.8 | 843.1 | 647.9 | 382.8 | 152.0 |
| 2x_AnimeJaNai_HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx | 1474.7 | 1170.2 | 944.0 | 605.6 | 282.6 |
| 2x_AnimeJaNai_HD_V3.1Sharp1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx | 1095.6 | 737.5 | 662.8 | 379.8 | 149.6 |
| 2x_AnimeJaNai_HD_V3.1Sharp1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx | 1396.8 | 1099.8 | 914.3 | 625.7 | 290.1 |
| 2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo.onnx | 719.6 | 477.3 | 347.2 | 172.5 | 77.9 |

## Reproducing and submitting another machine

1. Use the same application revision, bundled clips, and model names. In **Settings → Components**, install and verify the runtime, matching GPU builder resource, and spatial models before measuring.
2. Run **Settings → Benchmark** with all clips, Anime4K, AnimeJaNai, Spatial, and baseline selected. Leave the **RIFE group unchecked**. Keep unrelated GPU workloads stopped. Record each failure's full detail as well as successful fps values.
3. Include GPU name and VRAM, driver, CPU, RAM, OS, application revision, backend, and input/output dimensions. State any different clips, model revisions, or graphics settings. Do not merge unlike workloads into the same comparison table.
4. Send the result export or screenshots with their model/clip labels. Add each machine as a separate section with the same columns; leave unmeasured cells explicitly unmeasured rather than extrapolating from another GPU.

The developer `benchmark_probe` exercises the same Settings surface and can save machine-readable evidence. On Windows it must run beside the deployed runtime, shaders, models, and `benchmarks/` directory; a bare executable from the build directory is not a self-contained benchmark.

From that deployed directory in PowerShell:

```powershell
$env:QT_QPA_PLATFORM = "windows"
$env:QT_OPENGL = "desktop"
$env:MIKAI_PROBE_LANGUAGE = "en"
$ids = .\benchmark_probe.exe --list-models |
    Where-Object { $_ -notmatch '^RIFE\t' } |
    ForEach-Object { ($_ -split "\t")[1] }
.\benchmark_probe.exe --model ($ids -join ',') --all-clips --output rtx5090-no-rife.json
```

Use a matching filename such as `rtx3080-no-rife.json` or `rtx2070-no-rife.json` on another machine. `--list-models` lists stable catalogue IDs; `--model <id,id,...> --clip 1280x720.mp4 --output subset.json` captures a smaller selection.

For long first-use runs, capture subsets into separate files with `--model <id,id,...> --all-clips --output group.json`. Keep those completed exports so an interrupted process does not require repeating every model. Engine preparation is outside the timed sample, but it can dominate the total capture time.

A complete run can return exit code 1 because a model failed, including a known unsupported model. The JSON still contains every row: require `"complete": true`, then distinguish measured `fps` from `null` accompanied by `reason`. Exit code 2 identifies a setup, argument, or evidence-write error. An incomplete capture must not be presented as a completed matrix.
