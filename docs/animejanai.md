# AnimeJaNai on Linux: fork integration and failure modes

The AnimeJaNai filter ships inside the pinned mpv fork used by Mikai Ja Nai. Two of the fork's own
habits decide whether the application's plan can work at all. Both are cheap to honour and expensive
to debug, because the fork reports a single line and the player only relays it.

Provenance, manifests and the component store are documented in [`native/README.md`](../native/README.md);
this page covers the runtime contract and what a broken one looks like.

## 0. Runtime admission is marker-based on both platforms

Windows previously rejected an updated pinned fork before filter attachment because it required
the old exact DLL digest and client API 2.5. The current pinned Windows `libmpv` contains the fork's
`animejanai` filter marker and reports mpv client API 2.6, so neither an exact historical digest nor
an exact API minor version is a valid fork-identification rule.

Admission requires the loaded **Standard** core, the exact resolved module path to be a member of
the validated runtime inventory, the NUL-terminated filter and required `trtexec-libdir` and
`rife-model-dir` option markers, and client API 2.5 or newer.
This matches the Unix marker-based check; the compatible major version is enforced by the mpv API
loader. Stock mpv remains unsupported without those markers. Reaching this admission
check only permits attachment; it is not evidence that TensorRT engine building or GPU inference
works, nor that a package is verified or approved for public release.

***
## 1. The inference context needs the closure on the loader path

`aji_trt` resolves `libnvinfer.so.11`, `libnvinfer_plugin.so.11`, `libnvonnxparser.so.11` and
`libcudart.so.13` by **bare soname** when the filter creates its context. None of them live in a
directory the process loader searches, and the store is not on `LD_LIBRARY_PATH`.

`prepareAnimeJanaiPlan` records the closure folder in `AnimeJanaiPlan::backendLibDirs`, and
`VideoSurfaceAnimeJanai` preloads every `.so` there with `RTLD_NOW | RTLD_GLOBAL` immediately before
`vf add`. `trtexec-libdir` is *not* a loader hint for the filter itself — it configures the child
engine build only.

### TensorRT for RTX has no Linux path

`TensorRT for RTX` is a Windows-only, opt-in Standard backend (`TensorRT for RTX` in the UI,
persisted as `tensorrt-rtx`). No Linux RTX asset is published or offered, so nothing on Linux can
stage one:

- There is no Linux overlay archive and no Linux install or download path for RTX.
- A saved, restored or imported RTX request fails by name — installation/provisioning report
  `TensorRT for RTX is available only on Windows.`, runtime preparation reports
  `TensorRT for RTX is not available on this platform.` It is never clamped to TensorRT or DirectML
  and never silently substitutes the classic closure.
- The classic Linux contract above is unchanged: `libaji.so` + `libaji_trt.so` with the
  `libnvinfer`/`libcudart` closure and the child `trtexec` build. RTX-specific behaviour —
  in-process engine building, no `trtexec`/`trtexec-libdir`, `.trtrtx-<ver>.gpu-<gpu>.rtxengine` and
  `.jitcache` caches, and only the five RTX builder flags — cannot be exercised here.
- On Windows, selecting RTX acquires nothing by itself; an explicit install action stages the
  standard dispatcher plus the five overlay files under `components/aji/v0.24.0/trtrtx`, keeps both
  NVIDIA notices, and refuses a root that also contains `aji_trt.dll` so the dispatcher cannot fall
  back to classic TensorRT. Nothing from that overlay is bundled or redistributed.

## 2. The engine build must receive a shell-safe path

The fork builds TensorRT engines with:

```
/bin/sh -c 'LD_LIBRARY_PATH=<trtexec-libdir> "<trtexec>" --onnx="…" --saveEngine="…" … 2>&1'
```

The prefix is **unquoted**, so a space anywhere in the closure path splits it into an assignment and
a bogus command. The store lives under `…/Mikai Wrapper/`, so the raw path always splits.
`detail::shellSafeLibDir()` returns a symlink mirror of the closure under a space-free temp root
whenever the real path contains a space, quote or tab; both plan builders pass that value as
`trtexec-libdir`. Never collapse this back to `installation.root`.

## 3. Symptoms, in the order they appear

| Symptom (toast / log) | Cause |
| --- | --- |
| `animejanai.core.unsupported` | The loaded Standard core did not resolve the inventoried fork module, required filter/option markers are absent, or client API is below 2.5 |
| `animejanai.backend.failed: Failed to create inference context` | The TensorRT closure was not preloaded; the fork's own line just before it reads `libnvinfer.so.11: cannot open shared object file` |
| `animejanai.build.failed: … (see .build.log)` with `sh: line 1: Wrapper/…: No such file or directory` and `exit code: 127` | `trtexec-libdir` contained a space; `trtexec` never started |
| `animejanai.build.outOfMemory: Not enough free GPU memory …` followed by the log's `[E] … out of memory` line | The engine build ran while another process held the GPU's memory. TensorRT could not allocate a tactic buffer, skipped every tactic and wrote an empty `.engine.tmp`; the fork then reported that as `ICudaEngine::createExecutionContext … [cask_fusion.cpp:397: initialize_cask_fusion] Cask unknown internal error`, which names neither the build nor its cause. Free GPU memory and retry: at 1080×1920 the spatial model needs about a gigabyte of headroom — it built in 14 s with 8 GB free where it had failed with 830 MB. |

The build log beside the engine is the authoritative record — read it before guessing:

```bash
ls -t "$HOME/.local/share/Mikai/Mikai Wrapper/native/animejanai/models/spatial/"*.build.log | head -1 | xargs cat
```

That path is the per-user store; a portable or developer tree keeps `native/` beside the executable instead.

## 4. Reproducing without the GUI

Run the fork against the same store, module and options the application passes:

```bash
S="$HOME/.local/share/Mikai/Mikai Wrapper/native"
M="$S/components/aji/v0.24.0/trt"
MODELS="$S/animejanai/models/spatial"
RIFE="$S/animejanai/models/rife"
LD_LIBRARY_PATH="$M:/path/to/extracted/mpv" \
  mpv --no-config --vo=gpu --ao=null --loop=inf \
      --vf="animejanai=lib=$M/libaji.so:conf=CONF:model-dir=$MODELS:rife-model-dir=$RIFE:trtexec=$M/trtexec:trtexec-libdir=LIBDIR:slot=1002:stats=/tmp/stats.txt:output-444=no:passthrough=no" \
      --msg-level=all=info CLIP
```

`CONF` is the generated plan INI for the profile and `slot` must name a slot section it contains.
`MODELS` and `RIFE` are the built-in preset locations; a mixed or custom chain stages per-stage
model, config, statistics and RIFE directories, and both options must point at the directories that
plan uses.

- `LIBDIR="$M"` reproduces `exit 127` / `No such file or directory`.
- `LIBDIR` set to the mirror — or any space-free directory of symlinks to the same closure — lets
  `trtexec` print `&&&& PASSED TensorRT.trtexec` and write the `.engine`; playback then reports the
  upscaled stream.
- Dropping `LD_LIBRARY_PATH` while keeping the filter reproduces the bare-soname failure — that is
  the state the in-process preload exists to prevent.

Keep the store's own files untouched when testing: the mirror is read-only in effect, and the
verified digests in `components/aji/<version>/` must stay as installed.

## 5. Where the code and the checks live

- Plan construction: `src/player/native/components/AnimeJanaiRuntime.cpp` (`backendLibDirs`,
  `detail::shellSafeLibDir`); the RTX branch emits `backend=TensorRT-RTX`, omits the
  `trtexec`/`trtexec-libdir` options, sanitizes builder flags and hard-fails off Windows.
- RTX acquisition, per-file verification and the Linux refusal:
  `src/player/native/components/AnimeJanaiInstallation.cpp`.
- Attach and preload: `src/player/core/VideoSurfaceAnimeJanai.cpp` (`preloadAnimeJanaiBackendLibs`,
  `animeJanaiFail` logging).
- Session log location: `src/app/support/CrashLog.cpp` — an AppImage mounts its payload read-only, so
  `mikai.log` falls back to `AppLocalDataLocation` (`~/.local/share/Mikai/Mikai Wrapper/mikai.log` here).
- Regression tests: `TestAnimeJanaiRuntime::tensorRtPlanCarriesTheBackendLoaderDir`,
  `TestAnimeJanaiRuntime::shellSafeLibDirAvoidsASplitLibraryPath`,
  `TestDiagnostics::aBlockedPreferredLogFolderFallsBackToThePerUserFolder` (blocks the preferred
  write probe; it does not assume a directory's read-only attribute prevents writes on Windows).
- Pre-release gate for a packaged Linux artifact: [`releasing.md`](releasing.md), *AnimeJaNai gate for
  a Linux artifact*.

## 6. Optional upscaler graphs

The Upscalers page offers four downloaded ONNX graphs (4× Real-ESRGAN AnimeVideo v3 in fp32/fp16, 2× HFA2kCompact and its multijpg variant) as one-step chains. They enter the same custom-model store as an imported graph, so they plan, verify, cache engines and appear in Compare exactly like a spatial model; the chain carries the graph's own factor, and the first render at a new size builds an engine before it can show a frame.

Two pane-specific notes, both fixed but worth knowing when reading an older log: a comparison pane's core has to preload the backend closure with `RTLD_GLOBAL` (the player's surface does that itself), and preloading a second copy of an already resident `libnvinfer`/`libcudart` soname must be skipped — two runtime images in one process corrupt the heap. Teardown of a pane's filter is synchronous for the same reason: removing the filter is what joins the fork's builder thread.
