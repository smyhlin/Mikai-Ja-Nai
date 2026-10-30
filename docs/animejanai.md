# AnimeJaNai on Linux: fork integration and failure modes

The AnimeJaNai filter ships inside the pinned mpv fork, and two of the fork's own habits decide
whether the application's plan can work at all. Both are cheap to honour and expensive to debug,
because the fork reports a single line and the player only relays it.

Provenance, manifests and the component store are documented in [`native/README.md`](../native/README.md);
this page covers the runtime contract and what a broken one looks like.

## 1. The inference context needs the closure on the loader path

`aji_trt` resolves `libnvinfer.so.11`, `libnvinfer_plugin.so.11`, `libnvonnxparser.so.11` and
`libcudart.so.13` by **bare soname** when the filter creates its context. None of them live in a
directory the process loader searches, and the store is not on `LD_LIBRARY_PATH`.

`prepareAnimeJanaiPlan` records the closure folder in `AnimeJanaiPlan::backendLibDirs`, and
`VideoSurfaceAnimeJanai` preloads every `.so` there with `RTLD_NOW | RTLD_GLOBAL` immediately before
`vf add`. `trtexec-libdir` is *not* a loader hint for the filter itself — it configures the child
engine build only.

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
| `animejanai.core.unsupported` | The loaded module is not the fork: no `animejanai` filter marker, or client API below 2.5 |
| `animejanai.backend.failed: Failed to create inference context` | The TensorRT closure was not preloaded; the fork's own line just before it reads `libnvinfer.so.11: cannot open shared object file` |
| `animejanai.build.failed: … (see .build.log)` with `sh: line 1: Wrapper/…: No such file or directory` and `exit code: 127` | `trtexec-libdir` contained a space; `trtexec` never started |

The build log beside the engine is the authoritative record — read it before guessing:

```bash
ls -t "$HOME/.local/share/Mikai Wrapper/native/animejanai/models/spatial/"*.build.log | head -1 | xargs cat
```

## 4. Reproducing without the GUI

Run the fork against the same store, module and options the application passes:

```bash
S="$HOME/.local/share/Mikai Wrapper/native"
M="$S/components/aji/v0.17.1/trt"
MODELS="$S/animejanai/models/spatial"
LD_LIBRARY_PATH="$M:/path/to/extracted/mpv" \
  mpv --no-config --vo=gpu --ao=null --loop=inf \
      --vf="animejanai=lib=$M/libaji.so:conf=CONF:model-dir=$MODELS:rife-model-dir=$MODELS/rife:trtexec=$M/trtexec:trtexec-libdir=LIBDIR:slot=1002:stats=/tmp/stats.txt:output-444=no:passthrough=no" \
      --msg-level=all=info CLIP
```

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
  `detail::shellSafeLibDir`).
- Attach and preload: `src/player/core/VideoSurfaceAnimeJanai.cpp` (`preloadAnimeJanaiBackendLibs`,
  `animeJanaiFail` logging).
- Session log location: `src/app/support/CrashLog.cpp` — an AppImage mounts its payload read-only, so
  `mikai.log` falls back to `AppLocalDataLocation` (`~/.local/share/mikai.log` here).
- Regression tests: `TestAnimeJanaiRuntime::tensorRtPlanCarriesTheBackendLoaderDir`,
  `TestAnimeJanaiRuntime::shellSafeLibDirAvoidsASplitLibraryPath`,
  `TestDiagnostics::aReadOnlyExeFolderFallsBackToAWritableLogFolder`.
- Pre-release gate for a packaged Linux artifact: [`releasing.md`](releasing.md), *AnimeJaNai gate for
  a Linux artifact*.
