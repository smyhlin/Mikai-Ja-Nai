# Benchmark results

**Historical snapshot:** the 31-model, 160-cell RTX 5090 matrix and the
2026-10-17 old/new comparison below are preserved historical measurements from
their recorded revisions and settings. They have not been updated for the
custom-model/profile integration. The current benchmark catalogue can additionally
contain imported custom graphs and `builtin_slot:<1..10>` profiles, and current
configuration/Sharp behavior can change which chain/model a profile selects.
Do not treat those dynamic entries or current settings as members of the stock
historical selection, or merge their results into its comparison tables.

The separately dated **retired model-graph experiments** at the end preserve
historical measurements from tools removed by user decision. They are not an
update to the historical stock matrix or a currently reproducible feature.

Measured results from **Settings → Benchmark**, grouped by machine and model. Compare matching input resolutions, backends, and settings—not GPU names alone. Captures currently documented: RTX 5090 and RTX 2070 SUPER. These are independent captures with different/unrecorded settings; their rates are not a controlled GPU comparison.

**Historical matrix scope:** all 31 non-RIFE models—23 Anime4K presets, 3 AnimeJaNai profiles, and 5 Spatial graphs—at all five bundled input resolutions, plus baseline. **RIFE was excluded from that matrix by request.** The later RIFE 4.25-only default/variant comparison is recorded separately below.

> **Workload limitation:** the bundled clips are synthetic black-frame H.264 fixtures, not anime footage. These measurements describe this reproducible fixture workload; they are **not a guarantee of real-scene playback performance**. Black frames omit motion, scene changes, and real-scene complexity. A runtime refusal is not evidence that a GPU lacks performance or VRAM.

## 2026-10-18 — RTX 5090 integration smoke

A bounded Settings run verified automatic JSON export, not a replacement matrix.
Machine: NVIDIA GeForce RTX 5090, SM 12.0, driver 616.92, Windows 11 25H2,
Qt 6.12.0. Input: the bundled black-frame 1280×720 clip; render target:
2560×1440; three-second samples; no RIFE.

| Row | Rendered FPS |
| --- | ---: |
| Unfiltered baseline | 1559.4 |
| Anime4K A (Fast), existing final-shader path | 390.2 |

The Settings action automatically produced one `bench/benchmark-*.json` with
both stable IDs and full-precision numeric FPS. A separate missing-runtime run
also exported its failed Quality row with `fps: null` and the original
installation-record error; that refusal is not a Quality throughput result.

Functional GPU checks separately exercised all nine mixed-chain Anime4K
presets at 2× output, plus A at 1× and 4×. Real tiny Identity ONNX inference
also ran AJI → shader, shader → AJI, and two AJI instances with a shader
between them. The actual PlayerDialog cold-build popup, isolated Compare renderer
and Settings Benchmark all executed a four-stage ONNX → Anime4K → ONNX →
Anime4K profile from a disposable executable-local store. Both engine sessions
completed before readiness; switching Off removed every owned filter. Compare
repeats the same frozen frame to feed buffered filters without changing the
selected source frame. These checks establish execution/order/geometry, not production
model throughput, visual improvement, or DirectML compatibility. Mixed chains
negotiate software NV12 at shader boundaries and may pay transfer and bit-depth
costs; do not compare them as a zero-copy optimization.

Completed Settings runs now save their own JSON beside the executable under
`bench/`, including source/target dimensions, backend, preparation time,
timestamps, GPU/driver/SM, OS and Qt. Share that file with matching configuration
and model provenance when supplying another machine's results.

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
- Each model is tested independently. The historical RTX 5090 full matrix and its follow-up excluded interpolation and combined upscaling/interpolation pipelines. The 2026-10-20 RTX 2070 SUPER capture below additionally contains two RIFE model rows; its export does not record the interpolation factor or all RIFE settings.

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

**Coverage:** 160 unique model/clip pairs: five baseline samples, 115 Anime4K samples, 15 AnimeJaNai samples, and 25 Spatial samples. This historical `benchmark_probe` export reports `complete: true`; all 160 cells have positive finite fps and empty failure reasons. The probe exited successfully with zero failures. Numbers below are fps. This legacy CLI-export flag is distinct from the Settings schemaVersion 1 export documented below, which has no `complete` field.

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

## 2026-10-20 — RTX 2070 SUPER Settings capture

Source report: `benchmark-20261020T144703122Z-c5cf93e4-e728-472c-b6ee-8d60270cb393.json` (Settings export schemaVersion 1). The report is generated output, not a tracked or retained artifact in this tree; the recorded values below are transcribed from it. It records start `2026-10-20T13:49:16.978Z`, finish `2026-10-20T14:47:03.122Z` (57 min 46.144 s elapsed), application 0.1.0, TensorRT backend, NVIDIA GeForce RTX 2070 SUPER, driver 595.71, SM 7.5, Windows 10 Version 22H2, and Qt 6.12.0. **CPU: AMD Ryzen 5 3600 (user-supplied; not present in the JSON).**

**Coverage:** 170 unique model/clip pairs, five clips per each of 34 model IDs: baseline (5), 23 Anime4K presets (115), 3 built-in AnimeJaNai profiles (15), 5 Spatial graphs (25), and two RIFE models (10). Every row has positive finite FPS and an empty failure reason. This structural coverage is evidence of 170 successful exported rows; schemaVersion 1 has no `complete` field, and no such flag is inferred. FPS is rendered output throughput; columns are input resolution, with the same 2× output geometry described above. Each cell is one sample, rounded to one decimal. This independent capture is not a controlled comparison to the RTX 5090: matching settings, profiles, model revisions, drivers, and test environments are not established.

| Model (stable ID / label) | 480 × 360 | 640 × 480 | 768 × 576 | 1280 × 720 | 1920 × 1080 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline (no model) | 1296.3 | 1184.3 | 1066.4 | 831.3 | 474.3 |
| `anime4k:fast_a` — A (Fast) | 391.5 | 380.4 | 348.9 | 280.3 | 165.8 |
| `anime4k:fast_b` — B (Fast) | 394.8 | 354.1 | 340.0 | 273.7 | 169.1 |
| `anime4k:fast_c` — C (Fast) | 496.7 | 493.7 | 452.9 | 378.5 | 222.5 |
| `anime4k:hq_a` — A (High Quality) | 216.2 | 153.6 | 157.6 | 111.4 | 59.6 |
| `anime4k:hq_b` — B (High Quality) | 221.4 | 210.4 | 180.2 | 119.3 | 59.0 |
| `anime4k:hq_c` — C (High Quality) | 311.0 | 311.8 | 306.0 | 196.8 | 106.3 |
| `anime4k:hq_aa` — A+A (Maximum) | 225.5 | 179.0 | 153.6 | 94.0 | 45.7 |
| `anime4k:hq_bb` — B+B (Maximum) | 236.3 | 184.0 | 152.1 | 94.3 | 46.8 |
| `anime4k:hq_ca` — C+A (Maximum) | 309.3 | 262.1 | 221.2 | 135.3 | 65.5 |
| `anime4k:exp_3d_graphics` — 3D Graphics | 903.9 | 866.4 | 786.1 | 616.8 | 342.7 |
| `anime4k:exp_old_film` — Old Film Restoration | 447.4 | 411.2 | 373.1 | 270.7 | 152.1 |
| `anime4k:exp_deblur_strong` — Deblur | 494.0 | 457.3 | 413.6 | 296.0 | 170.7 |
| `anime4k:exp_noise_bilateral_mean` — Denoise · Bilateral Mean | 1336.6 | 1265.8 | 1132.0 | 832.7 | 478.1 |
| `anime4k:exp_noise_bilateral_median` — Denoise · Bilateral Median | 1173.2 | 1170.9 | 1091.0 | 812.2 | 479.8 |
| `anime4k:exp_noise_bilateral_mode` — Denoise · Bilateral Mode | 1209.8 | 1117.4 | 1095.4 | 832.6 | 480.7 |
| `anime4k:exp_thin_veryfast` — Thin Lines · Very Fast | 918.8 | 867.3 | 823.9 | 717.2 | 460.0 |
| `anime4k:exp_thin_fast` — Thin Lines · Fast | 904.9 | 869.4 | 834.6 | 713.7 | 446.2 |
| `anime4k:exp_thin_hq` — Thin Lines · High Quality | 914.2 | 837.3 | 810.8 | 652.5 | 382.0 |
| `anime4k:exp_texture_dtd_x2` — Texture Enhancement · DTD x2 | 609.2 | 575.9 | 526.1 | 425.9 | 263.5 |
| `anime4k:exp_texture_dog_x2` — Texture Enhancement · DoG x2 | 1063.1 | 1038.4 | 954.3 | 770.0 | 468.2 |
| `anime4k:exp_darken_veryfast` — Darken · Very Fast | 901.2 | 860.6 | 799.0 | 672.5 | 428.3 |
| `anime4k:exp_darken_fast` — Darken · Fast | 890.3 | 869.5 | 778.7 | 721.8 | 454.9 |
| `anime4k:exp_darken_hq` — Darken · High Quality | 1009.5 | 948.3 | 827.3 | 640.4 | 422.2 |
| `builtin1001` — Quality | 102.4 | 42.6 | 42.2 | 44.1 | 20.8 |
| `builtin1002` — Balanced | 100.9 | 43.7 | 42.2 | 43.3 | 20.4 |
| `builtin1003` — Performance | 101.8 | 44.5 | 42.1 | 53.3 | 35.2 |
| `2x_AnimeJaNai_HD_V3.1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx`<br>`spatial:sha256:b308e93c3dda3c4f9f968295b2b7b8d1c0583a711bca53a08d51134d9d1cbd5c` | 191.8 | 119.0 | 110.3 | 45.8 | 21.7 |
| `2x_AnimeJaNai_HD_V3.1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx`<br>`spatial:sha256:7b804d87320f37b7269d6e27796046fbd975e95e2aa1f65d5557f52a36282716` | 258.3 | 176.7 | 152.6 | 54.2 | 32.9 |
| `2x_AnimeJaNai_HD_V3.1Sharp1_Balanced_SPANF3_b8f64_unshuffle_fp16.onnx`<br>`spatial:sha256:dde763c8202cfebac5ec030ea6644f3ee339ea35bbda8a76e257405399552f0f` | 179.1 | 122.9 | 109.6 | 51.7 | 23.8 |
| `2x_AnimeJaNai_HD_V3.1Sharp1_Performance_SPANF3_b5f48_unshuffle_fp16.onnx`<br>`spatial:sha256:72246eded3aab52f8d1a71613c0a1d900734b358d1820b62eb36f1ed376e1cf4` | 262.9 | 149.6 | 133.2 | 76.5 | 37.3 |
| `2x_AnimeJaNai_SD_V1beta34_Compact_1x3xHxW_dyn-HW_strong_fp16_op21_dynamo.onnx`<br>`spatial:sha256:ded46591ddfaeb22b3daaf84ad7b36838335aa3efca66acfa36177ce4ab21eb5` | 109.1 | 46.9 | 44.8 | 17.9 | 10.4 |
| `rife:rife_v4.25.onnx` | 343.6 | 255.9 | 215.8 | 117.1 | 59.5 |
| `rife:rife_v4.26.onnx` | 334.0 | 223.9 | 203.4 | 108.6 | 59.3 |

All five bundled inputs appear in the source paths as `D:/mikai-portable/benchmarks/<clip>`; the report does not include content hashes, so fixture identity is not independently proven by this export. The five names/resolutions are shown in the table columns. `enginePreparationMs: -1` means preparation time was absent/unrecorded for that row—not a measured zero and not proof that an engine was cached. Recorded nonnegative preparation values are separately distinct from FPS and excluded from throughput. RIFE output rates include original and interpolated frames; the export contains model IDs for 4.25 and 4.26 but does not record interpolation factor, scene threshold, or other full RIFE profile settings.

The capture does **not** record source commit, RAM/VRAM, full profile/compiler settings, RIFE factor/settings, clip/session/focus provenance, or content digests. Do not infer these from another benchmark, current checkout, model IDs, or hardware specifications. The JSON gives application version but no source revision. The supplied machine shorthand was Ryzen 5 3600; CPU identification above is explicitly user-provided, not JSON evidence. This fixture workload also inherits the black-frame/synthetic-footage limitation stated above.

## Reproducing and submitting another machine

The `complete: true` check below describes the legacy `benchmark_probe` CLI export, not the Settings schemaVersion 1 JSON export. For the latter, report row-level coverage and failures; do not expect or invent a `complete` flag.

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

## RTX 5090 — retired model-graph experiments (2026-10-18)

**Status:** historical experiment record only; the optimization tools and
dedicated comparison probe were removed by user decision. No Python
optimization feature or probe is shipped, and only production correctness
fixes remain. Numerical tables, quality limits, measured methods and hashes
below are preserved unchanged; none of these methods is a supported workflow.

**Good / bad findings:** FP8 was selectively useful but lossy and could also
regress throughput. Sparse pruning offered little benefit with major output
drift. Fusion helped the tested custom multi-model chain; adding cubic
pre-resize did not help and changed resampling. Existing SPAN had no eligible
fold; a new SpaceToDepth/RepConv or luma architecture needs trained weights.
RIFE coordinate specialization was marginal. The tables below retain the
measurements; short CGI runs on one GPU do not establish general speed or
quality guarantees.

### Machine and controls

| Setting | Observed value |
| --- | --- |
| GPU / driver | NVIDIA GeForce RTX 5090, SM 12.0 / 616.92 |
| Installed GPU memory | 32,607 MiB reported by NVIDIA; not a per-model minimum |
| CPU / system RAM | AMD Ryzen 7 5800X3D / 47.9 GiB reported |
| OS / Qt | Windows 11 Pro x64, 10.0.26200 / Qt 6.12.0 |
| Runtime | Standard AJI v0.9.0 / TensorRT 11.3.0 / desktop OpenGL / requested NVDEC |
| Source | Base `0d1f49f`, with the uncommitted optional-tool/comparison changes on `optimizations-flow` |
| Isolation | Dedicated owned deployment; mutable models, configuration and caches cloned separately from `dist-mikai` |
| Interpolation | RIFE disabled in every profile and excluded from all measurements |

Eight corrected comparisons completed, with **64 published steady-state
samples**. Each comparison first ran a separate preparation/capture invocation;
all of its FPS values were excluded. The cached measurement invocation then
ran ABBA twice: four three-second samples per variant. Tables show medians and
the observed minimum–maximum, not confidence intervals. No steady-state sample
reported engine preparation.

Both sides used the same clip, target, TensorRT builder optimization level 5,
and application `runBenchmarkSample` path. Sparse candidates alone added
`--sparsity=enable`. Dynamic source graphs used
`--optShapes=input:%video_resolution%`; fixed-shape fused graphs used Static
ONNX settings without explicit shape arguments. Both resolve to the same
concrete per-stage dimensions. Results describe graph variants; detailed
engine tactics were not captured, so neither sparse-kernel selection nor
universal FP8 tensor-core acceleration is claimed.

The footage was a local **3D CGI** clip, re-encoded to 23.976 fps at 1280 × 720
and 640 × 360. It is not a representative licensed anime corpus or ground
truth. FP8 calibration used eight real frames with 128 × 128 crop/resize
inputs. Four disjoint held-out frames were checked numerically at that size;
one separate full-frame native capture per comparison supplied the image
agreement columns below. No source footage, model weights or screenshots are
included in the repository.

The actual initial comparison window was observed foreground. Continuous
focus/session state, clocks, temperature, power and peak VRAM were not
recorded. Other benchmark/inference jobs were not run concurrently with the
published steady-state measurements. These short comparisons do not prove
sustained interactive playback or cross-machine performance.

### Standard preset model variants

These were source-matched **custom profile clones**, not modified stock
catalogue entries. At 1280 × 720 and 23.976 fps, Standard Quality and Balanced
both select the HD Balanced SPAN graph; Performance selects HD Performance.
At 640 × 360 all three select the SD Compact graph. Those shared paths are
measured once rather than presented as distinct algorithms. Sharp is not
covered.

`PSNR` measures agreement with the original model's 8-bit RGB capture, **not
ground-truth quality**. MAE is the mean channel error on a 0–255 scale.

| Active model / input | Variant | Original median fps (range) | Variant median fps (range) | Change | PSNR dB | MAE |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| HD Balanced / 1280 × 720 → 2560 × 1440 | FP8 Q/DQ | 368.70 (368.31–369.82) | 419.38 (418.80–420.75) | +13.75% | 51.78 | 0.394 |
| HD Performance / 1280 × 720 → 2560 × 1440 | FP8 Q/DQ | 584.78 (580.83–587.27) | 480.17 (480.07–481.49) | −17.89% | 48.90 | 0.615 |
| SD Compact / 640 × 360 → 1280 × 720 | FP8 Q/DQ | 491.28 (489.72–491.75) | 580.44 (579.64–583.16) | +18.15% | 47.85 | 0.567 |
| HD Balanced / 1280 × 720 → 2560 × 1440 | 2:4 sparse | 368.01 (366.42–370.62) | 371.22 (365.41–372.82) | +0.87% | 25.15 | 9.537 |
| HD Performance / 1280 × 720 → 2560 × 1440 | 2:4 sparse | 580.84 (574.91–587.70) | 581.44 (577.93–592.04) | +0.10% | 20.74 | 18.196 |
| SD Compact / 640 × 360 → 1280 × 720 | 2:4 sparse | 490.78 (489.14–492.10) | 503.83 (501.40–505.19) | +2.66% | 37.02 | 1.924 |

FP8 is lossy even where agreement is high. Maximum channel differences in the
native captures were 10/255, 17/255 and 28/255 for HD Balanced, HD Performance
and SD respectively. Sparse HD maximum differences reached 150/255 and
169/255; the small throughput changes do not justify calling it a
quality-preserving replacement. No fine-tuning or energy compensation was
applied.

### Custom multi-model fusion

Reference chains used SD Compact 2× followed by HD Performance 2×. The fused
candidate combined those trained graphs without pruning or quantization.
These are custom workloads: the stock built-in chains above each use one
spatial model.

| Workload / input → target | Sequential median fps (range) | Fused median fps (range) | Change | PSNR dB | MAE |
| --- | ---: | ---: | ---: | ---: | ---: |
| Two-model 4× / 640 × 360 → 2560 × 1440 | 308.04 (306.76–308.30) | 331.43 (330.77–331.89) | +7.59% | 60.15 | 0.063 |
| 75% pre-resize then two models, 3× / 640 × 360 → 1920 × 1080 | 528.20 (504.87–531.79) | 527.57 (523.98–529.58) | −0.12% | 48.89 | 0.350 |

Without pre-resize, the native capture differed by at most 2/255 per channel.
The pre-resize candidate uses ONNX cubic interpolation instead of AJI's native
resizer and differed by up to 50/255; it is not bit-identical. The −0.12%
median difference lies within overlapping sample ranges. No shared-memory/L2
placement or the guide's proposed 15–30% gain is inferred from fusion.

### Applicability, diagnostics and recorded verification

- Existing Standard SPAN graphs already have deployed unshuffle structure; the
  real graph inspection found no eligible Conv3×3 + Conv1×1 + identity pattern
  to fold. The folding tool was checked numerically on compatible fixtures;
  there is no additional stock-model GPU speed result to report.
- A new narrower SpaceToDepth or one-channel luma network needs trained
  weights. None were supplied. RGB/luma wrapping was checked with numerical
  fixtures, but no random checkpoint or unsupported luma speed claim was
  published.
- This spatial-only run did not expose or execute RIFE: numeric pinned
  selection cannot choose arbitrary renamed custom graphs, and specialization
  depends on exact padded geometry. A subsequently requested, separate
  RIFE 4.25-only comparison is documented below; it is not part of these eight
  spatial comparisons.
- The first fusion attempt used inappropriate `--optShapes` with a static
  graph. TensorRT refused immediately, but the old diagnostic gate waited for
  a build timeout. That failed preparation was retained separately and
  excluded from every table. Corrected Static ONNX settings completed.
  The gate now recognizes the authoritative AJI background-build-failed event
  and includes its bounded `.engine.build.log` error summary. A real
  after-fix refusal smoke returned `TensorRT engine build failed` with
  `Static model does not take explicit shapes...`, without waiting for the
  ten-minute timeout. Generic mpv error-level lines remain diagnostic-only.
- Verification passed: application/tests/probe builds, all four CTest entries,
  27 Python graph-tool tests, real sparse/FP8/fusion exports, paired native
  captures, the unsafe-workspace refusal, and the real builder-refusal smoke.
  Hash checks confirmed all five original stock graphs were unchanged and no
  original manager configuration was created.

Local ignored historical evidence is under `.probe-out/aji-optimizations/`:
`comparison-summary.json`, `measurement-provenance.json`, `dataset.json`,
`machine.json`, `specs/`, `results/*-steady.json`, paired capture metrics,
full builder logs, and test/build logs. Every published comparison retains
graph/profile hashes and actual engine flags. Generated graphs, engines,
calibration data and private captures remain outside the source deliverable.
The historical results earlier in this file are unchanged; no RTX 3080 or
RTX 2070 optimization result is extrapolated.

## RTX 5090 — retired RIFE 4.25 experiment (2026-10-18)

**Status:** historical measurement only; the geometry-specific coordinate
specialization and dedicated probe have been removed by user decision. The
stock RIFE graph and production catalogue were not changed. The small measured
gains do not justify replacing the stock default; sample ranges overlap.
No other RIFE version or spatial preset was benchmarked in this run.

| Source / native render target | Padded neural input | Default median output fps (range) | Specialized median output fps (range) | Change |
| --- | --- | ---: | ---: | ---: |
| 1280 × 720 | 1×11×768×1280 | 584.77 (576.32–588.21) | 592.26 (587.62–594.80) | +1.28% |
| 1920 × 1080 | 1×11×1088×1920 | 398.81 (396.83–401.24) | 401.75 (398.14–404.75) | +0.74% |

These are rendered **output frames per second**, including original and
interpolated frames—not neural inferences per second. Source cadence was
23.976 fps; the selected 2× profile produces 47.952 fps at normal playback
speed. Both measured pipelines have substantial throughput headroom for this
clip, but this is not a sustained all-content playback guarantee.

### Controls and interpretation

- NVIDIA GeForce RTX 5090, SM 12.0, driver 616.92, 32,607 MiB installed VRAM;
  Standard AJI v0.9.0, TensorRT 11.3.0, Qt 6.12.0, desktop OpenGL, requested
  NVDEC. Same Ryzen 7 5800X3D/Windows 11 machine as the earlier experiments.
- RIFE **4.25 full**, code `425`, factor `2/1`, scene threshold `0.150`,
  ensemble off and before-upscale on. No spatial model or spatial upscaling.
  Scene detection remains enabled, so scene-cut behavior is part of the
  default pipeline rather than bypassed for this comparison.
- Each geometry had a separate first-use preparation invocation, excluded in
  full from the table. The published invocation used ABBA twice: four
  three-second samples per graph. All **16 published samples** succeeded,
  with no engine preparation during measurement.
- Both sides used the same moving local 3D CGI segment, source interval
  80–110 seconds, re-encoded to matching 23.976-fps clips. This differs from
  earlier workloads and is not representative anime or interpolation ground
  truth. Continuous focus, power, clocks, temperature and peak VRAM were not
  recorded. The actual comparison window was observed foreground.
- Requested RIFE arguments and executed toolchain arguments are distinct.
  Builder logs confirm the same effective `--stronglyTyped`,
  `--optShapes=input:1x11xPHxPW`, `--skipInference` and per-role timing-cache
  settings. The installed toolchain omits upstream legacy I/O-format/tactic
  exclusions. The manager's spatial builder-optimization setting does not
  control RIFE's hardcoded stage; no forced tactic-selection claim is made.

### What the optional graph changes

The actual 4.25 graph splits its input into `[6,1,2,2]`: two RGB frames,
timestep, mesh coordinates and normalization multipliers. Its first Conv
reads **3 channels**, not 11. The guide's proposed 11→7 first-convolution
reduction therefore does not apply to this model.

The optional export embeds only the four coordinate planes for the exact
mod-64 padded geometry. RGB frames and timestep remain dynamic, the external
11-channel ABI remains intact, and all **158 original initializers** are
unchanged. A symbolic input batch axis permits AJI's existing shape override;
the actual engine profile remains batch one. Geometry-specific constants add
model storage; they are not a new trained model or a general batching feature.

Stock and candidate graphs were copied to separate role-and-full-SHA
directories inside the disposable native store, retaining the runtime's
expected filename. Neither the official graph nor its acquisition record was
replaced; no cache-key migration, runtime DLL patch or production RIFE
selection change was needed.

### Temporal correctness and limits

A real adjacent moving-frame pair at each resolution was run through both
graphs at timestep `0.5` using ONNX Runtime 1.31.0 CPU. Centered black padding
matched the native geometry. The cropped midpoint tensors were **bit-identical
at both sizes**: mean and maximum differences were zero. The default
midpoints differed from both source frames, so this was actual temporal
inference rather than a still-image or copied-frame surrogate.

That is CPU numerical agreement on one pair per geometry, not native GPU
pixel-identity proof, a temporal-artifact survey or ground-truth quality.
Four focused experimental regression tests exercised multiple timesteps,
multiple padded sizes, wrong geometry/layout refusal and source preservation;
those tests were removed with the prototype, not retained as current coverage.
The installed 4.25 source digest remains
`de0fb71ae50fe082eb3903fd5cf752063dea5e9fd61e8bbf3c4577e11b877c9c`.

Recorded experiment verification: the comparison completed at both
geometries, and the non-425 request was refused before interpolation or engine
preparation. Original RIFE/spatial graph hashes and the distribution's manager
configuration remained unchanged. No commit or distribution update was made.
No active reproduction instructions remain. Local ignored historical evidence
is under `.probe-out/aji-rife425/`: paired specs, warm-up/steady exports,
`comparison-summary.json`, moving-frame provenance, CPU midpoint agreement,
build/test logs and the source-only refusal smoke.
Generated model variants, the private Python environment, compiled experimental
probe and disposable runtime/engine store were removed after the review.
Numerical exports, builder logs, quality metrics and private media remain as
historical evidence; paths inside old exports are not current installation
paths. This section does not alter earlier tables or establish an RTX 3080 or
RTX 2070 result.
