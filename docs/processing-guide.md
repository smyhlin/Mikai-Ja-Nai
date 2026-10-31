# Video processing guide

Mikai Ja Nai offers three different ways to process video: Anime4K shaders, Standard AnimeJaNai, and the separate Prepared Native workflow. Ordinary playback does not require any of them. Choose based on your platform, available components, and whether you want a shader preset, a Standard AnimeJaNai profile/model, or a prepared Native bundle.

For the comparison workstation and its controls, see [Compare and GIF export](player-guide.md#compare-two-processing-results). This guide covers processing setup and playback selection, not Compare usage.

## Optional video processing and components

| Workflow | What it is | Platform and prerequisites |
|---|---|---|
| **Anime4K** | Built-in GLSL shader presets using the OpenGL rendering path. No AI model download. | Windows and Linux; requires a usable OpenGL rendering path. |
| **Standard AnimeJaNai** | The regular AnimeJaNai manager, profiles and spatial models using the AnimeJaNai filter in the app's pinned mpv runtime. | Linux requires NVIDIA and TensorRT. Windows offers the Standard backends shown in Components; TensorRT is the NVIDIA path and DirectML is Windows-only. |
| **Prepared Native** | A separate, private Native bundle/recipe workflow with distinct preparation, verification and bundle admission. It is not a Standard AnimeJaNai profile. | Windows-only in the app. The Native backend, recipe and required assets must be available for the current build and machine. |

The availability of a selector, imported graph or installed component does not prove that a model will execute, that inference occurred, or that playback will meet a particular frame rate. See [Native documentation](../native/README.md) for architecture, provenance and store details, and [AnimeJaNai runtime notes](animejanai.md) for Linux runtime failure modes.

## Where processing data is stored

The application resolves its native data root in this order:

1. `MIKAI_NATIVE_ROOT`, if it names a directory;
2. `<applicationDir>/native`, if the application can actually create and remove a write-probe file there;
3. Qt's `AppLocalDataLocation/native` (typically `~/.local/share/Mikai/Mikai Wrapper/native` on Linux).

The root must be writable for acquiring and using components/models. A read-only AppImage mount or protected installation directory should use the user-data fallback; do not change permissions on a system directory to force storage beside the executable. Application startup removes only stale work files and staging trees, not installed components, models or configuration. See [the native component store documentation](../native/README.md#component-store-ownership-pack-membership-and-cleanup).

## Components: acquire and verify

The recommended setup omits optional RIFE interpolation models by default. Select them separately if you need interpolation.

1. Open **Settings → Components**. Let the inventory refresh so the UI can show installed/recommended state.
2. Select the items you need. **TensorRT runtime** and GPU-matched **TensorRT kernels** may be recommended for the detected NVIDIA GPU; a compatible resource in the validated installed runtime or app-owned component store satisfies the GPU-kernel requirement. **AnimeJaNai upscaling models** comprises the five pinned HD and SD graphs used by the built-in Standard profiles. **RIFE interpolation models** are optional and are not needed for upscaling alone.
3. Click **Apply changes** and allow acquisition to finish. These are optional AI components; ordinary playback works without them. Downloads are verified before installation. The UI displays progress/status for the selected work.
4. Click **Verify** to re-check installed components and model files against their recorded digests. Treat a missing or failed verification result as unavailable until repaired; “installed” and “verified” are distinct states.
5. Use **Refresh** to update the displayed inventory. On Windows, **Install local runtime...** selects a local AnimeJaNai runtime directory for the selected backend closure; this is a separate action from acquiring the listed packages. Use **Cancel** on the active Components tab to cancel acquisition or verification. Closing Settings does not cancel an active acquisition. Cancel may leave an item absent or unverified; refresh and verify the resulting state before selecting it for playback. Changing the AnimeJaNai backend while a verification/acquisition check is in progress changes which runtime is being checked; prior check results are not fresh evidence for the new backend.

Runtime and model availability are platform- and GPU-dependent. Linux Standard AnimeJaNai uses TensorRT and requires its compatible runtime closure. Read [AnimeJaNai on Linux](animejanai.md) before diagnosing a Linux inference-context or engine-build failure. Do not substitute an unrelated system mpv: the app uses its pinned runtime.

## Standard AnimeJaNai profiles and custom models

### Edit and save a profile

1. Open **Settings → Processing** and select **Standard**.
2. In the AnimeJaNai manager, select the profile/slot and chain you want to edit. Configure the available model steps, chain resolution/FPS bounds and any optional interpolation or resizing settings.
3. Commit the current chain's editor values before changing to another slot or chain; save the manager configuration when the edits are ready.
4. Use the manager's profile import/export controls when you need to move profile configuration. `.pconf` and `.conf` files contain configuration, not model graphs, runtime components or compiled TensorRT engines.

Manager/profile edits are deferred until **Save**. Custom model import, external registration and removal are immediate actions and are not undone by discarding unsaved profile edits. A failed config import leaves the current edits intact. The manager stores custom profiles in slots 1–10; profiles may contain ordered chains with resolution/FPS activation bounds. Chain selection is based on source geometry and FPS metadata, not the filename or transformed output size.

Sharp selects the corresponding HD model variant; it does not replace the SD branch. TensorRT shape/builder settings control preparation, not backend choice or graph compatibility. The built-in Static and Dynamic shape helpers use the ONNX input name `input`; graphs with a different input name may require compatible raw shape arguments or Static ONNX with an appropriate fixed shape.

In a chain, **Enable ONNX upscale steps** toggles ONNX upscale steps only; it does not disable Anime4K steps. ONNX steps expose **Temporal window frames** (0 means detect the window from ONNX; 1 means a single frame; the tooltip recommends an odd number up to 15) and **Temporal scene threshold** (0 disables scene-cut protection). Anime4K steps do not show those ONNX temporal controls. The separate **Temporal fix ONNX model** field takes a registered ONNX model identifier; leave it empty to disable that fix. Its **Temporal fix scene threshold** is adjustable, and **Temporal fix before upscaling** controls its placement. Do not confuse these controls with the separate per-chain RIFE interpolation settings.

### Import an ONNX model

1. Open **Settings → Processing → Standard** and choose **Import .onnx Model…**.
2. Select the self-contained spatial ONNX graph. The app imports it into its owned model directory, records integrity/provenance metadata and, when registration completes, makes it available in the model catalog, player spatial-model menu, profiles, Compare and Benchmark.
3. Confirm the model is shown as registered before using it. Use the manager's **Remove Selected** only when you intend to remove an app-owned imported model or its registration.

An external-model directory is an optional candidate-browsing location. Registering an external graph records a reference and digest; the file is borrowed, not copied, modified or deleted. Removing an external registration leaves the graph in place. A graph pasted directly into the owned directory is only an unregistered candidate; select it and use **Import .onnx Model…** to register it before use.

Validation establishes a plausible ONNX graph/provenance record, not tensor, operator, shape, dtype, backend or GPU compatibility. Filename-derived scale/architecture labels are hints. Multi-file external-weight packages are not imported as bundles. Importing/registering a graph does not grant redistribution rights; users remain responsible for applicable model and weight licenses. See [Standard AnimeJaNai model and profile details](../native/README.md#2-standard-animejanai-custom-models--profiles).

## Prepared Native workflow

Prepared Native is not the Standard AnimeJaNai manager, its model staging, or its `.pconf` profiles. On Windows, select **Native** under **Settings → Processing** to reveal its backend, recipe, availability status, Native assets/preparation panel and RIFE controls. Linux does not offer this private Native runtime in the app.

Before starting, confirm that the desired backend/recipe and required Native assets are available. The assets panel is wired to a live-video source-presence check; it reports that preparation cannot proceed when no source-presence path is available. Use the Native assets/preparation UI for the available recipe and source, then wait for its status/progress to settle. The panel's **Cancel** controls apply separately to model import and preparation; cancellation is cooperative, and preparation already publishing its result can no longer be cancelled. Do not treat a generated raw TensorRT plan, an imported ONNX model, an AJI engine build, or a “prepared” label from a different tool as equivalent to verified Native bundle admission.

The Native helper tools have separate roles: `native_inspect` can inspect ONNX and run CPU reference inference; `mikai_native_prep` builds a raw TensorRT plan out of process; `mikai_native_verify` checks plan bindings and numerics; bundle tooling verifies the versioned bundle manifest. `mikai_native_prep` success only means it wrote a non-empty raw plan. Read [Native documentation](../native/README.md) and the [MSVC + CUDA/TensorRT helper notes](../native/build-msvc.md) for the precise contracts and prerequisites. Those tools and checks do not establish unobserved GPU playback or performance.

## Select processing during playback

Processing is optional; playback starts with the **Original** pipeline unless a processing choice is explicitly selected. In the player, open the processing/preset controls and choose the available Anime4K, Standard AnimeJaNai or Native option presented by the active runtime/catalog. For Standard, built-in profiles and registered spatial models are distinct choices; a profile can choose chains according to source resolution/FPS. For Native, the selected runtime, backend and recipe must be available for the running app; changing runtime may require restarting the app as indicated by Settings.

If an option is absent, inspect **Settings → Processing** availability and **Settings → Components** verification before assuming it can be selected. An installed ONNX graph or profile registration does not establish that its backend can execute it. A failed runtime/model setup is an error, not evidence that the GPU is too slow. Ordinary playback remains available without processing components.

## Benchmarking

### Prepare a run

1. Supply the benchmark clips in a `benchmarks/` directory beside the executable. Required exact filenames and dimensions are `480x360.mp4`, `640x480.mp4`, `768x576.mp4`, `1280x720.mp4` and `1920x1080.mp4`. The benchmark clips are not redistributed with the app.
2. Open **Settings → Benchmark**. Select the clips, the unfiltered baseline if desired, and the model groups/individual models to measure. The catalogue may include registered custom graphs and profiles as well as built-in entries.
3. Before a Native/AnimeJaNai model run, acquire and **Verify** the required components/models in **Settings → Components**. Keep in mind that a custom profile may itself enable RIFE even when a separate RIFE model group is not selected.
4. Click **Run benchmark**. Preparation/engine compilation is separate from the timed FPS sample. On first use, engine preparation can take time even though that time is excluded from reported throughput.

### Interpret the output

The app measures rendered output frames per second through its decode/filter/render path at 2× input dimensions. FPS is throughput, not latency, image quality, GPU utilization, engine-build time or a guarantee of sustained real-scene playback. A failed row with a reason is not a zero-FPS measurement; missing components, unsupported models, setup refusal and inference failure must remain distinguished from measured throughput.

Results depend on GPU, driver, backend, clip, output size, model/profile settings, engine/cache state and foreground/display conditions. The historical captures in [`bench_results.md`](../bench_results.md) use synthetic black-frame H.264 clips and are not anime-scene speed guarantees. They are historical captures, not controlled comparisons between GPUs or current model/profile configurations. Do not compare unlike backends, input sizes, profiles, clips or engine conditions as if they were a controlled test.

For a useful report, preserve the generated JSON and note GPU/driver, CPU, OS, application revision, backend, input/output dimensions, selected model/profile configuration, each custom model's full file location and 12-character SHA-256 prefix, and whether engines were already cached or built. Keep failures and their details; do not replace null/failed rows with zero or infer missing results. The exact report destination differs by platform: Windows writes under `bench/` beside the executable; Linux uses Qt's writable `AppLocalDataLocation/bench`.

## Troubleshooting checklist

- **Processing choice missing:** refresh Components, verify installed items, confirm model/profile registration and check the selected runtime/backend availability. Do not infer compatibility from an ONNX import succeeding.
- **Acquisition cancelled or interrupted:** refresh inventory, run **Verify**, and apply/reacquire the missing or failed selected packages as indicated. Closing Settings alone is not cancellation.
- **Linux Standard AnimeJaNai failure:** consult [Linux fork integration and failure modes](animejanai.md), especially its loader-path and shell-safe engine-build path requirements. The engine build log is authoritative for the documented path-splitting failure.
- **Benchmark row failed:** read its failure detail and separate setup/runtime/model refusal from a measured low FPS. Verify the relevant packages and backend; ensure clips have the expected exact names and dimensions.
- **Benchmark seems unexpectedly fast or slow:** check that the same clip, input/output size, model/profile, backend, cache state and foreground conditions are being compared. Black-frame synthetic fixtures do not predict anime playback.

## Further reading

- [Native Enhancement Provenance, Manifests & Component Store](../native/README.md)
- [AnimeJaNai on Linux: fork integration and failure modes](animejanai.md)
- [MSVC + CUDA + TensorRT build notes](../native/build-msvc.md)
- [Historical benchmark captures and limitations](../bench_results.md)
