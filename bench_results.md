# Benchmark results

**Historical snapshot:** the 31-model, 160-cell RTX 5090 matrix and the
2026-10-17 old/new comparison below are preserved historical measurements from
their recorded revisions and settings. They have not been updated for the
custom-model/profile integration. The current benchmark catalogue can additionally
contain imported custom graphs and `builtin_slot:<1..10>` profiles, and current
configuration/Sharp behavior can change which chain/model a profile selects.
Do not treat those dynamic entries or current settings as members of the stock
historical selection, or merge their results into its comparison tables.

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
- For the original full matrix, no peak-VRAM or power measurements were collected. Installed VRAM is a machine specification, **not** a per-model minimum requirement. GPU telemetry was collected during the separate controlled follow-up, but it does not establish peak-VRAM requirements.
- Each model is tested independently. Interpolation and combined upscaling/interpolation pipelines are outside this report.

## Measurement method

The real application benchmark runs decode/filter/render through libmpv into an offscreen OpenGL framebuffer. Playback is unthrottled (`untimed=yes`), with audio, cache, and frame dropping disabled. New video output frames count; redraws and repeated render requests do not. The first eligible frame starts the clock without incrementing the counter, followed by a three-second sampling window.

TensorRT engine preparation finishes before eligible measurement starts. A build during measurement resets the sample. Compilation is therefore **excluded** from fps; a fast row can still require a slow first-use build. Engines are GPU-specific, so do not copy an RTX 5090 engine to another GPU as a substitute for compiling there.

The five bundled clips have 2,158 H.264 frames each, a frame rate of `2997/125`, and a duration of about 90 seconds. They loop if needed. Each published cell is one completed sample, not an average of repeated trials; small differences between cells or machines can be run-to-run noise.

The RTX 5090 full matrix below is one fresh, complete capture on 2026-10-17 after the rendering-lifecycle and diagnostic fixes. Its 160 cells are single measurements and remain unchanged; the separate controlled AnimeJaNai follow-up below reports repeated old/new medians and does not replace or merge into that matrix. No full-matrix cells were merged from older runs, remeasured selectively, estimated, or averaged. RIFE is excluded.

This offscreen throughput run checks the decode/filter/render pipeline, not interactive HUD cost, display-vsync pacing, or a before/after speedup. Separate real-player probes verified moving non-black video, paused repaint stability, fractional-DPR resizing, hidden/minimized/covered recovery, context recreation, GPU-painted chrome, PiP transitions, and cached TensorRT playback.

Native rows request NVDEC hardware decoding; baseline and Anime4K rows do not request hardware decoding. Cross-group fps therefore includes different decode paths. Built-in AnimeJaNai profiles choose resolution/FPS-dependent chains, while standalone Spatial rows request the named graph directly.

The probe isolates QSettings, selecting the default TensorRT backend. It still uses and updates the deployed `native/` runtime, model, engine, configuration, and statistics directories. Building GPU-specific caches beside the executable is expected.

## RTX 5090

| Setting | Observed value |
| --- | --- |
| Capture date (UTC) | 2026-10-17 |
| Complete capture elapsed time | 9 min 49 s |
| GPU | NVIDIA GeForce RTX 5090 |
| Driver | 616.92 |
| GPU memory reported by NVIDIA | 32,607 MiB |
| Compute capability | 12.0 |
| CPU | AMD Ryzen 7 5800X3D, 8 cores / 16 threads |
| System RAM | 47.9 GiB reported |
| OS | Windows 11 Pro x64, version 10.0.26200 |
| Application source | `cb73c9c` (rendering, GPU chrome, diagnostic fixes, and regression probes) |
| Qt | 6.12.0 |
| Native runtime | AJI v0.9.0, TensorRT 11.3 component closure |
| Graphics | `QT_QPA_PLATFORM=windows`, `QT_OPENGL=desktop`; diagnostic renderer: `NVIDIA GeForce RTX 5090/PCIe/SSE2` |
| Run environment | Active Windows console, English labels, isolated probe settings; other verification probes finished before measurement |

The real-player preflight reported NVIDIA OpenGL 4.6 and `hwdec-current=nvdec`. Desktop OpenGL was explicitly selected for the matrix; no software-renderer results are mixed into these tables.

**Coverage:** 160 unique model/clip pairs: five baseline samples, 115 Anime4K samples, 15 AnimeJaNai samples, and 25 Spatial samples. The export reports `complete: true`; all 160 cells have positive finite fps and empty failure reasons. The probe exited successfully with zero failures. Numbers below are fps.

### Baseline

| Model | 480 × 360 | 640 × 480 | 768 × 576 | 1280 × 720 | 1920 × 1080 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline (no model) | 1612.8 | 1369.4 | 1399.9 | 1301.0 | 1038.6 |

### Anime4K

| Model | 480 × 360 | 640 × 480 | 768 × 576 | 1280 × 720 | 1920 × 1080 |
| --- | ---: | ---: | ---: | ---: | ---: |
| A (Fast) | 234.3 | 357.5 | 367.7 | 389.3 | 374.1 |
| B (Fast) | 393.5 | 398.4 | 384.0 | 362.8 | 359.2 |
| C (Fast) | 528.8 | 539.6 | 543.7 | 534.0 | 483.3 |
| A (High Quality) | 228.8 | 228.3 | 228.8 | 223.9 | 278.0 |
| B (High Quality) | 226.9 | 288.0 | 349.4 | 352.8 | 277.0 |
| C (High Quality) | 541.7 | 549.7 | 549.8 | 540.2 | 489.4 |
| A+A (Maximum) | 311.8 | 324.6 | 330.5 | 194.7 | 237.4 |
| B+B (Maximum) | 328.2 | 319.2 | 335.5 | 330.0 | 238.2 |
| C+A (Maximum) | 451.3 | 453.6 | 441.8 | 304.1 | 271.3 |
| 3D Graphics | 1195.5 | 1196.9 | 1201.9 | 1087.3 | 987.2 |
| Old Film Restore | 559.0 | 550.8 | 489.9 | 544.8 | 492.3 |
| Deblur | 596.8 | 612.1 | 600.8 | 603.2 | 529.1 |
| Noise Reduction · Bilateral Mean | 1852.7 | 1681.6 | 1606.6 | 1551.4 | 1321.6 |
| Noise Reduction · Bilateral Median | 1703.0 | 1654.0 | 1586.7 | 1462.2 | 1151.4 |
| Noise Reduction · Bilateral Mode | 1633.1 | 1643.5 | 1570.6 | 1439.2 | 1047.3 |
| Thin Lines · VeryFast | 1129.9 | 1142.0 | 1101.8 | 1012.9 | 700.3 |
| Thin Lines · Fast | 1011.7 | 993.8 | 972.2 | 917.7 | 662.3 |
| Thin Lines · HQ | 781.6 | 769.9 | 865.8 | 1018.7 | 833.7 |
| Texture Enhancement · DTD x2 | 716.9 | 663.8 | 717.0 | 671.9 | 573.2 |
| Texture Enhancement · DoG x2 | 1550.2 | 1525.8 | 1471.5 | 1334.2 | 1101.8 |
| Darken · VeryFast | 1240.4 | 1235.7 | 1223.5 | 1124.1 | 981.1 |
| Darken · Fast | 1271.9 | 1193.4 | 1137.6 | 1097.7 | 825.9 |
| Darken · HQ | 1114.1 | 1096.7 | 953.5 | 916.8 | 780.4 |

### AnimeJaNai

| Model | 480 × 360 | 640 × 480 | 768 × 576 | 1280 × 720 | 1920 × 1080 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Quality | 729.6 | 470.2 | 338.5 | 362.0 | 142.1 |
| Balanced | 713.6 | 470.0 | 345.6 | 383.9 | 152.7 |
| Performance | 727.2 | 475.8 | 345.1 | 612.1 | 272.8 |

### Controlled AnimeJaNai follow-up

A separate RTX 5090 old/new comparison was run on 2026-10-17 for the three built-in AnimeJaNai profiles at 1280 × 720 and 1920 × 1080 input. The preserved executables were old `8a71e08` and new `cb73c9c`, tested in ABBA order (old/new/new/old) for each clip. Each profile/resolution cell has two valid samples per version; the six comparisons below report medians. Across the comparison, 24 valid native and 8 baseline measurements were collected. The engine cache was unchanged, and RIFE was excluded. The benchmark remained foreground in a connected Windows/RDP desktop session.

| Profile | Input | Old median (fps) | New median (fps) | New vs old |
| --- | --- | ---: | ---: | ---: |
| Quality | 1280 × 720 | 383.75 | 383.55 | −0.05% |
| Balanced | 1280 × 720 | 383.05 | 382.45 | −0.16% |
| Performance | 1280 × 720 | 612.05 | 608.80 | −0.53% |
| Quality | 1920 × 1080 | 153.40 | 153.15 | −0.16% |
| Balanced | 1920 × 1080 | 153.25 | 153.20 | −0.03% |
| Performance | 1920 × 1080 | 280.90 | 281.25 | +0.12% |

The historical 6–7% Quality-profile loss was not reproduced as a version-dependent regression in this comparison. These results do not establish an all-GPU or all-resolution outcome. A separate native Quality 1080p foreground/background focus probe produced only one valid pair (152.8/150.7 fps); the failed repeat was excluded, so this effect was not replicated. Do not transfer the separate 26.6% Anime4K focus effect to AnimeJaNai. These measurements do not identify the historical cause or support a zero-effect claim.

The original full-matrix cells above remain single measurements and unchanged; these follow-up medians are separate and must not be mixed into that matrix.

### Spatial

| Model | 480 × 360 | 640 × 480 | 768 × 576 | 1280 × 720 | 1920 × 1080 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx | 1164.3 | 861.6 | 679.8 | 382.2 | 151.7 |
| 2x_AnimeJaNai_HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx | 1491.5 | 1172.9 | 933.9 | 620.9 | 280.4 |
| 2x_AnimeJaNai_HD_V3.1Sharp1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx | 1171.8 | 871.2 | 689.6 | 382.2 | 150.1 |
| 2x_AnimeJaNai_HD_V3.1Sharp1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx | 1456.5 | 1133.9 | 874.4 | 581.0 | 259.4 |
| 2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo.onnx | 690.2 | 456.3 | 327.1 | 167.2 | 76.0 |

## Reproducing and submitting another machine

1. Use the same application revision, bundled clips, and model names. In **Settings → Components**, install and verify the runtime, matching GPU builder resource, and spatial models before measuring.
2. Run **Settings → Benchmark** with all clips, Anime4K, AnimeJaNai, Spatial, and baseline selected. Leave the **RIFE group unchecked**. Keep unrelated GPU workloads stopped. Record each failure's full detail as well as successful fps values.
3. Include GPU name and VRAM, driver, CPU, RAM, OS, application revision, backend, and input/output dimensions. For custom-model results also provide each graph's SHA-256, exported profile/config, selected backend, and TensorRT logging/engine-type/builder flags; state whether engines were cached or built for the run. Custom profiles may enable per-chain RIFE even when the separate RIFE group is unchecked: record whether chain RIFE is enabled and disable it for a non-interpolation comparison. State any different clips, model revisions, or graphics settings. Do not merge unlike workloads into the same comparison table.
4. Send the result export or screenshots with their model/clip labels. Add each machine as a separate section with the same columns; leave unmeasured cells explicitly unmeasured rather than extrapolating from another GPU.
5. For controlled old/new comparisons, keep the benchmark application foreground on a connected, non-minimized desktop (including over RDP); repeat comparisons and interleave old/new runs rather than relying on one pass. Record the session and focus state, plus whether each model used a cached engine or first-use build. Keep these comparison medians separate from the original single-cell full matrix.

The developer `benchmark_probe` exercises the same Settings surface and can save machine-readable evidence. On Windows it must run beside the deployed runtime, shaders, models, and `benchmarks/` directory; a bare executable from the build directory is not a self-contained benchmark.

From that deployed directory in PowerShell:

```powershell
$env:QT_QPA_PLATFORM = "windows"
$env:QT_OPENGL = "desktop"
$env:MIKAI_PROBE_LANGUAGE = "en"
$ids = .\benchmark_probe.exe --list-models |
    Where-Object {
        $_ -notmatch '^RIFE\t' -and
        ($_ -split "\t")[1] -notmatch '^(spatial:custom:|builtin_slot:)'
    } |
    ForEach-Object { ($_ -split "\t")[1] }
.\benchmark_probe.exe --model ($ids -join ',') --all-clips --output rtx5090-stock-no-rife.json
```

This filter excludes RIFE and the current custom graph/profile IDs, retaining
the stock non-RIFE catalogue selection. Current profile IDs for the three
built-in profiles remain stock entries; their profile chains/configuration may
differ from the historical revision, so inclusion alone does not make results
comparable. Verify the emitted IDs and groups with `--list-models` before
capture; do not assume the live catalogue matches the historical 31-model list. Name current captures explicitly (for example,
`rtx5090-stock-no-rife.json`). For a historical old/new comparison, reproduce
the recorded source revisions and settings; current settings—including custom
profile configuration and Sharp chain behavior—are not interchangeable with
the older revision. `--list-models` lists stable catalogue IDs;
`--model <id,id,...> --clip 1280x720.mp4 --output subset.json` captures a
smaller selection.

For long first-use runs, capture subsets into separate files with `--model <id,id,...> --all-clips --output group.json`. Keep those completed exports so an interrupted process does not require repeating every model. Engine preparation is outside the timed sample, but it can dominate the total capture time.

A complete run can return exit code 1 because a model failed, including a known unsupported model. The JSON still contains every row: require `"complete": true`, then distinguish measured `fps` from `null` accompanied by `reason`. Exit code 2 identifies a setup, argument, or evidence-write error. An incomplete capture must not be presented as a completed matrix.
