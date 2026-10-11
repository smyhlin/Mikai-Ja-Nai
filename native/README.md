# `native/` — source pins, provenance and rights record

Everything the native (AnimeJaNai) track needs in order to be *permitted* and
*reproducible* to exist, plus the acquisition records for what has actually been
obtained, and what each acquisition does and does not prove.

**The Qt/MinGW app build uses the pinned official Windows mpv developer
archive.** CMake acquires or validates `MIKAI_DIST_MPV_ARCHIVE` against the
release SHA-256, extracts the matching headers, and compiles against them. The
build output and `dist-mikai/mpv/` contain only `libmpv-2.dll`, loaded
explicitly through `LoadLibraryExW`/`GetProcAddress`; the app uses libmpv's C
and render APIs in its existing Qt `QOpenGLWidget` GUI, not `mpv.exe` or IPC.
Runtime discovery never downloads mpv or falls back to an arbitrary stock
cache; without the packaged module playback is unavailable. No native SDK is
needed for this ordinary application build.

**The Qt application has two distinct enhancement contracts.** The official
AnimeJaNai ABI-8 filter path uses the packaged API DLL plus an Aji/backend/model
closure; the project's separate private prepared-Native protocol is a different
capability. Neither a component install, a plan-only smoke, nor a configured
choice establishes prepared inference. Keep requested, effective and disabled
states distinct; no fallback/substitution is implied.

**The separate patched Native fork remains a different runtime artifact.**
Its source pin and `GET_META` patch are recorded below; prior isolated-core
load evidence is not evidence about the currently packaged official API DLL or
about processed GUI playback.

## Current observed integration state

**Evidence boundary:** implementation descriptions are not blanket acceptance of
the final UI-cleanup tree. The production source pass was finished before the
following verification; no probes ran during that implementation pass.

### Built-in AnimeJaNai model acquisition repair

The reported built-in-profile refusal on 480p HLS came from an absent installed
`dist-mikai/native/assets` store. Components previously exposed backend, RIFE
and GPU-kernel cards but no spatial selection; its spatial download queue
therefore could not be reached from the UI.

Components now exposes one explicit **AnimeJaNai upscaling models** pack for
all five pinned HD/SD graphs (6,637,479 bytes). Apply preserves the selected
packs/backend; accepted initial setup includes missing spatial graphs. Refresh
does not download or reset subsequent selections. Installation state and
acquisition selection are separate. Verification covers absent catalogue rows,
not just records already present. Closing Settings reprobes the same settled
unavailable request without restarting healthy/pending plans or saving
cancelled preference edits.

**Asynchronous work and shutdown contract.** Startup runtime resolution and
acquisition/verification work run off the GUI thread on the app's worker pools
(shared 4, bulk 3, stream 2, image 12 per runner), with GUI commits handed back
through a copyable dispatch closure: a worker copies only shared state plus the
closure, never a widget pointer, bodies run on the GUI thread, each delivered
body re-checks its owner's alive flag together with a GUI-thread-only address
lookup and drops itself when the owner is gone, and invoking the closure after
its runner is gone is a safe no-op. Every worker body is wrapped in a
work-cancellation scope over its runner's flag, so a verifier or preparation
job aborts at its blocking points (the HTTP wait, the rate-limit window, a
download's own cancel poll); a cancelled build publishes nothing and leaves
prior artifacts and source bytes unchanged. On shutdown the app asks for an
asynchronous drain (`Runner::shutdownAll`) before `QApplication` quits — the GUI
loop stays responsive and the drain's completion runs once on the GUI thread
after every pool has retired — so no backend reference outlives the task that
owns it. Component verification is served from a stat-only cache under the
executable-local native root, so a repeat start does not re-hash the whole store.

**Pending, not failed.** Startup runtime resolution runs as a worker task whose
result is committed on the GUI thread, and the time between the two is a
*distinct* state: the surface reports that resolution is in flight with its own
truthful line rather than a "runtime unavailable" error, never claims a module
exists, and a callback registered against the resolution fires once the terminal
state is published (a destroyed recipient or a dead subscription discards).

The installed source-asset store was repaired through the production
`SpatialAssetStore::publish` API using existing verified original graph bytes.
All five size/SHA-256 checks passed, with provenance/notice retained
(`.probe-out/janai-model-pack-installed-publication-1791679240471.log`);
all 15 pre-existing protected executable/module/component files remained
unchanged
(`.probe-out/janai-model-pack-protected-publication-1791679240471.json`).
This is local acquisition, not a distribution, private Ready/Active claim,
numerical-fidelity proof or licence clearance.

The natural-player probe now accepts an unchanged HTTP/HTTPS media URL instead
of coercing it through `QFileInfo`. Built-in IDs are `builtin1001`,
`builtin1002`, `builtin1003`. It does not infer their active graph from the
first model key in a multi-slot configuration. `--require-engine-build` refuses
built-in profiles with `builtin-selected-engine-identity-unavailable`; normal
built-in playback still requires real observed processing and rendered frames.
Fresh selected-engine identity evidence remains supported for individual
spatial/RIFE requests.

Observed after the repair, on the user's unchanged HLS source:

- Separate natural-player runs of Quality (`builtin1001`), Balanced
  (`builtin1002`) and Performance (`builtin1003`) exit 0. Each selects the real
  menu, observes backend spatial processing and moving/nonblack rendered output,
  removes the filter through Off, reopens the same source and closes cleanly.
  Input is H.264/NVDEC 640×480 at 23.976 fps; processed output is 1280×960.
  Logs are `.probe-out/janai-hls-1791679240471/exact-hls-natural-player-unobstructed.log`
  and `exact-hls-builtin100{1,3}.log` in the same directory. The source has one
  audio track and no subtitles; alternate-track exercises are explicitly
  not applicable, not passes.
- The uncached Balanced request starts a real TensorRT build, shows an
  indeterminate operation window, writes `pause=true`, reports build completion,
  shows success and then writes `pause=false`. The retained operation image is
  `builtin1002-fresh-build-progress.png` in that directory. Configuration/log
  evidence does not establish an independent active-engine identity event or
  numerical fidelity.
- The actual public Components Apply action downloads all five graphs from
  their pinned upstream URLs into an owned physical helper root, verifies all
  original sizes/digests and retains exact notices/provenance. Only the spatial
  card is selected, 0/5 becomes 5/5, and the visible Ukrainian 760×932 surface is
  unclipped (`.probe-out/janai-public-download-1791679240471/public-ui-spatial-download.log`
  and its before/final PNGs). This is network acquisition, not just direct
  publication.
- Full hermetic CTest passes in 24.45 s: 32 suites PASS, LiveOracle SKIP.
  Missing/corrupt spatial selection and unchanged failed-request recovery have
  consumer regressions
  (`.probe-out/janai-model-pack-hermetic-windows-1791679240471.log`).
  Settings passes 133 checks with no failures
  (`.probe-out/janai-components-final-1791679240471/components-settings-final.log`).
  Its former empty-inventory Verify-success assertion was removed: absent
  spatial graphs are now reported rather than falsely verified.
- The production targets build without warnings/errors; the Ukrainian
  catalogue has 1004 finished entries and zero unfinished. The verified
  executable and catalogue are copied in place, without `dist`, and the actual
  installed app paints a readable Ukrainian feed and closes normally, exit 0.
  Native-DPI screen evidence is
  `.probe-out/janai-installed-startup-native-dpi-1791679240471.{log,png}`.
  The final integrity record
  `.probe-out/janai-model-pack-final-integrity-1791679240471.json` confirms
  the intended executable/catalogue replacement and all 14 other protected
  existing files unchanged.

Earlier attempts are not rendering acceptance: the first Windows-QPA fixture
run hung while loading a deliberately fake PE and left an owned
`mikai_tests.exe - Bad Image` CSRSS dialog. That dialog obstructed the next
native menu checks; HWND ownership and its text identified the test fixture.
Only the owned dialog was dismissed, not CSRSS or unrelated windows. The
async fixture now suppresses expected loader-error UI with a scoped Windows
process error mode. A separate one-shot modal-review hazard was bounded;
neither failure is presented as an HLS transport or production-menu defect.
Throwaway publication/download helper sources and binaries were removed after
proof; graph/runtime/model evidence stays under `.probe-out/`.


### Latest observed player and Compare acceptance

The normal API-module contract is unchanged: the official published
`2026-10-07-d6d93599d5` `libmpv-2.dll` is loaded in-process through the C/render
APIs; no new module, external player, IPC, backend cutover or CPU fallback was
used to fix the captioned image. Public Aji downloads remain v0.9.0; the
selected-model runs use the explicitly imported local custom ABI-8 closure.

The reproduced lower-half stripes are fixed at the Qt → libmpv state boundary,
not by relaxing a pixel oracle. Before the fix, actual entry-state logging
observed `GL_BLEND` enabled and the first separated-chroma texture contained
378/740/474/959 non-finite R/G/B/A values. `VideoSurface::paintGL` now disables
blending before rendering. The same published DLL, NVDEC decoder, backend and
cached engine produce a clean held captioned frame with finite color/chroma
intermediates, including after subtitles are restored. Exact before/after
logs and images are preserved in the source-first chronology below.

Latest fresh menu-driven builds on uncached source geometries:

| Selected model | Source → output | Paused hold | Pause / FBO samples |
| --- | --- | --- | --- |
| AnimeJaNai HD V3.1 Balanced SPAN | 544×320 → 1088×640 | 17.767 s | 69 / 68 |
| RIFE v4.14 lite | 640×352 → 640×352, observed 2/1 interpolation | 34.511 s | 139 / 138 |

Both observe request-bound new engines and passed builder logs, stable source,
media time and native framebuffer during the hold, advancing elapsed progress,
successful visible completion and automatic resume. Logs are
`.probe-out/ui-audit-final-cold-spatial-544-1791656532391.log` and
`.probe-out/ui-audit-final-cold-rife-640-owned-1791656532391.log`; inspected
player/FBO images are `.probe-out/final-cold-{spatial,rife}-{player,fbo}-1791656532391.png`.
Clean captioned natural-player and finite-intermediate checks are separately
recorded in `.probe-out/ui-audit-natural-{spatial,rife}-gl-blend-fix-1791656532391.log`.
These are selected-model build/playback observations, not numerical fidelity,
all-47 inference, private Native Ready/Active or an FPS-improvement claim.

Final Compare acceptance passes **464/464**, covering all 24 Anime4K presets
and five spatial models on both sides, immutable Original, exact live-player
pixels/timestamp/preferences, cross-family changes, rapid selection, source
replacement, named backend failure and close/reopen behavior. The production
pooled-close fix clears the frozen source and both outputs; the original
frozen-frame-null regression is restored. The authoritative log is
`.probe-out/ui-audit-final-original-close-regression-all-models-1791656532391.log`,
not an intermediate run with that check removed. Native `SC_CLOSE` independently
passes **9/9** and observes no pending capture/build/release or retained plan
lock/source/outputs
(`.probe-out/ui-audit-final-close-cleanup-native-manager-1791656532391.log`).
The pre-isolation close-cleanup CTest entry passed in 16.65 s
(`.probe-out/ui-audit-final-close-cleanup-hermetic-1791656532391.log`);
that result is not proof that installed component assets are safe.

Other composed post-blend observations remain: H.264 operation 93/93,
real-pointer/PiP interactions 180/180, isolated Settings 129, RIFE settings 56,
NativeMenu 33, missing-runtime refusal 52, external media 22 and HLS recovery
45. Earlier failures remain historical below; none of these passes establishes
numerical fidelity, private Ready/Active or redistribution clearance.

### Installed test-fixture incident and recovery

`nativeRoot()` resolves `<applicationDir>/native`, independent of CWD,
application name or `QStandardPaths`. Copying `mikai_tests.exe` into a
DLL-complete installed directory is therefore unsafe. The direct installed
run logged 32 suites PASS plus `TestLiveOracle` SKIP, exit 0
(`.probe-out/verify-dist-hermetic-resolved-1791675034866.log`), but
`TestAnimeJanaiRuntime` fixtures removed/replaced Aji data under that same
installed native root. The incident inventory records 17 synthetic payload/
record files: ten TensorRT payloads were 90-byte fixture data, its `install.json`
was the 36-byte plaintext `prior installation remains untouched`, and DirectML
was left with five 90-byte files plus a 735-byte synthetic record. A visible
Ukrainian feed and normal application exit did not prove component preservation.

The authorized recovery copied an existing **1400-byte local custom ABI-8
TensorRT manifest** byte-for-byte from
`.probe-out/menu-spatial-smoke/native/components/aji/v0.9.0/trt/install.json`
and restored all ten matching payloads (721,542,477 bytes total) from
`.probe-out/aji-local-runtime-trt/`. Every restored size/SHA-256 matches that
manifest. Its SHA-256 is
`7e0e65b049dee8c6170e758d848d86de479bf0af3acec0ad47e81a7a40aff490`;
the recovered 622,592-byte backend remains
`cda06b490141655c7f0401f7d0d895a4c9216d6c551354f58e663baa2a08b6d3`.
The recovery report verifies that the app executable, published API DLL,
RIFE install record and TensorRT component descriptor stayed unchanged.

This is **not** byte-exact restoration of the unavailable prior 2138-byte
install record (SHA-256
`55a012c1db36c8c26686d75c743c35c30381994b917d5e612d5dfec96ceacda0`).
That record's validity remains unknown. The earlier fixture-restoration report
checked 90-byte `build/release` payloads against a preserved installed record;
it does not prove the old installed record or payloads were fake.
The six DirectML files were proven synthetic fixtures and were removed, along
with their empty directory, only after matching the preserved snapshot by
checksum. Removal was file-specific and directory removal nonrecursive; no
DirectML installation was published. Exact evidence is
`.probe-out/installed-aji-recovery-1791675481383/synthetic-directml-cleanup.json`.
Genuine installed DirectML presence before the run is **unobserved**: there is
no pre-run DML snapshot. Do not infer that a real DirectML installation was destroyed or that
restoring one is required. Local SDK/NuGet payload candidates do not establish
an authentic intended installed manifest; no guessed package or reconstructed
record is published as a restoration. The exact bounded evidence is
`.probe-out/installed-aji-recovery-1791675481383/directml-local-source-audit.json`.

Exact damaged snapshots, per-file inventories, protected hashes and recovery
outcome are in
`.probe-out/installed-aji-recovery-1791675481383/recovery-report.json`.
An independent audit of all ten restored TensorRT payloads and the installed
manifest is recorded in
`.probe-out/verify-restored-installed-baseline-1791675034866.json`.
Matched physical license texts are not redistribution permission, and digest
recovery is not runtime inference or private Ready/Active acceptance.

After temporary test files were removed, the actual cleaned installed app
started, displayed a visible Ukrainian feed with readable posters/text and
closed normally, exit 0
(`.probe-out/verify-clean-installed-app-1791675034866.log`).
Its inspected 2186×1591 image is `dist-mikai/packaged-app-startup.png`.
The final post-smoke report
`.probe-out/verify-cleanup-final-1791675034866.json` confirms all 15 protected
installed files match the adopted recovered baseline, all 33 retained screenshot
files match the cleanup mapping by bytes/hash, zero owned scratch directories
remain and the temporary test executable/library/platform plugin are absent.
These observations establish clean startup and the recorded integrity boundary,
not processed inference or restoration of the unavailable original metadata.

### Isolated hermetic execution and artifact locations

Use the application README's [Tests](../readme.md#tests) commands. CTest keeps
the name `mikai_tests` but launches `mikai_test_launcher`, which stages only
the test executable, shaders and the configured pinned API DLL into a unique
`build/release/_test-runs/mikai-tests-XXXXXX/` directory. It runs with a fresh
native root and removes that owned directory on every return path — success,
suite failure, child crash, timeout kill or staging failure. The launcher applies
an internal deadline (`MIKAI_TEST_TIMEOUT_MS` in milliseconds, default 600000),
kills the child on expiry — inside a Win32 Job object on Windows so grandchildren
die with it — and exits nonzero (2 for a timeout) even when the killed child
reported 0. Direct `mikai_tests.exe` invocation refuses an existing nonempty or
unsafe adjacent native tree before starting suites; a renamed app or changed CWD is
not an isolation substitute.

The matching configured Qt runtime `bin` supplies `Qt6Test.dll` through
`PATH`: it is a test library, not a plugin. `qoffscreen.dll` is the platform
plugin under Qt `plugins/platforms`. The launcher supplies those exact plugin
directories through `QT_PLUGIN_PATH` and `QT_QPA_PLATFORM_PLUGIN_PATH`, pins
`MIKAI_MPV_MODULE` to its staged DLL and defaults an empty QPA selection to
`offscreen`. Explicit inherited QPA and live-oracle opt-in remain deliberate
inputs; unset `MIKAI_LIVE` for hermetic runs. Neither test-only Qt dependency
belongs permanently in `dist-mikai/`.

After source integration, the composed isolation build exits 0 with no errors
or warnings (`.probe-out/verify-fixture-isolation-build-1791675034866.log`).
Verbose CTest exits 0 in **19.40 s**, with 33 suite lines: 32 PASS plus
`TestLiveOracle` SKIP
(`.probe-out/verify-isolated-hermetic-1791675034866.log`).
That run includes the destructive-fixture guard regression and observes the
unique executable directory and owned cleanup. Actual suite filtering/
forwarding with `TestStreams` passes, exit 0
(`.probe-out/verify-isolated-suite-filter-1791675034866.log`).
A deliberately unknown QTest function correctly fails, exit 1
(`.probe-out/verify-isolated-failure-propagation-1791675034866.log`), proving
child failure propagation rather than a false green launcher. Both cases leave
zero owned scratch directories. These results do not retroactively make the
earlier installed-folder suite safe or prove installed inference.

The corrected runner was also exercised physically beside the actual installed
native assets, targeting `cancelledImportDoesNotPublish`. It refuses with exit
**2 before any suite runs**, and all 15 protected files (ten TensorRT payloads,
three records, app executable and API DLL) retain their SHA-256 values
(`.probe-out/verify-installed-direct-test-refusal-1791675034866.log` and
`.json`). Temporary `dist-mikai/mikai_tests.exe`, `Qt6Test.dll` and
`platforms/qoffscreen.dll` were then removed. This verifies refusal and the
adopted recovered 1400-byte baseline, not restoration of the old 2138-byte
metadata or inference acceptance.

Probe logs, screenshots and owned staging/recovery evidence belong under
`.probe-out/`, not the repository root. Existing archived screenshots remain
evidence, not trash. Run screenshot-producing probes with an owned artifact
directory as their working directory; relative screenshot names otherwise
pollute the root. The completed cleanup removed all 32 root PNGs: 14 unique
images were moved unchanged into `.probe-out/root-screenshots/`, and 18 root
duplicates were removed only after exact-byte matching to existing archived
copies. One legacy `build-cmake/native_menu_probe.png` duplicate is retained at
`.probe-out/menu-spatial-smoke/build-cmake/native_menu_probe.png`; the now-empty
`build-cmake/` scratch directory and empty `$env` file were removed
nonrecursively. The exact old→new paths, sizes and digests for all 33 images are
in `.probe-out/root-screenshots/cleanup-manifest.json`. Existing probe evidence,
staging/backups, media and installed native assets were not cleanup targets.
Installed `dist-mikai/packaged-app-startup.png` remains a separate installed-app
artifact, not a root screenshot.

### Source-first follow-up (chronology; later results above supersede open gates)

The initial source-freeze build passed with zero warnings/errors
(`.probe-out/ui-audit-final-reachable-build.log`). The subsequent desktop-OpenGL
and PiP fixes are compiled into the app, tests and relevant probes. The parallel
build ran out of compiler memory at `dialog_probe`; the remaining targets
completed with `--parallel 1`, zero warnings/errors
(`.probe-out/ui-audit-desktop-player-build.log` and
`.probe-out/ui-audit-desktop-player-remaining-build.log`). That CTest passed in
15.92 s (`.probe-out/ui-audit-desktop-final-hermetic.log`). `lupdate` found 997
existing strings and no new strings at source freeze.

The isolated Settings run passes 129 checks
(`.probe-out/ui-audit-final-isolated-settings.log`); the inspected
`.probe-out/menu-spatial-smoke/native-processing-uk-compact.png` shows the single Runtime module selector and
no redundant Native checkbox or private controls under Standard. RIFE settings
passes 56 checks (`.probe-out/ui-audit-sourcefreeze-rife_settings_probe.log`).
Missing-runtime refusal passes 52 checks with no skips
(`.probe-out/ui-audit-final-runtime-refusal.log`).

Settings isolation uses a fresh executable directory, because `nativeRoot()`
resolves `<exeDir>/native`, not a different `QStandardPaths` location when the
application name changes. The Verify fixture now refuses to overwrite existing
selected or inactive Aji records. The earlier direct-build-root run's one Verify
failure was a fixture precondition failure; the corrected isolated run passes.

The source changes re-arm an active engine operation's pause hold on the current
source and owner, retain the latest explicit Play/Pause intent across PiP and
source changes, and prevent late load completions from replaying older intent.
The resumed real-core gate caught one further defect: parking the main core
overwrote held Play before PiP inherited it. PiP now receives the intent captured
before parking. The existing gate failed that handoff before the fix; afterward
all 93 operation checks pass on its real lavfi source
(`.probe-out/ui-audit-desktop-engine-intent-lavfi.log`). All 172 player interaction
checks also pass (`.probe-out/ui-audit-desktop-interaction.log`), including pooled
PiP geometry and pause restoration. These are not actual TensorRT-build proof.

The earlier disconnected H.264 operation rerun failed initial readiness
(`.probe-out/ui-audit-desktop-engine-intent-fixed.log`); its startup cause remains
unproven. With the console attached, the same H.264 operation gate passes all
93 checks (`.probe-out/ui-audit-resumed-h264-operation-1791635749077.log`).
The independent surface/shell result also passes; the shell covers 10 checks
(`.probe-out/ui-audit-desktop-shell.log`).

Qt's automatic OpenGL selection used Mesa 11.2.2 / llvmpipe OpenGL 3.0 after
session 1 disconnected, and mpv refused that context. Forcing
`QT_OPENGL=desktop` supersedes the blanket rendering blocker: the canonical clip
renders with NVDEC, moving nonblack pixels and zero decoder/VO drops
(`.probe-out/ui-audit-final-forced-desktop-opengl.log`); its inspected image also
shows the subtitle normally. Production `main()` now sets
`Qt::AA_UseDesktopOpenGL` before constructing `QApplication`. No software
fallback or weaker pixel tolerance was added.

The resumed reference run used the attached console. Its first capture with an
extra `--dpr 2` ran at DPR 4 and captured Explorer rather than the video; its
failed oracle is not playback-corruption evidence. The reference probe now keeps only its own window
above other apps and uses the monitor's native DPR. At 1366×975 logical / DPR 2,
6.333 s / frame 152, the physical screen is captured before the FBO and passes
5/5 requirements: 4,196,051 of 4,196,352 reference-lit pixels remain visible
(99.99%). The inspected image is clean
(`.probe-out/ui-audit-resumed-visible-canonical-1791635749077.log` and
`.probe-out/ui-audit-resumed-visible-screen-1791635749077.png`). This reference mode disables
subtitles, so at that historical stage it left captioned main/Compare corruption
open; the later blending fix and final Compare passes above supersede that status.
The earlier disconnected all-black capture's cause is still unproven.

Both required fresh engines now build through the actual PlayerDialog →
packaged libmpv fork → locally imported custom ABI-8 Aji TensorRT menu flow.
This local package is separate from the default public v0.9.0 downloads. The native foreground,
cursor and hit-target guards remain enabled. The probe's own player is kept
above other apps; no other application's window or install records are changed.

| Selected model | Natural source | Observed paused build hold | Processed output |
| --- | --- | --- | --- |
| RIFE v4.14 lite | 576×320 / 24 fps | 26.021 s | 576×320; 146 rendered updates over 3 s versus 73 baseline |
| AnimeJaNai HD V3.1 Balanced SPAN | 512×288 / 24 fps | 15.569 s | 1024×576; lower-half stripes reproduced |

Both runs exit 0, verify a new nonempty engine and changed passed builder log
bound to the exact selected ONNX/saveEngine, observe stable media time/source
and framebuffer throughout the hold, then observe visible completion and
automatic resume. The operation is on top, indeterminate, with advancing elapsed
time; no stage percentage is invented. Off clears the filter and its processing
observation; the real playback controls and clean shutdown pass.
Evidence: `.probe-out/ui-audit-current-rife-cold-576-1791635749077.log` and
`.probe-out/ui-audit-current-spatial-cold-512-1791635749077.log`.

Visual inspection after those operation checks reproduced horizontal black
stripes in the spatial player's lower picture band. Frozen images are
`.probe-out/spatial-artifact-before-graph-player-1791635749077.png` (frame 776)
and `.probe-out/spatial-artifact-before-graph-fbo-1791635749077.png` (native FBO,
frame 765). Independent CPU decoding of the exact same input produces clean
frames at both timestamps: `.probe-out/source-reference-512-frame765-1791635749077.png`
and `.probe-out/source-reference-512-frame776-1791635749077.png`.
The existing smoke's nonblack/moving predicate did not catch this corruption.
That run proved the fresh engine/build hold, not correct spatial pixels. The
stripes were a reproduced defect at that stage, not then attributable to
numerics, CUDA, Qt or a driver. The later GL-state captures and blending fix
below resolve this defect. The RIFE image was clean at its recorded timestamp.

The exact live Aji backend and cached spatial engine were also exercised without
mpv or Qt using the existing upstream harness, built without changing backend
bytes. It processed the CPU-decoded NV12 frame 765 twelve times under the same
configuration, slot and color tags. Both CUDA graph and plain inference exited
0; their final 1024×576 NV12 frames were byte-identical, and the graph's first
and last frames were identical. The inspected
`.probe-out/spatial-frame765-graph-1791635749077.png` is clean. Logs are
`.probe-out/ui-audit-spatial-headless-graph-closure-1791635749077.log` and the
corresponding `plain-closure` log. Backend and engine hashes stayed unchanged.

This isolates neither a cause nor live-path acceptance: the harness uses packed
planes, a primary CUDA context and per-frame synchronization, while playback
uses decoder pitches, an adopted context and an asynchronous depth-three queue.
At that stage live context, stride, ordering, lifetime and CUDA-to-GL publication
were still candidates. The later GL-state proof identifies the stripe cause.
A GUI no-graph attempt failed its foreground/input guard
before model selection and therefore supplies no graph-versus-plain evidence.

The later active-frame triplet observes the actual paused spatial playback at
24.750 s / frame 594 under one unchanged source epoch, filter generation,
`nvdec` decoder and empty Anime4K chain. Its raw post-video-filter 1024×576 frame
is clean, while both surrounding 1920×1080 OpenGL framebuffer captures retain
the lower-half stripes. Frozen files are
`.probe-out/spatial-active-{fbo-before,raw-postvf,fbo-after}-1791656532391.png`;
the capture log is
`.probe-out/ui-audit-spatial-active-raw-triplet-1791656532391.log`.
This narrows the observed difference to the displayed/imported frame versus the
clean postfilter frame. It does not rule out an earlier publication race:
a paused renderer can reuse an already uploaded texture after the raw download.
The running author core is `d6d93599d59069b715013a6e77e898056499b905`, not the
separate private SDK/fork source; its decode-event ordering, ticket wait before
publication and full CUDA-to-GL copy synchronization are present.

The stripe cause is now reproduced at the Qt → libmpv GL-state boundary. Actual
entry-state logging observes `GL_BLEND` enabled before every sampled `paintGL`
render. The published core's first separated-chroma pass does not disable it:
finite RG8 source samples and finite, normalized Lanczos weights are blended
into an undefined RGBA16F framebuffer. The captured first chroma texture has
378/740/474/959 non-finite R/G/B/A values, all in its lower half
(`.probe-out/ui-audit-published-real-gl-entry-state-1791656532391.log`).
The CUDA-GL ownership experiment did not remove those stripes and is not the fix.

`VideoSurface::paintGL` now disables blending before rendering, as required by
the pinned `render_gl.h` contract. The unchanged published DLL, backend and
cached spatial engine then render the same held 24.750 s / frame 594 cleanly
with NVDEC and the second caption track still active. Every captured active
color/chroma plane is finite; subtitles off/restored preserve the clean image.
Evidence is `.probe-out/ui-audit-published-gl-blend-fix-1791656532391.log` and
the inspected `.probe-out/gl-blend-fixed-{fbo-before,subtitles-restored}-1791656532391.png`.
The opt-in `animejanai_probe --capture-processed-raw --capture-gl-textures`
now rejects non-finite output-sized color/chroma planes rather than accepting
mere motion. SDK-only admission and the unused native patch were removed:
no new DLL, CPU fallback, shader substitution or tolerance change is needed.
Natural-player and Compare acceptance after this fix are recorded separately;
the earlier captioned-player diagnostic incorrectly judged caption pixels
against an uncaptioned reference, so that throwaway mode was removed, not weakened.

On the composed post-fix binaries, natural spatial and RIFE selections also
pass with clean inspected captioned frames and finite chroma/color intermediates
(`.probe-out/ui-audit-natural-{spatial,rife}-gl-blend-fix-1791656532391.log`).
The real H.264 operation API passes 93/93 with no skips or unavailable
instrumentation (`.probe-out/ui-audit-gl-blend-operation-pause-1791656532391.log`).
The first interaction run exposed a probe defect: it sent synthetic Enter/Leave
events while the actual pointer remained over the float. The corrected probe
moves and observes the real pointer, without changing production PiP behavior,
and passes 180/180
(`.probe-out/ui-audit-gl-blend-real-pointer-interactions-1791656532391.log`).
The app, tests and relevant probes build without warnings/errors, and the full
hermetic CTest entry passes in 17.60 s
(`.probe-out/ui-audit-gl-blend-{composed-build,hermetic}-1791656532391.log`).

The exact-author SDK core initially refused loading with Win32 126 despite its
staged dependency closure. A direct Windows discriminator loaded and unloaded
the same core, GGML, FreeType and curl bytes when only `/` changed to `\`.
`MpvApi` now converts validated module and dependency paths to native separators
at the `LoadLibraryExW`/`AddDllDirectory` boundaries without widening the search.
The previously refused core passes the real loader/async/swap-refusal gate
(`.probe-out/ui-audit-exact-author-unpatched-loader-after-native-path-1791656532391.log`).
This resolves that error-126 scenario, not the historical unresolved error 127.
The app/test rebuild has zero warnings/errors and full CTest passes in 16.32 s
(`.probe-out/ui-audit-native-loader-boundary-hermetic-1791656532391.log`).

The keyboard and three-dot footer buttons are removed. Their supported actions
now live in the video's right-click menu, with the local containing-folder action
offered only for an existing file. Actual right-click, keyboard-row activation,
Escape and `?`, local `#` filename versus streamed-source availability, and the
existing playback/PiP flows pass all 178 interaction requirements
(`.probe-out/ui-audit-context-menu-interactions-1791656532391.log`).
The app and tests build without warnings/errors, CTest passes in 18.32 s, and
the rebuilt Ukrainian catalogue has 998 finished strings and none unfinished.
Those context-menu results alone did not clear spatial pixels or Compare;
the later blending and final restored-close evidence above does.

The preceding 512×288 RIFE build did complete and process, but its probe exited
1: a stale prior stats file prematurely satisfied the effect predicate, so the
pause assertions ran before build completion. The probe now requires current
backend-observed processing before accepting that stats evidence. The passing
576×320 run uses an uncached shape without deleting the completed 512 engine.

Engine-to-active-model identity remains inferred from the exact live config,
fresh artifact and backend report because Core emits no engine-ID event.
These selected-model results do not establish numerical fidelity, all-47
inference, private Native Ready/Active, or redistribution clearance. Captioned
player and final Compare acceptance are now covered by the later results above.

An actual rebuilt app's URL menu opened its modal dialog, but synchronous UIA
invocation timed out; native-input acceptance is not asserted. Only that owned
app and dialog were closed, exit 0; no user processes were stopped.

At that earlier executable-only update, no `dist` target was run. The executable
matched the build and the three recorded component digests were unchanged
(`.probe-out/ui-audit-desktop-installed-update.json`). The installed app painted
a visible Ukrainian feed and closed normally, exit 0, without a `QT_OPENGL`
override (`.probe-out/ui-audit-desktop-installed-startup.log`). This historical
preservation/startup observation does not cover the later installed test-fixture
incident or establish processed playback; recovery is documented above.

### Earlier cleanup observations

Earlier Compare runs recorded 343 shader checks and 460 spatial checks, each
exiting 0. Settings 129 and RifeSettings 57 checks were also observed. Earlier
handoff observations include NativeMenu 33, a cached selected-RIFE run with 23
checks and a five-spatial source-isolated Compare run with 107 checks; these are
not fresh-engine proof or final-source acceptance.

The earlier player-flow build (`.probe-out/ui-audit-playback-failure-cause-build.log`)
compiled the app, tests and probes with exit 0, zero errors and zero warnings. Its
root fix: a synchronous `MpvError` from the surface's own open was overwritten by
failed-open callers' generic "Could not play" text. All eight callers now share
one central helper that preserves the named cause, no Aji plan is applied from a
core that failed to open, and a refusal raises `QAccessible::Alert`. On the
baseline that alert did not exist at all — the gate observed 0 of them and failed
— and the real missing-runtime consumer then hit heap `c0000374`, which GDB
traced to `VideoSurface::~detach` emitting to an already-destructing
`PlayerDialog`; the destructor now disconnects early and guards on `closed_`.

Observed after that fix: the runtime-refusal cause gate passed 52 requirements
with 0 skips and exit 0 (`.probe-out/ui-audit-runtime-refusal-cause-after.log`),
naming the actual missing Root/DLL cause, actually dropping the request and
closing cleanly. Real external-media file-picker, HTTP, same-URL retry and
file-URI playback passed 22/22 with exit 0
(`.probe-out/ui-audit-real-playback-after-failure-fix.log`). Real
expired-signature HLS recovery passed 45 requirements with exit 0
(`.probe-out/ui-audit-hls-recovery-after-failure-fix.log`). The full hermetic
CTest suite after the same fix is 100% in 15.69 s
(`.probe-out/ui-audit-failure-cause-hermetic-after.log`).

The real refused-action gates also pass after the cause fix, on the current
binaries. The player clipboard gate covers 16 checks and the Search clipboard gate
1 under the corrected hidden-HWND Windows-QPA proof; the Search browser gate
passes 1 check under `QT_QPA_PLATFORM=offscreen`. The title browser gate
(`.probe-out/ui-audit-title-browser-refusal-after.log`, exit 0) passes actual
action, visible warning, parent, accessible `Alert`, screenshot and
no-false-success checks with the session intact, and the title copy gate under
Windows QPA passes the same flags
(`.probe-out/ui-audit-title-clipboard-refusal-after.log`). An offscreen browser
also needs a readable font surface: with `C:/Windows/Fonts` on its font path the
readable-surface run passes
(`.probe-out/ui-audit-title-browser-refusal-fonts.log`).

Historical parent gates on those post-cause-fix binaries:

- `InteractionProbe` passes 172/172 with 0 skips and exit 0
  (`.probe-out/ui-audit-free-pip-live-intent-final.log`). M4's PiP change, which
  made an unrequested change to the live float, is reverted; the original
  behaviour stands: after entering the float playing, the probe explicitly pauses
  the float, captures its live state before `Restore`, and checks positions,
  controls, two non-corner free placements at different sizes, pooled reentry and
  pass-through. The earlier 169/2 paused-restore result was a stale probe
  expectation, not a production bug. The chrome-idle wait drives a real
  `QTest::mouseMove` with a bounded wait instead of a synthetic `QMouseEvent`
  plus a 2800 ms sleep.
- The real operation-window API passes 69/69
  (`.probe-out/ui-audit-engine-operation-pause-after-cause-fix.log`).
- Settings passes 129 checks
  (`.probe-out/ui-audit-native-settings-catalog-after-cause-fix.log`, which needs
  the translation catalogue in the environment), RifeSettings 56
  (`.probe-out/ui-audit-rife-settings-catalog-after-cause-fix.log`) and
  NativeMenu 33 (`.probe-out/ui-audit-native-menu-after-cause-fix.log`).

Earlier composition binaries — predating both the native traversal correction and
the runtime-cause fix — recorded the operation-window gate 66/66, the full PiP
interaction probe 171/171, and the hermetic CTest suite in 18.22 s. Their
SearchPage browser and clipboard outcomes are superseded by the after-fix gates
above; only the clipboard gates run under Windows QPA, and the browser gates run
offscreen.

Later evidence supersedes the player-flow open gates recorded at this stage:

- The fresh-build gate is closed for the selected RIFE and spatial models by the
  current natural-player runs above. The earlier 384×224 poll-argument stall
  and 512×288 pre-selection cursor refusal are historical failures, not the
  latest engine outcome.
- The original captioned main-window/Compare stripe defect stayed open after
  the pixel-clock correction below; it is now closed by the blending fix and
  final restored-close acceptance above.

The pixel clock-0 defect is diagnosed and fixed: the capture deferred **one** seek
until `hasVideo` and `duration > 0`, cycled subtitles to none only for the existing
reference fixture, and waited for `QOpenGLWidget::frameSwapped` at the target
timestamp with the window active before capturing. On that basis a windowed
1366×975 logical run at DPR 2 (a 2732×1950 screen) at 6.333 s / frame 152 with
NVDEC (`cuda[nv12]`) took the screen surface first and matched the FBO on 5 of 5
captures with 0 unavailable
(`.probe-out/ui-audit-canonical-screen-first-windowed.log` and its PNG);
4,196,051 of 4,196,352 reference-lit pixels were covered (99.99%) and the
inspected image is clean. The user's first full-screen Alt-Tab occlusion is
acknowledged and excluded. The later full-screen all-black-screen run with a clean
FBO has an **unproven** cause and must not be attributed to DWM or GDI as fact;
the main/Compare diagnostic was pending at that historical stage. The later
attached-console GL-state investigation and final Compare runs are recorded
above; this is no longer the current desktop prerequisite.

The original 6.333 s / frame 152 lower-half-block report was reproduced against a
known fresh CPU-decoded reference at that canonical file, so the input file is not
corrupt. Hardware decode, rendering and on-screen compositing were then candidate
causes; the later GL-entry-state/texture captures separate the stripe cause.

The earlier production catalogue rebuild extracted 997 source strings with
`lupdate` (7 new, 2 obsolete removed); 7 curated translations followed
`lrelease`, and the catalogues finished 997 / unfinished 0
(`.probe-out/ui-audit-catalog-production-build.log`). With that catalogue, `mikai`,
`mikai_tests` and `widget_pixel_probe` build with exit 0 and no warnings or errors,
and full CTest is 100% in 15.71 s
(`.probe-out/ui-audit-current-catalog-hermetic.log`).

That earlier gated application was copied **in place** into
`dist-mikai/mikai.exe` only, with a matching digest; no `dist` target was run. At that time the three
install records — Rife 3.7, Aji 0.9 trt and TensorRt 3.7 — had unchanged hashes;
this does not cover the later test-fixture incident.
The actually installed executable starts, paints a visible Ukrainian feed and
closes normally (`.probe-out/ui-audit-installed-app-startup-current.log`, with the
inspected startup image at `dist-mikai/packaged-app-startup.png`).

The refused-action probe's own selector defect is fixed and its gate now passes:
it had used `findChild<QPushButton*>` while the real control is a typed
`PillButton`, so the probe now finds a `PillButton` parent and drives it with
`QTest::mouseClick`.

The fresh 416×240 Compare run recorded 462 passing checks but overall **FAIL**:
an early live-surface Off restoration measured 0.676/255 against the unchanged
0.05 tolerance. The cause remains unproven; it must not be explained as dithering
or hidden by a weaker threshold. A subsequent canonical startup exited 5 after
only its CLI line, so that invocation supplied no further rendering acceptance.
That exit-5 startup is retained as a historical, unexplained result: a later
normal canonical startup ran under GDB and reported `RESULT: PASS`, which is the
startup outcome at that stage. Neither startup resolved the Off-restoration
failure or established its cause. The later final Compare run passes 464 checks
at the unchanged tolerance; the failed observation remains historical rather
than becoming a retroactive pass.

Subsequent composed build/CTest, PiP, refused-action and runtime results are
recorded above. Installed startup is separate from component integrity, and
the later installed test-fixture recovery has its own explicit evidence boundary.

The earlier menu-driven runs use the packaged fork's libmpv API, the embedded
Qt player, and an explicitly imported local custom TensorRT Aji closure.
Both selected-engine directories began empty. RIFE v4.14 lite built a fresh
9,313,340-byte engine and rendered 145 successful output-frame updates over
three seconds against 73 at baseline (24→48 fps, unchanged 320×180 resolution).
The AnimeJaNai HD Balanced graph built a fresh 3,083,884-byte engine and rendered
320×180 NVDEC input as 640×360 output. The recorded logs are
`.probe-out/menu-rife-smoke/final-observed-cold-proof.log` and
`.probe-out/menu-spatial-smoke/final-observed-cold-proof.log`.

These runs selected the preset through the visible enhancement menu with
`MIKAI_MPV_VERBOSE=0`, exercised playback controls, Off, source reopening and
shutdown, and captured visibly moving Qt/OpenGL output. The HUD now separates
filter acceptance from the pinned backend's processing report and clears that
report on Off/source changes. A stale private Native request neither attaches
the private filter nor paints its status on Standard. Backend log reports are
not numerical verification or private Native admission. The runs contain VO
drop counts and render-stall warnings; they do not establish performance
improvement, numerical fidelity, all-model execution, or private `Ready`/`Active`.
The default downloader still selects upstream Aji v0.9.0, not the local custom
backend used here.

Standard exposes Anime4K and the official AnimeJaNai spatial/RIFE selectors,
not private prepared-Native status or menus. Processing keeps runtime selection
and official entry points; its private recipe/backend editor, model import,
preparation/cancellation, artifact cleanup and exact private RIFE request
controls appear only when Native is deliberately selected or already loaded.
The private inventory refresh is lazy, rather than scanning the store on a
normal Standard settings open. There is no redundant Native checkbox or toggle
shortcut, planned nonfunctional menu leaf, or duplicate provenance/status panel.
Saving visible backend/recipe edits preserves the hidden private enabled request
and unrelated settings; Cancel and live language changes preserve their
respective saved/unsaved state. Components owns explicit downloads.

Compare offers the player's 24 Anime4K presets (including Off) and five public
spatial models, excluding loose shaders and temporal RIFE. Its separate
offscreen libmpv/GL renderer loads one immutable lossless source PNG at the held
timestamp; side processing does not mutate the live player's pipeline. Only
two corner badges choose models, closing their menus before rendering. Model
switches retain the unchanged side; failures name the affected side without
stale labels. Five layouts have mode-specific controls and grid-aware Fit/wheel
zoom. See the application README for the full UI/rendering contract.

PiP retains its own playback core and controls, with only one player core
playing at a time. The GPU-safe float is pooled rather than destroyed; arbitrary
drops do not snap to corners, and hide/reentry preserves position and size.
Only an unreachable placement is recovered into a monitor work area. These are
implementation descriptions; the earlier 172/172 interaction run covered two
non-corner free placements and pooled reentry, and the later real-pointer run
passes 180/180. Actual fresh-engine completion/pause proof is separately recorded
in the latest selected-model runs above.

Engine builds use the always-on-top operation window for real phases, elapsed
time and visible completion/failure. With no backend-reported measurable
progress, TensorRT stays indeterminate: recipe stages are not build percentages.
The actual playback owner stays paused throughout official or private engine
work, retaining explicit Play/Pause intent until terminal acknowledgement;
source replacement clears the old hold. This implementation contract now has
selected-model fresh-build hold/completion/resume evidence above. Earlier
paused-but-stalled and pre-selection-refusal runs remain historical failures.
The public filter's external backend builder and the private preparation
helper's in-process builder are distinct operations, not interchangeable proof.

Earlier verification (before this cleanup): 32 CTest suites PASS plus LiveOracle SKIP;
`native_settings_probe` 102 checks, `rife_settings_probe` 55, and
`native_menu_probe` 86, all exit 0. The English/Ukrainian Processing surfaces
were inspected at minimum and wide sizes, including a live language switch.
`dist-mikai` was rebuilt; its app and API DLL hashes match the build outputs,
and no component/model/inference payload is packaged. Its actual Ukrainian
main window launched with only Windows directories on PATH and the packaged Qt
plugins, then closed normally with exit 0. The compiler build had no warnings;
the translation extractor still reports its existing qualified-`tr()` warnings.

Latest release APIs were rechecked: the official mpv developer archive remains
`2026-10-07-d6d93599d5` (32,830,217 bytes, SHA-256
`077cb75fed47b185428f97224e1d798e2d4c2f4063fd8cda2662b74f2892327c`), with
the extracted API DLL locally observed at 122,146,816 bytes and SHA-256
`90de8f89fa1421eaec510e5cfe11de75846b445489864220ec811fd0f08b4649`.
AnimeJaNai remains `3.7.0`; Aji remains `v0.9.0`. The generic descriptor label
`stock-20c…` is not evidence of a stock mpv binary: the package contains the
verified custom-fork libmpv DLL. Mikai continues to use `vo=libmpv` in its Qt
OpenGL widget.

The upstream portable AnimeJaNai updater's `--components --json` query exited
0 and reported RTX 5090 SM 12.0, recommended `trt-runtime` and
`trt-sm120`, with `trt-sm120` preselected for a new install. This query
installed nothing. The TensorRT component archive was actually downloaded and
verified (SHA-256 `282cb9…`); it contains exactly seven members:
`CUDA_LICENSE`, `DirectML_LICENSE`, `cudart13`, `nvinfer11`,
`nvinfer_plugin11`, `nvonnxparser11`, and `trtexec.exe`. It has no mandatory
package-wide notice members; the installer preserves optional notice files
already present. The upstream source is GPLv3; the application adapts the
existing transport and component protocol, without verbatim unsafe HTTP-copy
code or a standalone-player updater.

All 47 RIFE archive graphs and metadata were imported and compared by hash.
The v4.18 ensemble's malformed 55-character compiled pin was corrected to the
observed full SHA-256 digest
`69c46eb0cf3c1874ca5278758a8e7c4df1b70d0feaa682ce895459b7a6fa9716`
(10,785,406 bytes); the other 46 names, sizes and hashes match the acquired
archive. Full-package inference-folder models use the official hash dispatcher
without querying; custom models require the observed ABI 8. DirectML's actual
closure is four DLLs plus `DirectML_LICENSE`; optional notice files are
preserved.

The local custom Aji import record at
`.probe-out/natural-rife-a42/native/components/aji/v0.9.0/trt/install.json`
is schema 2, ABI 8, provenance `local`, `heavyRifeSupported: true`;
the dispatcher is 313,344 bytes (SHA-256
`59a06a64985465b94dbcea9aedfa131bfecf538c5fa809b77f6b0b976941d859`) and
selected TensorRT backend 622,592 bytes (SHA-256
`cda06b490141655c7f0401f7d0d895a4c9216d6c551354f58e663baa2a08b6d3`).
This is a local import, not the public default download. No backend source
substituted the heavy model.

The 47-model plan-only smoke (artifact 339) prepared plans for all 47 actual
`prepareAnimeJanaiPlan` combinations, including heavy RIFE v4.26. The exact
backend, model code, ensemble flag and verified graph were present; the
filename-versus-extensionless `rifeModelName` boundary was corrected. ABI
codes decode two-digit suffixes correctly (47/48/49), while code 407 is
incorrectly decoded as 4.07. A temporary catalog lifetime issue that had
crashed a heavy test was fixed. This smoke did not request or build an engine.
A fresh spatial TensorRT engine build succeeded in the earlier attempt
(artifact 340). The raw build log records a TensorRT 11.3.0 `[b99]` engine
build in 18.527 seconds, producing 3.01755 MiB for graph SHA-256
`b308e93c3dda3c4f9f968295b2b7b8d1c0583a711bca53a08d51134d9d1cbd5c`;
engine SHA-256 is
`a122bc35c163740a3eedb1221c72e289c296682594bc580ac5faa5676a6f74a8`.
File: `.probe-out/natural-spatial-a43/native/animejanai/models/spatial/aji-1f13ce41.3460285348.trt-11.3.0.gpu-NVIDIA-GeForce-RTX-5090-sm12.engine`
(with the adjacent `.build.log`). Runtime logs show the engine loaded and
640×360 output; slot 2001 was configured for `320x180→640x360` (depth 3),
CUDA-graph chain capture occurred, and `vo=libmpv` reported 640×360 CUDA NV12.
The overall spatial probe was marked FAIL after a 600-second wait expected the
incidental text `filter accepted`; actual status was `filter retained (inference
not observed)`. The false status-predicate gate is now fixed, but this original
run remains failed; later successful runs are recorded above. This engine build
and configuration alone do not establish an
overall probe PASS, pixel correctness, end-to-end processed-playback acceptance,
or private `Ready`/`Active`.

Artifact 349's initial fresh RIFE v4.14 lite probe failed because its
request-bound scan searched the spatial `plan.modelDir`/wrong model identity
and used `time-pos` transitions instead of rendered-frame counts. The corrected
fresh run (artifact 366, 88.63 seconds, stage 7) passed. Its selected-engine
directory began with zero engines/logs; Qt worker → vf → local custom ABI-8 →
`trtexec` built a request-bound TensorRT engine in 38.932 seconds (9.12264 MiB;
9,565,780 bytes), SHA-256
**This is an official/upstream-config observation, not the private prepared
helper:** it is exactly the path that keeps ESC-5 unresolved, because that
configuration still invokes the `trtexec` developer tool. Do not read this
record as evidence that the private helper shells out, nor as any
redistribution clearance for `trtexec.exe`. SHA-256
`e1a4421a3634df8fedaf9d45f1ecc5120aa371e41beb26612ffd7fdfa98dee22`, retained under
`.probe-out/natural-rife-a45/native/animejanai/models/rife/`; the full log is
`.probe-out/natural-rife-a45/qt-flow-proof.log`. The engine used the
5,986,311-byte graph (SHA-256
`948019ffb5e7e54ac6b89a13542da8d100af9b4ade7374551c5f595ac3ff2ce7`). The
PASSED build footer matched exact ONNX and `saveEngine`; all request-bound
components were present, with no prebuilt engine. Engine-to-active-model
identity is inferred from exact configuration/fresh artifacts because Core
does not emit an engine-ID event.
RIFE reported active at `2/1`, `NewVideoFPS48`, padded to 320x192 then cropped
to 320x180, with actual NVDEC output 320x180. Successful
`MPV_RENDER_UPDATE_FRAME` outputs over three seconds measured 73 baseline
versus 145 processed at equal resolution; the 1.5x threshold passed. The old
`time-pos` event counts (70 vs 72) were not used. The processed PNG was
nonblack, changed, and moving (SHA-256
`91ede513739640d2dacb86e3dbdfa48cabb8c4ecb6d99e194344b9f6fac40111`); PlayerDialog
image, pause/seek, volume/mute, audio/subtitle cycle, Off/hwdec restore,
source reopen and shutdown-surface checks passed; decoder/VO drops were zero.
This passes only the described selected-model RIFE run; it does not establish
numerical fidelity, all-47-model inference, private Ready/Active, or FPS
improvement. No FPS performance claim is made.

The default downloader's investigation found the extraction-closure root cause:
the existing 7zr fallback supports only `.7z`, while the Aji closure requires
ZIP and NuGet handling. The source correction now selects the official
[7-Zip 26.03 release](https://github.com/ip7z/7zip/releases/tag/26.03)
assets [`7z2603-x64.exe`](https://github.com/ip7z/7zip/releases/download/26.03/7z2603-x64.exe)
and [`7zr.exe`](https://github.com/ip7z/7zip/releases/download/26.03/7zr.exe)
instead of the old `tools/7zr.exe` fallback. Both digest values come from the
[official release asset listing](https://github.com/ip7z/7zip/releases/expanded_assets/26.03);
the helper checks fetched bytes before writing them, and checks the installer
before executing it with the bootstrap. On successful extraction, the bootstrap
and installer are removed; `7z.exe`, `7z.dll` and other extracted payload
files remain in `cacheDir()/../tools/7zip-26.03`, not `dist`. Upstream
[licensing guidance](https://www.7-zip.org/faq.html) identifies the GNU LGPL;
the unRAR restriction does not apply to this ZIP/7z usage. The exact extracted
license-file name/content was not inspected, so no per-file notice is asserted.
Bootstrap SHA-256:
`ad4c82fadcbdf93c03b4fc440f300509c7d60c5c2f4d183e35d9d70d6957037d`;
installer SHA-256:
`0859c524b8a63551848f0c246abddcb1d0b7b656b0fbfe879f8d85e61a9e6edd`.
Artifact 351 passed selection, verification and review but DirectML extraction
rejected a valid Windows archive path as unsafe. After the separator-
normalization correction, artifact 357 completed actual DirectML download,
install and verification after Review Yes. The selected DirectML backend
remained selected; inactive corrupt TensorRT was ignored. Its ABI-8 install
record SHA-256 was
`24443bc9d5f8624149d7b3b1f91dd851ad7fbc5323fa2836d3380bbe268ebd10`;
the observed closure contained four DirectML DLLs. Artifact 357's overall GUI
probe still failed four shortcut/hidden-tab checks caused by false predicates,
unrelated to the successful installer flow. Subsequent source fixes use
`QTabWidget` registered-page ancestry and the Presets tab caption for the
shortcut locator. That initial run did not pass the GUI gate; the later
corrected run is recorded below.

That earlier full union build reportedly had zero errors/warnings, CTest passed
in 13.78 seconds, and the Ukrainian catalog had 999 finished, zero unfinished
entries; `dist` was rebuilt at stage 7. GUI artifact 343 passed English and Ukrainian access to all 47 RIFE and
five spatial selections, live catalog Save/refusal, Verify preservation of
missing selected DirectML and inactive corrupt TensorRT, refresh persistence,
Review Yes, and backend persistence. The earlier default download attempt
failed before DirectML record creation with `The verified runtime archive
listing could not be read`; artifact 357 later passed actual DirectML
download/install/verification. Artifact 357's four false shortcut/hidden-tab
predicates were fixed, then artifact 361 passed the full GUI/settings gate with
zero failures in 13.68 seconds: visible preset-shortcut editing and clearing,
Save/Cancel,
English/Ukrainian all-47/five-spatial access, live language Save/refusal,
Verify preservation, and Review Yes followed by default DirectML
download/install/verify. The ABI-8 four-DLL record SHA-256 is
`24443bc9d5f8624149d7b3b1f91dd851ad7fbc5323fa2836d3380bbe268ebd10`.
The seven-member TensorRT archive does not include the separately needed Aji
ZIP or NuGet inputs for a full install closure. Artifact 361 supersedes the
earlier 2DIP overrun for the documented shortcut and geometry checks. The
packaged `mikai.exe` window launched and shut down normally; an inspected
startup PNG is recorded at
`.probe-out/packaged-aji-test2/packaged-app-startup.png`. A genuine 320×180
H.264 24 fps fixture with two audio tracks and subtitles played through the
Wasapi/multitrack baseline before the first Aji request (artifacts 331/333);
that baseline itself was not processed playback. Later, artifact 366 passed a
fresh selected RIFE v4.14 lite run: 145 successful surface updates versus 73
baseline in three seconds at equal resolution, with the 1.5x threshold passed;
the processed image changed/moved, and player interactions/source reopen/
shutdown-surface checks passed. This specific run does not establish numerical
fidelity, all-47-model inference, private Ready/Active, or FPS improvement.
The spatial engine build succeeded in artifact 340; its overall probe failed on
the now-corrected incidental-status gate. Spatial attempt a44 observed raw
build and 640× output/render logs, but its probe failed CRC filename filtering
and an early raw-output read of 320 before configured-slot Core property reconfiguration. Corrected spatial run a46 (artifact 367, 66.47 seconds) passed for the selected
model; the 3,133,804-byte raw engine built in 15.7249 seconds (2.98863 MiB),
SHA-256
`0a3fd65a1de1abe5507a996ee571abfebef8eda3294046a195b91b4648c759c9`. Its
full log is `.probe-out/natural-spatial-a46/qt-flow-proof.log`. Four
post-geometry render flags, nonblack changed/moving
OpenGL output, and controls/source-reopen/shutdown checks passed. Postfilter
output was 73 frames/3 seconds (24 fps unchanged), decoder/VO drops zero. This does
not establish all-47 inference, numerical fidelity, private Ready/Active, or
an FPS improvement. ESC-1/2/3 remain open, private Ready/Active are not
established, and public redistribution remains uncleared.

## Contents

| Path | What it is |
|---|---|
| `manifest/sources.json` | Pinned source identities, ABI contract, build-configuration pins, and a clearly separated block of local measurements |
| `manifest/components.json` | Every artefact a shipped bundle would contain, each with a licence and an explicit `cleared` / `blocked` / `external` decision |
| `manifest/THIRD_PARTY_NOTICES.md` | The same findings as prose, with citations, and the reasoning behind each decision |
| `manifest/videojanai-release.json` | Release-asset inventory for the VideoJaNai 2.1.0 components |
| `tools/manifest_check.cpp` | `mikai_manifest_check` — recomputes recorded digests and validates component/source/rights identity |
| `tools/native_inspect.cpp` | `native_inspect` (M11) — read-only ONNX inspection CLI over the official ONNX Runtime C API |
| `tools/bundle_tool.cpp` | `mikai_bundle` — verify, describe, manifest, list and install runtime bundles; only `verify` loads a module |
| `bundle/manifest.schema.json` | The runtime-bundle manifest schema (M05) |
| `bundle/stock.json` | Layout example: the Standard bundle — one stock mpv DLLMODULE and its closure (M05) |
| `bundle/native.json` | Layout example: the Native bundle — the fork core, the ABI-8 shim and the TensorRT/ORT closure (M05) |
| `permission-request.md` | A drafted, **unsent** request to the inference shim's copyright holder. ESC-1 is still open (`blocking`) for bundle clearance — see *Status* |
| `patches/0001-content-addressed-cache-and-prepared-only.patch` | The applicable shim diff: FNV content token in the cache key, `AJI_ENGINE_CACHE`, `AJI_PREPARED_ONLY`, and removal of foreign-engine deletion |
| `patches/0002-native-snapshot-and-control.patch` | C status ABI v2 diff: negotiated observational snapshots, completion-ticket drain, suspend/resume, and measured per-stage engine/model SHA-256; readiness remains `Unavailable` until verified prepared-plan identity exists. **Applicability + union build observed** — `strict` configure applies `0001`+`0002` (build 6.81 s) and now `0001`+`0002`+`0003` (build 23.69 s), compiling/linking `aji.dll`, `aji_trt.dll`, `mikai_native_prep`, `mikai_native_verify`, `native_abi_probe` and a new `native_prepared_runtime_probe`; ABI v2 probes exit `0` (default + real-context); native state stays `Unavailable`, `effective`/prepared empty despite legacy-active |
| `patches/0003-managed-prepared-plan.patch` | Managed prepared-plan C ABI/filter bridge; builds in the strict SDK union (`0001`+`0002`+`0003`, exit `0`, 23.69 s) but establishes no `Prepared`/`Active` or inference state; a later odd-420 crop-kernel correction is source-only/unbuilt |
| `include/aji_native.h` | C-compatible protocol/schema v2 snapshot/control declarations; prepared build keys remain absent unless independently attested |
| `tools/native_abi_probe.cpp` | SDK-independent v2 export/protocol probe with bounded-buffer and restricted-load checks for `aji.dll` |
| `patches/mpv/0001-get-meta-mikai-native.patch` | Applicable pinned-fork schema/protocol-1 Probe producer: typed cached snapshots, actual hardware-context device facts and lifecycle epochs; private non-copy NVDEC smoke passes 14/14, not Native activation |
| `patches/notes/` | The superseded three-part patch **specification**. Records what was required; not applied and not a build input |
| `tools/mikai_native_prep.cpp` | `mikai_native_prep` (M15) — out-of-band preparation CLI that builds a plan **in-process** through the TensorRT builder API |
| `tools/mikai_native_verify.cpp` | `mikai_native_verify` — binds a real engine on the GPU and compares it to a CPU reference against fixed tolerances |
| `sdk-acquisition.md` | How the MSVC/CUDA/TensorRT/ONNX Runtime/DirectML/Windows SDK trees were obtained, and what was proven with them |
| `spatial-acquisition.md` | The five spatial ONNX graphs: source pin, hashes, licence, and the CPU reference observation |
| `rife-acquisition.md` | The four RIFE v4.14 graphs: archive pin, extraction, structural check, weight-data comparison, rights position |
| `videojanai-acquisition.md` | VideoJaNai 2.1.0 component archives: digests, roles and terms evidence |

M04's `CMakeLists.txt` and `build-msvc.md` are real: an optional MSVC + nvcc
project that stages the pinned read-only source, applies `patches/` to the
staged copy and builds `aji`, `aji_trt`, `aji_dml` and the
`tools/mikai_native_prep` CLI. `patches/0001-content-addressed-cache-and-prepared-only.patch`
and `patches/0002-native-snapshot-and-control.patch` both apply under `strict`
configure, and the strict native SDK union build **compiled/linked** `aji.dll`,
`aji_trt.dll`, `mikai_native_prep.exe`, `mikai_native_verify.exe` and
`native_abi_probe.exe` (0001+0002, 6.81 s; with `0003` added, 23.69 s). The ABI
v2 probes then ran exit `0`;
`tools/native_abi_probe` is the opt-in, SDK-independent ABI v2 export/protocol
probe. The earlier three-part specification survives in
`patches/notes/` and is not a build input, and `patches/mpv/` holds
the separate fork `GET_META` diff. The build refuses to configure unpatched by
default; see `build-msvc.md`. `bundle/` carries the manifest schema and the two
layout examples (M05).
Native ABI v2 snapshots report engine/model measurements for diagnostics only.
They do not establish a verified prepared-plan identity: this shim has no
validated prepared-plan attachment producer or native controller/preflight.
Therefore configured legacy and resize-only chains remain `Unavailable`, with
no effective slot/plan or Prepared/Active claim. Drain and resource
suspend/resume remain real diagnostic operations, not readiness attestations.

## Official API DLL and separate patched Native core

The application build uses the official developer archive
[`mpv-dev-x86_64-20261007-git-d6d93599d5.7z`](https://github.com/the-database/mpv-winbuild/releases/download/2026-10-07-d6d93599d5/mpv-dev-x86_64-20261007-git-d6d93599d5.7z)
from [`the-database/mpv-winbuild` tag `2026-10-07-d6d93599d5`](https://github.com/the-database/mpv-winbuild/releases/tag/2026-10-07-d6d93599d5),
release commit `6ac50ae6b135c5c9b484c37cc47e5368d1f8b613`, pinned by SHA-256
`077cb75fed47b185428f97224e1d798e2d4c2f4063fd8cda2662b74f2892327c`. CMake
extracts and uses its matching headers and copies only `libmpv-2.dll` into the
app output and `dist-mikai/mpv/`; the runtime package contains no `mpv.exe`.
This is distinct from the project-patched Native core described below: that
separately built core uses the older
`63d127174d8f70336c82a22408929e2bc301edcb` base and its project-specific
`GET_META` patch. Do not treat its probes, patch or status protocol as contents
of the official release DLL.

The official release contains the ABI-8 `vf_animejanai` filter, faster ASS
changes and display-rate-match, but not this project's private
status/admission protocol. Upstream latest-release APIs were rechecked and still
resolve to mpv-winbuild `2026-10-07-d6d93599d5`, AnimeJaNai `3.7.0`, and Aji
`v0.9.0`. The official v0.9.0 Aji path cannot encode RIFE v4.26 heavy; that
mapping is present only in the explicitly local custom ABI-8 import, which is
not the public downloader's default. The actual current 47-model plan-only
smoke resolves verified graph records and calls the exact plan backend/model
codes, but did not request an engine build. That smoke makes no filter-inference,
fresh-build, Ready/Active or FPS claim; selected-model official-filter playback
runs are recorded separately above.
The DLL alone does not supply Aji or models and does not change their legal
status. Faster ASS is included. Subtitle GPU blur/compositing/rasterization
requires gpu-next and forklibass and is not claimed on this OpenGL surface.
Display-rate-match remains default-off, limited to local seekable mkv/lavf.
DirectML's required shared-NTHANDLE interop with this Qt OpenGL surface remains
unproven; no gpu-next-only subtitle rendering or CPU-ahead fallback is claimed.
The observed genuine H.264 playback was a Wasapi/multitrack baseline before the
first Aji request, not evidence of processed playback. No FPS measurement is
claimed. Local runtime installation requires an explicit directory
and backend selection, is cancellable, and is not auto-imported.
The API DLL package is local/private only: public
redistribution is not cleared until complete corresponding source, dependency
licences, and relink obligations for the prebuilt release are satisfied.

## Scope of use and licence basis

**Owner grant, 2026-10-04 (in-session).** The project owner grants
**unrestricted OpenSource use of this project's own native integration code, its
patch series and its recorded decisions** (recorded in
`manifest/components.json#projectIntegrationGrant`). This is the owner's grant
over code this project owns; it does **not** relicense, license or verify any
third-party binary or source. The repository's root `LICENSE` is **The Unlicense**,
covering this project's own code only; third-party components are excluded from
it. Every third-party component keeps its own per-component licence and decision
below.

The third-party components still carry their own terms, and the earlier recorded
scope for the project was **private, local and non-commercial**, which remains
the basis for the components whose own terms are non-commercial or unverified.
This section is recorded because several findings turn on the difference between
*using* a component and *redistributing* it.

- **CC BY-NC-SA 4.0 — the spatial upscaler weights.** §2(a)(1) licenses
  reproduction and sharing "for NonCommercial purposes only", and §1(k) defines
  NonCommercial as "not primarily intended for or directed towards commercial
  advantage or monetary compensation". §3 adds attribution, a warranty
  disclaimer and ShareAlike, which engage only on a Share. Local non-commercial
  use stays within the located grant, but **redistribution is not cleared**:
  ESC-2 is `blocking`, and no recorded decision places the five graphs into a
  distributed bundle. The §3 conditions remain obligations if a Share ever
  occurs. This is not a claim that CC BY-NC-SA is permissive.
- **The inference shim's upstream terms are not verified.**
  `the-database/animejanai-inference` declares no `LICENSE`, no SPDX identifier
  and no per-file headers, and its GitHub licence field is `null`; no upstream
  licence text has been located and no upstream grant has been obtained. This is
  a **third-party** component and is **not** relabelled OpenSource by the
  project's own owner grant. The project records its decision to use it
  (ESC-1), and the drafted request in `permission-request.md` remains unsent; a
  shipped notice must not claim an MIT licence from upstream.

## Status: ESC-1/2/3 are open (blocking); ESC-4 is resolved; ESC-5 is resolved for the private prepared-helper path only

ESC-1, ESC-2 and ESC-3 carry `severity: blocking` in
`manifest/components.json`; ESC-4 is `RESOLVED` (licence obtained); ESC-6 is
advisory. **ESC-5 is not globally resolved.** In `components.json` its
`resolution` reads `UNRESOLVED FOR THE OFFICIAL UPSTREAM-CONFIG PATH`: the
private prepared-helper path avoids `trtexec.exe` by architecture, while the
official v0.9.0 configuration/first-build path still invokes it, and no
developer-tool redistribution clearance for `trtexec.exe` has been obtained.
Do not read "not shipped" as "no longer needed". The 2026-10-04 owner grant is
recorded at `components.json#projectIntegrationGrant` and covers this project's
own integration code, patch series and recorded decisions only. Read the
findings, resolutions, `scopeNote`/`noticeBasis` and `residualAccepted` fields
there rather than a summary here.
That applies to ESC-5 as well: the in-process helper's architecture resolution
is private-path only. It does not clear the official upstream-config first-build
path, which still requires and receives `trtexec.exe` and still has no
NVIDIA redistribution clearance on record.

These points must not be lost in that summary:

- **The owner grant is not an upstream grant.** The inference repository still
  declares no licence and its licence field is still `null`; the third-party
  shim's upstream terms are **not verified**. The 2026-10-04 grant is the basis
  for this project's own integration only, and every shipped notice has to say
  so in those terms.
- **The owner grant is not a capability.** It does not build a bundle, produce a
  runtime core, or run one frame of native inference.
- **Third-party redistribution is not cleared by the owner grant.** The
  2026-10-04 owner grant covers this project's own integration code, not any
  third-party binary or source. For the shim the upstream terms are **not
  verified**, so a **shipped** Native bundle that carries it has no
  redistribution clearance and none is claimed. The draft
  [permission request](./permission-request.md) remains unsent, and no
  redistributable bundle is assembled or promised.

### What exists and what does not

| Piece | State |
|---|---|
| Shim source | staged and patched by `native/CMakeLists.txt` from a locally mutable tree at the official v0.9.0 tag baseline `c7f56baf…` + authorized edits; the historical `738ce3b3…` baseline comparison predates those edits (`matchesPinnedCommit: false`) |
| Shim patch | one applicable diff applied under `AJI_PATCH_POLICY=strict`; **not** the full M14 SHA-256 `buildKey` integration |
| mpv-fork `GET_META` patch | current typed Probe diff applies against the immutable pin; current owned fork compiles and private real-CUDA lifecycle smoke passes 14/14 (`build-msvc.md`) |
| The pinned mpv fork core | **built** here from the pinned source (273 steps, Release) |
| That core loaded as an isolated module | **passes** (exit `0`) once the stage closure matches the Qt/GNU cohort, with matching client/header API `131077` and mpv `v0.41.0-UNKNOWN`; swap refusal, transport, correlation, `pending 0` and error-end-file all `RESULT: PASS` |
| The original `Win127` load failure | **UNRESOLVED** as to the missing symbol; completing the stage closure fixed this scenario, but no particular missing export has been identified |
| Preparation helper `mikai_native_prep` | built and working; builds a plan **in-process** through the TensorRT builder API — no `--trtexec`, no child process |
| TensorRT components the app needs (per machine) | **provisioned, not shipped** — the AnimeJaNai 3.7.0 pack sizes and SHA-256 digests are pinned; the app reads the compute capability from the driver's NVML, selects the exact `trt-sm{MA}{MI}` pack (or `trt-ptx` when the release publishes none for that generation), downloads the missing set on this machine, proves every file by digest into the app-owned store, and uses the bundle's own resource as-is when the bundle already declares it. This closes the sm120-only bundle gap by provisioning and redistributes nothing. The 3.7.0 archive store is installed per machine by explicit user request; its
SHA-256 is checked before install and can be rechecked with **Verify**. A cold
setup downloads each archive once: the TensorRT runtime archive is fetched to a
scratch path, digest-verified, and the same verified bytes serve both the
component install and the `trtexec.exe` extraction, rather than the pack being
downloaded twice; `mplayer::native::animeJanaiMissingDownloads` /
`animeJanaiMissingDownloadBytes` report the real queued-transfer total (and `0`
with an empty list on a warm store) from store/installation metadata alone. No
inference binaries or model payloads are shipped. The recorded `--fetch` and
real-builder results apply to historical 3.6.3 pack measurements, not the 3.7.0 payloads |
| RIFE component archive | **47-model store** — the official 3.7.0 `component-rife.7z` archive is SHA-256 checked, manually verifiable, and downloaded only on user request. The upstream v0.9.0 filter lacks the v4.26 heavy mapping; a separate local verified import maps it, but missing metadata/ref-inspection proof leaves it not Ready. |
| Spatial raw engine | **produced** (1 380 548 bytes, `input:1x3x64x64`, RTX 5090 `GPU-f76e436f-…`, exit `0`); the source graph's digest was unchanged by that run |
| Spatial CPU reference export | **passes** (fp16 in `1x3x64x64` → out `1x3x128x128`, declared output `N=-1` retained); a non-finite output is refused with `reference.output.nonFinite` |
| RIFE CPU references, all four graphs | **pass** on the original graphs, using one genuine 64×64 PPM pair with the source 11-channel packing and `t = 0.5`; 5 451 samples differ between the two frames |
| RIFE raw engines, all four | **serialized** through the real TensorRT builder API, outside any playback path |
| `mikai_native_verify`, **RIFE lite** | **passes** (exit `0`) — `maxAbsoluteError` `0.0122070312`, RMSE `0.000512728931`, both inside the unchanged `0.02`/`0.005` bounds, no overrides |
| `mikai_native_verify`, **RIFE lite ensemble** | **passes** (exit `0`) — `maxAbsoluteError` `0.00341796875`, RMSE `0.000227462237` |
| `mikai_native_verify`, **RIFE full** | **rejected** — RMSE `0.00198529105` inside bound, `maxAbsoluteError` `0.0561523438` over `0.02`. New O3 current flags build `13003908` bytes and verify fails (exit `9`): `maxAbsoluteError` `0.0385742188`, RMSE `0.00104668487`, 8 outside |
| `mikai_native_verify`, **RIFE ensemble** | **rejected** (exit `9`) — RMSE `0.00104451097`, `maxAbsoluteError` `0.0358886719` over `0.02`. New O3 current flags build `13549972` bytes and verify fails: `maxAbsoluteError` `0.0244140625`, RMSE `0.000785205742`, 4 outside |
| `mikai_native_verify`, spatial HD Performance | **verifies** — same unchanged engine (`sha256 65918177…`) on the unchanged original graph and corrected planar reference: `maxAbsoluteError` `0.00610351562`, RMSE `0.000591099329`, `0` outside tolerance, bounds unchanged. The earlier `0.02734375` rejection (malformed interleaved tensor) is historical |
| What a RIFE pass covers | **raw engine bytes, bindings and numerics only**: `manifestVerified: false` and `prepared: false`. It is **not** `Ready` or `Active` |
| Diagnosis of the rejected engines | **unresolved**; cross-engine FP16 math is the working hypothesis, not a conclusion. The unchanged bounds are not to be widened to make a rejection pass |
| CPU optimisation diagnostic | on the same original spatial graph and input, `DISABLE_ALL` vs `ENABLE_ALL` measured RMSE `8.5556432276e-06` — provider optimisation is **not** the cause. The earlier GPU-vs-`DISABLE_ALL` difference (`maxAbsoluteError` `0.02734375`) was measured on the malformed interleaved tensor and is historical; on the corrected planar reference the engine verifies |
| `native_prep_probe` over the real in-process helper | seven behaviours pass (see `build-msvc.md`) |
| Earlier self-built bundle candidate verify | **passed** after static export classification and isolated per-dependency loads: 270 files, 139 DLLs, 138 restricted-search child-process dependency loads, one mpv module accepting `@mikai_native`. Evidence: `.probe-out/bundle-isolated-dll-load-verify-stdout.log`. This is **not** M05 completion, not prepared inference, and not acceptance of the newly selected author release |
| Native bundle | **`0.1.0-dev.2` installed and load-verified**. `current-native` points at `native-0.1.0-dev.2`; `.1` remains for rollback. Installed verify exit `0`: 83 files, client API `131077`, core accepts `@mikai_native` (`.probe-out/native-crt-closure-verify-installed.log`). Current `mpv_surface_smoke` also passes through this exact installed module with processing off: inspected 1280×720 framebuffer, 921600/921600 non-black samples, 297305/921600 motion, 24 fps, zero decoder/VO drops, zero live filters (`.probe-out/native-bundle-dev2-off-surface.log` and `.png`). Not startup discovery, prepared readiness or inference. Stored runtime choice was not written. The old `.2` closure that failed `Win32 127` is superseded |
| Native-off GUI surface through the owned runtime | **passes** (exit `0`) after full candidate hash validation: the Native-kind candidate is explicitly selected as `0.41.0-native.1`, a real 320×180 HEVC 24 fps video plays with `contextReady` true, position `0.542 → 2.542` over a 12 s duration, **921600/921600** non-black samples and **290264/921600** motion, **0** decoder and **0** VO drops, and **0** live `vf` chain entries with Native off. The inspected `.probe-out/mpv_surface_smoke.png` (1280×720) is a visibly colored motion test pattern |
| Widget/lifecycle probe on that candidate | `widget_pixel_probe` **PASS 21/21**, 0 skips, exit `0`; inspected 640×360 PNG is colored; GL 4.6 with 32 live GL-logger messages, 0 `ErrorType`, `glGetError` 0, motion 30.44%. Late events after close remain unknown and the 256×144 15 fps reopen refreshes metrics and the `vf`-absence check. Descriptor digest changed intentionally (NOTICE); the module core digest is unchanged |
| What those surface proofs cover | **real Qt/OpenGL through the owned native runtime with Native off** — they are **not** the native filter, **not** CUDA interop, **not** active inference and **not** task completion. Nothing is `Prepared` or `Ready` |
| Qt preparation runner, GPU work lease, recipe publication | **do not exist yet** |
| M12 (RIFE assets) | **partially satisfied, not done**; two RIFE engines (lite, lite-ensemble) verify numerically; full and ensemble are rejected; models are still not `Prepared` and not `Ready` |
| `Prepared` / `Active` / `Ready` for any engine | **not reached** (two RIFE engines verify numerically but are `manifestVerified: false`, `prepared: false`) |
| DirectML → Qt WGL interop | **actual probe, honest refusal** — on an attached session (`3.3.0 NVIDIA 616.92`) the legacy `SHARED` path imports the D3D11 texture and blits the Qt framebuffer with exact RGBA corner pixels (`d3dTextureImported:true`, `framebufferBlit:true`, PNG written); the **required** `SHARED\|SHARED_NTHANDLE` registration fails `wglDXRegisterObjectNV` `0xC007006E` → `CAPABILITY_UNAVAILABLE`, exit `2`, `directMlMayBeEnabled:false`, no CPU fallback. DirectML stays disabled with that factual reason; where offered this is an honest refusal, not an NN proof. Evidence: `.probe-out/wave-native_dml_interop_probe.log` |
| ABI v2 export + probes | **observed** — strict SDK union (0001+0002+0003) compiled/linked the shim tools plus a new `native_prepared_runtime_probe` (exit `0`, 23.69 s, parent session bg235; a later odd-420 crop-kernel correction is source-only/unbuilt); `native_abi_probe` exit `0` on default (ABI 8 unchanged, protocol/schema 2/2, refusals `Unavailable`, canaries untouched) and on a real context (UUID `GPU-f76e436f-…`, resize-only `64x64 -> 96x96`, ticket 1 completed, `drain=0 suspend=0 resume=0`, post-resume frame completion) — native state stays `Unavailable`, `effective`/prepared empty despite legacy-active. No NN fidelity/admission/`Prepared`/`Active`. Evidence: `.probe-out/native-abi-v2-default-union.log`, `.probe-out/native-abi-v2-real-context-union.log` |
| Original HD owned import (M11) | **exit `0`** — `modelId user:532fe8…`, declared output `N=-1` preserved, CPU run `N=1` `320x180 -> 640x360`, eligibility `eligible (verified reference)`, `published:true`; inspection only, not an engine and not prepared. Evidence: `.probe-out/native-builder-original-spatial-import.log` |
| Producer fork cadence build | **compiles exit `0`** with `av_d2q` and observed rotation/crop/color facts; updated 17780-byte `patches/mpv/0001-get-meta-mikai-native.patch` exported against base `63d127174d…` |

No binary has been vendored into the repository.

## User-selected author runtime baseline

The native integration base is the exact author release
[`2026-09-17-63d127174d`](https://github.com/the-database/mpv-winbuild/releases/tag/2026-09-17-63d127174d),
acquired independently into
`F:/ZKS/mikai-native-sdks/mpv-author-2026-09-17/`. Both the requested
`mpv-x86_64-20260917-git-63d127174d.7z` and its matching
`mpv-dev-x86_64-20260917-git-63d127174d.7z` match GitHub's published SHA-256
digests; `manifest/sources.json#mpv-author-winbuild` pins the archives.
The workflow commit is `e44cfd8af3eb0aa41b3740f66be4e5a1fcf4171e`,
not the release-tag commit. The exact resolved custom-libass commit is still
unestablished.

Observed against those unmodified release binaries:

- `mpv.exe` plays the real 320×180 HEVC motion fixture through NVDEC and
  OpenGL, exits `0`, and produces an inspected coloured window capture.
  It exposes `animejanai`, `sub-ass-render-threads`, and `sub-gpu-blur`.
- `mpv_loader_probe` passes with client/header API `131077`.
- `mpv_surface_smoke` reports `RESULT: PASS` through the existing
  Qt/OpenGL surface: real moving pixels, no decoder/VO drops in the sampled
  interval, and no native filter in the live chain.
- `widget_pixel_probe` passes **21/21**, exits `0`, and has no skips:
  source replacement and close/reopen refresh cadence, geometry, drop counters,
  and native-filter absence. Its inspected 320×180 framebuffer is coloured,
  with no GL `ErrorType` or `glGetError` errors. These are native-off rendering
  and lifecycle proofs, not inference or performance comparisons.

- `interaction_probe` passes **149/149**: real quality changes, Anime4K
  pixel effect above a stable zero-noise control, independent PiP controls,
  restore/re-entry, and player/float teardown.
- `compare_probe` passes **30/30**: frozen-frame shader comparison changes
  the picture and restores the prior chain; a later capture follows the new
  playback position.

Evidence: `.probe-out/author-mpv-playback.log`,
`.probe-out/author-libmpv-loader.log`,
`.probe-out/author-libmpv-qt-surface.log`,
`.probe-out/author-libmpv-widget.log`,
`.probe-out/author-libmpv-interaction.log`, and
`.probe-out/author-libmpv-compare.log`. The Qt smoke descriptor is a
Standard-capability baseline with protocol/engine schema `0/0`, because
the original release does not implement this project's native status protocol
or prepared-only patch. It does not declare those missing capabilities merely
because the author filter is compiled in.

This supersedes the broad MSYS2 candidate as the selected runtime baseline.
That earlier patched candidate remains a separate experiment; its proofs do
not transfer to the author release. `mpv.exe` is a smoke target, not a Mikai
external-player dependency. No application runtime preference or safe default
was changed. No engine is made `Prepared` or `Active` by these runs, and
complete corresponding source, licence closure, and patched native protocol
acceptance remain required before distribution.

## What is acquired, and what each acquisition does and does not prove

Acquisition is not clearance, and none of the records below is a readiness
claim. Every tree lives outside the repository and outside every tracked source
tree, so nothing can be committed or accidentally vendored.

- **mpv fork core** — licence cleared (GPL-2.0-or-later or LGPL-2.1-or-later per
  build options, see ESC-6), and **built by this repository** from the pinned
  commit. It is a separate opt-in build from the packaged official API DLL
  and uses its own older headers and patched source. The built core loads in
  isolation once the stage closure matches
  the Qt/GNU cohort; the earlier `Win127` failure is explained as a closure
  mismatch in that scenario, not by an identified missing export. The
  corresponding-source, licence-text and relinkability obligations attach to the
  build that actually exists.
- **Official API DLL** — the pinned release and SHA-256 are recorded above.
  This is the app's packaged libmpv module, not the separate patched Native
  core. The release asset's build options, full dependency/license closure,
  complete corresponding source and relinkability obligations are not
  established; package use remains local/private, not publicly redistributed.
- **Native SDKs — acquired, isolated.** Official Windows SDK, CUDA, TensorRT,
  ONNX Runtime and DirectML packages were obtained into
  **`F:\ZKS\mikai-native-sdks\`** with no admin rights, no installer, no driver
  change and no system-wide install, replacing the empty system CUDA stub and
  the Conda TensorRT residue the earlier bounded search had rejected. The tree
  holds the pinned CUDA flavour **13.4** (never inferred from the TensorRT
  version) and TensorRT **11.3.0.99**. A standalone MSVC + CUDA compile, link
  and SM 12.0 kernel execution passed against it. Full layout, URLs, digests and
  terms: **`sdk-acquisition.md`**. That is a toolchain and CUDA-kernel result
  only — no inference chain, playback path or framebuffer result is claimed
  from it. A later build drove the in-process TensorRT builder on a real GPU
  through that helper (see *What exists and what does not*), which is a further
  step, not a playback result.
- **Five spatial ONNX graphs — historical 3.6.3 acquisition and CPU-reference evidence.** Downloaded
  anonymously from `the-database/mpv-AnimeJaNai` at pinned commit
  `d9cbaae5e92a56e71fdf4da5e063ae3e2611e5f7` (tag 3.6.3), digest- and
  size-matched to the manifest. Read once each through `native_inspect`: every
  graph declares rank-4 FP16 input and output with `C=3`, and one **CPU
  reference** run per graph produced concrete `N=1`/`C=3` at both ends — the HD
  declaration's symbolic output batch `N=-1` is **preserved, never clamped**,
  with the concrete `N=1` living only in the separate verified-reference
  profile. This is a CPU execution-provider run: not a GPU/DirectML/TensorRT
  binding, not an engine build, not activation. Hashes, boundary and rights:
  **`spatial-acquisition.md`**.
- **Four RIFE v4.14 graphs — acquired and weight-data traced.** Extracted from
  the pinned `rife-fp16-1.7z` release (published SHA-256 reproduced exactly):
  `rife_v4.14`, `rife_v4.14_lite`, `rife_v4.14_ensemble`,
  `rife_v4.14_lite_ensemble` — the full {Full, Lite} × {ensemble on/off} space.
  Each graph was walked as a protobuf: 128/128 float16 initializers and the
  documented fp32 GridSample island are present in the bytes. The author's own
  checkpoints were then compared tensor by tensor: **128/128 names and shapes
  matched with FP32→FP16 round-to-nearest-even bytes identical** in all four
  comparisons, so the stored weights *are* the author's MIT v4.14 / v4.14.lite
  weights. Exporter identity, the non-weight graph material and whole-graph
  redistribution are the residual findings; the owner has determined they are
  acceptable for this project (ESC-3 — see
  `components.json#components[id=models.rife-v4.14].clearanceBasis`), which
  accepts the residual rather than establishing facts about it. Archive pin,
  extraction, comparison report and rights: **`rife-acquisition.md`**.
  Since acquisition, all four graphs have run a **real CPU reference** (one
  genuine 64×64 PPM pair, the source 11-channel packing, `t = 0.5`) and all four
  were **serialized through the real TensorRT builder API**. The verifier splits
  the matrix: **lite** and **lite ensemble** pass numerically; **full** and
  **ensemble** are **rejected** on `maxAbsoluteError`. A pass covers raw engine
  bytes, bindings and numerics only — `manifestVerified: false`,
  `prepared: false` — so no model is `Ready` or `Active` and M12 is not done.
  See *What exists and what does not*.
- **VideoJaNai 2.1.0 components — acquired as evidence only.** Six assets
  (manifest, packs, TRT runtime/SM120/PTX, RIFE) with independent SHA-256
  recomputation; no component was installed, executed, bundled or used to
  replace a driver. That acquisition located no source/binary grant for the aji
  source or binaries, and that upstream fact still stands; the project records
  its decision to use them, and the shim's upstream terms are **not verified**
  (ESC-1 stays `blocking` for bundle clearance). See
  `videojanai-acquisition.md`.
- **ONNX Runtime 1.24.4 (DirectML) and DirectML 1.15.4** — cleared, MIT /
  Microsoft Software License Terms respectively; both packages carry their
  licence files, and DirectML's `LICENSE.txt` §1(a) expressly permits
  distributing the runtime in Windows/Xbox ML applications. Terms and acquisition
  detail: `sdk-acquisition.md`; component decisions: `manifest/components.json`.
- **TensorRT 11.3.0.99 runtime and the CUDA `cudart` redistributable** —
  `external`: distributable at bundle-assembly time under the SLA's §1.2.1.x
  conditions, with the notice *"This software contains source code provided by
  NVIDIA Corporation."* and NVIDIA's `Acknowledgements.txt` verbatim. The
  package was acquired (anonymous 302 → 200, `application/zip`,
  1 985 279 435 bytes) and it ships `trtexec.exe` as one of its seven members.
  On the private prepared-helper path `trtexec.exe` is **not used at all**:
  the preparation helper builds in-process through the TensorRT builder API and
  launches no external executable. That is what ESC-5's architecture resolution
  rests on — and it is scoped to this path only. The official v0.9.0
  config/first-build path still invokes `trtexec.exe`, so its
  developer-tool redistribution question under the TensorRT SLA stays
  unresolved, and no NVIDIA clearance has been obtained.
- **Anime4K shaders** — MIT (47 files) and Unlicense (2 files), read directly
  from the files. Out of native scope, so no `components.json` row.

## Pins that matter

| Pin | Value |
|---|---|
| mpv fork | `63d127174d8f70336c82a22408929e2bc301edcb` (signed, 2026-09-17) — current owned patched fork built and Probe-exercised privately; not a root CMake build target or shipped Native bundle |
| inference shim | historical baseline `738ce3b3020b3950c48ce3122a08f32af531d4fa`; current build input is the official v0.9.0 tag commit `c7f56baf966064a7855db2f82a2f1b0d0c6cfdb3` **plus authorized mutable local edits** (`sources.json` records `matchesPinnedCommit: false`). `include/aji.h`, `src/aji_conf.cpp` and `src/aji_dispatch.cpp` were byte-identical to v0.9.0 at the historical baseline comparison; the TRT implementation and CMake files are fork-modified. Upstream declares no licence; adopted on the owner's determination (ESC-1) |
| patch series | `patches/0001-content-addressed-cache-and-prepared-only.patch` then `patches/0002-native-snapshot-and-control.patch` applied; `patches/notes/` and `patches/mpv/` not applied by the shim build |
| C ABI | version **8**, 17 exports, 3 of them optional for older callers |
| `AJI_NVINFER` | `nvinfer_11` — the value this project's configure commands pass; `native/CMakeLists.txt` itself defaults to `nvinfer_11`, and TensorRT 10 is **not** a dependency. The configure check requires `${AJI_TRT_ROOT}/lib/${AJI_NVINFER}.lib` to exist |
| CUDA architectures | `75-real;80-real;86-real;89-real;90-real;100-real;120-real;120-virtual` |
| CUDA flavour | **13.4** — never inferred from the TensorRT version |
| TensorRT | 11.3.0.99 |
| ONNX Runtime / DirectML | 1.24.4 / 1.15.4 (1.24.x is the DirectML ceiling) |
| Spatial graphs | `the-database/mpv-AnimeJaNai` commit `d9cbaae5…` (tag 3.6.3) — see `spatial-acquisition.md` |
| RIFE graphs | `animejanai-inference` release `models-rife-fp16-1` — see `rife-acquisition.md` |

The historical 3.6.3 spatial graph acquisition and CPU-reference observations
above describe those pinned files and measurements only; they do not describe
the current 3.7.0 RIFE component store or establish current filter runtime
readiness.
*** End patch code
*** Begin Patch patch
The CUDA architecture list must always be passed explicitly: the upstream
CMakeLists fallback is guarded by `if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)`
and CMake pre-defines that variable during `project()`, so the fallback never
fires and omitting it yields a single-arch binary.

The upstream build record (`docs/BUILD-WINDOWS.md`) describes a *different*
machine (VS 18 Community, toolset 14.51.36231, CUDA 13.3). It is a record of the
author's environment, not of this one.

## Observations are not pins

`plan.md` §3.2 lists hashes for the stock core, the fork core, `aji.dll` and
`aji_trt.dll`. Those, and everything else in the `observations` block of
`sources.json`, were measured from pre-existing files that **this repository did
not build**. No source→binary reproduction has been proved. They are descriptive
evidence about files on a machine, and must never be used as a release pin,
referenced by a bundle manifest, or treated as a download verification.

They become pins only when this repository builds the artefact itself, the build
is reproducible, and the digest is recorded from that build.

## Tooling

**`mikai_manifest_check`** recomputes the SHA-256 of every file whose digest the
manifests record, and validates that each component's
`sourceIdentity.sourceId` resolves to a source and that `redistribution` is one
of the three allowed values. Exit codes: `0` clean, `1` invalid identity or hash
mismatch, `2` digest inventory or source root not established, `3` a component
is `blocked`. It loads no DLL and proves nothing about runtime behaviour.

It was built through the root CMake entry and executed against the declared
read-only inference and spatial-model roots supplied at the time. Observed:
**exit 3**, with **14 recorded file digests recomputed and matched** and
unestablished inventory for the sources that were not present then (`mpv-fork`,
`mpv-upstream-stock-build`, `tensorrt-11.3-sdk`, `cuda-toolkit`,
`onnxruntime-directml`, `directml`, `rife-v4.14-assets`, `toolchain.msvc`,
`ffmpeg-dev`). It proves the recorded digests of the files that were present
match, and nothing about runtime behaviour or bundle assembly.

That exit 3 is **historical**. A current-tree checker run against all declared
digest inventories now exits **0**: **19** file digests match, including both
author-runtime archives, and **18** observation digests are format-checked
without being treated as pins. The checker was rebuilt after replacing an
invalid `QJsonValue::toStringList()` call with direct JSON-array comparison.
A mismatched architecture list is rejected with exit `1`.
Evidence: `.probe-out/author-runtime-full-manifest-check.log`.
This checks declared source/component identities, not undeclared dependency
source, bundle redistribution readiness, or prepared inference.

The historical `ctest -R mikai_tests` run also passed (14.65 s test time);
the live oracle remains disabled. No linter is configured.
Evidence: `.probe-out/author-runtime-ctest.log`.

**`native_inspect`** (M11) is the read-only ONNX inspection CLI. It uses the
official ONNX Runtime C API to read a graph's declared I/O and, with
`--resolve WxH`, run one **CPU reference** at that geometry. It is CPU-only and
touches no engine path; the five-graph observation above is its result. It is
also how the exercised graph was pinned: `--reference-dir` exports one real CPU
reference — input bytes, output bytes and a manifest into a new directory — at
fp16 `1x3x64x64` → `1x3x128x128`, with the declared symbolic output `N=-1`
retained rather than clamped. A reference whose output holds a NaN or an
infinity is refused with `reference.output.nonFinite`.

**`mikai_native_prep`** (M15) is the out-of-band preparation CLI built by
`native/`, described with its invariants and exercised scope in
`build-msvc.md`. It builds a plan **in-process** through the TensorRT builder
API — no `--trtexec`, no child process — for one graph, one shape set and one
named CUDA device, and moves raw plan bytes onto `--out`. **A raw plan is not a
prepared plan**: it has not been verified, published to a manifest or an
artifact store, and it is not `Prepared` or `Active`.

**`mikai_native_verify`** binds a real engine, enqueues it on the real GPU and
compares against the CPU reference. On the corrected planar reference the
spatial HD Performance engine now **verifies** (`maxAbsoluteError`
`0.00610351562`, RMSE `0.000591099329`, `0` outside tolerance, bounds
unchanged); RIFE full and ensemble still reject. The `0.02`/`0.005` bounds are
not to be widened to make a rejection pass, and a verified engine is still
`prepared: false`, not `Prepared`.

**`mikai_bundle`** verifies, describes, lists and installs runtime bundles.

`verify` is the acceptance instrument for a bundle and it loads. Under the same
restricted search the application uses — the module's own directory, the
bundle's declared dependency directories and System32, with the process `PATH`
never read — it loads **every** declared DLL, then refuses the bundle unless
exactly one of them is an mpv module, that module is the one the manifest names,
its reported client API matches the declared one (when one is declared), and,
for a bundle labelled `native`, the core itself accepts `@mikai_native`. That
last check asks the core by name and nothing else: a handle is created, given
`config=no`, asked to take the filter, and destroyed uninitialized, so no window,
no video and no audio ever exist. A core without the filter refuses the option,
which is exactly the incoherent case the manifest would otherwise hide. What a
passing `verify` still does not prove is playback, a prepared engine or a real
frame.

`describe`, `list` and `manifest` load nothing, which is what makes them usable
on a machine with no core present. `manifest` reads the layout a manifest
declares without the payload it names, and it is deliberately not a weaker
`verify`: every check that needs bytes needs bytes.

**The two `bundle/*.json` files are layout examples, not bundles.** No binary is
vendored here, so every file record carries `bytes: 0` and the reserved
sixty-four-zero placeholder digest — well-formed under the schema so the
examples parse, and a value no file has ever hashed to, so an example can never
be mistaken for a measured bundle. `verify` recomputes every digest and
therefore refuses both until M04's real build output is assembled beside a
manifest. Reading their layout:

```text
mikai_bundle manifest native/bundle/native.json
```

The Standard example describes the packaged official `libmpv-2.dll` API
module, not an app-launch download or stock-cache fallback. The Native example
describes a separate fork core, the three ABI-8 shim DLLs, and the
TensorRT/CUDA and ONNX Runtime/DirectML closures in `trt/` and `ort/` — the
layout that removes the
`AddDllDirectory`-into-an-existing-TensorRT-folder dependency the research
harness needed, and that leaves nothing to Conda, torch, Python or any other
ambient install.

## Root CMake and the optional inspector

The root build is deliberately free of any native-SDK dependency. The relevant
pieces:

- **mpv**: CMake acquires or validates `MIKAI_DIST_MPV_ARCHIVE` against the
  pinned official release SHA-256, extracts matching headers and compiles
  against them. The app uses the libmpv C/render API through its existing
  `QOpenGLWidget`; no `mpv.exe`, IPC, or load-time DLL import is used. Build
  output and `dist-mikai/mpv/` contain only `libmpv-2.dll`. It is local/private
  only pending the complete source, dependency-license and relink obligations.
- **`MIKAI_WITH_MODEL_INSPECTION`** (`option`, default **OFF**): when ON it
  `find_path`s `onnxruntime_c_api.h` under **`-DORT_INCLUDE_DIR`** and adds the
  `mikai_model_inspection` static library and the `native_inspect` executable
  (plus `TestNativeCatalog`). This is **real CPU-only ONNX Runtime inference** —
  it is not a GPU/DirectML/TensorRT binding and produces no engine.
- Everything else the Qt app builds (`mikai_native_support`, `mikai_player`,
  `mikai_backend`, probes, tests) needs no native SDK.

## Verification status
The per-suite counts and engine outcomes in this section are **historical**
isolated-build ledgers, separate from the selected-model spatial/RIFE results
and the later player/Compare acceptance (see *Current observed integration state*
above). The earlier
complete CPU matrices were inspection-ON exit `0` in **21.48 s**
and inspection-OFF exit `0` in **15.92 s**
(`.probe-out/native-batch-on-epoch-guard-ctest.log`,
`.probe-out/native-batch-off-epoch-guard-ctest.log`); an earlier settled-source
run (ON `20.55 s` / OFF `15.37 s`) predates the later MinGW-CRT/source updates.
A genuine prepared CAPI first-snapshot regression now passes **all 7 previously
failing fields before output** (state `Prepared`, not `Active`; real CUDA NN
maxAbs `0.0034044981`, RMSE `0.000446848262`) — a **DIRECT CAPI/CUDA** proof, not
a Qt-framebuffer proof (`.probe-out/native-batch-prepared-snapshot-proof.log`).


- **Fresh isolated build with the inspector OFF (default, no native SDK):**
  `mikai` and `mikai_tests` build (exit 0) and the C++ ctest entry passes in
  **14.82 s** (14.83 s wall, hermetic; 24 suites PASS and `LiveOracle` still
  skips by design). A real Qt/OpenGL **640×360** colored moving frame
  (`testsrc2` + motion) rendered through the explicitly loaded module with the
  GL debug logger active: **32 messages, no `ErrorType` message, `glGetError`
  0**. Perf-warning messages remain, so this is not a warning-free run.
- **Historical import inspection was clean.** `objdump -p` on `mikai.exe` from
  both the default/OFF and review/ON builds lists only `Qt6Core`, `Qt6Gui`,
  `Qt6Network`, `Qt6OpenGLWidgets`, `Qt6Widgets`, `KERNEL32`, `msvcrt` and
  `SHELL32` — no mpv, ONNX Runtime, CUDA, TensorRT or DirectML DLL. mpv is
  resolved at run time and nothing native is on the link path.
- **Optional inspector ON:** the isolated configuration
  (`-DMIKAI_WITH_MODEL_INSPECTION=ON -DORT_INCLUDE_DIR=…`) builds the app,
  tests and the listed probes/helpers, and its ctest run passes in **15.17 s**
  (25 suites PASS, `LiveOracle` SKIP).
- **The engine builder works in-process; the spatial engine now verifies on the
  corrected reference.** A Release build of the native tools and of the Qt tree
  both succeeded. `native_prep_probe` passes seven behaviours against the real
  in-process helper (existing `--out` refused and unchanged; a corrupt graph
  exits `5` leaving no engine-shaped file; an absent GPU UUID refuses rather than
  falling back to another adapter; `build-started` is emitted before the build
  proceeds; the TF32-off/strongly-typed precision gate holds; cancelling
  mid-build publishes nothing; failure and cancellation leave prior artifacts and
  source bytes unchanged). A real 64×64 build on the selected RTX 5090 exited `0`
  and wrote **1 380 548 bytes** of raw plan, with the source graph's digest
  unchanged. `native_inspect --reference-dir` exports the matching CPU reference
  successfully and refuses a non-finite output with `reference.output.nonFinite`.
  `mikai_native_verify` now **verifies** that same unchanged engine
  (`sha256 65918177…`) against the **corrected planar** reference on the
  unchanged original graph: `maxAbsoluteError` `0.00610351562`, RMSE
  `0.000591099329`, `0` outside tolerance, bounds unchanged
  (`.probe-out/numerical-boundary-spatial-verify.log`, exit `0`). The earlier
  `maxAbsoluteError` `0.02734375` rejection was measured against the malformed
  interleaved tensor and is **historical for this engine/input boundary**. RIFE full and ensemble remain rejected in this historical run (see below).
**No engine was published, `Prepared` or `Active`, and no frame of native
inference was rendered in that run; `mikai_manifest_check` still loads no DLL.**
A separate private `0.1.0-dev.2` bundle installation is recorded below; it does
not supply a prepared engine or inference result.
- **Two RIFE engines verify numerically; two do not.** All four original graphs
  run a **real CPU reference** against one genuine 64×64 PPM pair (5 451 samples
  differ between the frames) with the source 11-channel packing and `t = 0.5`,
  and all four graphs were **serialized through the real TensorRT builder API**
  outside any playback path. `mikai_native_verify` then splits the matrix against
  the unchanged `0.02`/`0.005` bounds, with no overrides: **RIFE lite passes**
  (`maxAbsoluteError` `0.0122070312`, RMSE `0.000512728931`) and **RIFE lite
  ensemble passes** (`0.00341796875`, `0.000227462237`), while **RIFE full is
  rejected** (`maxAbsoluteError` `0.0561523438`, RMSE `0.00198529105`) and **RIFE
  ensemble is rejected** (exit `9`; `maxAbsoluteError` `0.0358886719`, RMSE
  `0.00104451097`). Every graph is preserved at dtype FP16 with input
  `1x11x64x64` and output `1x3x64x64`, and that packing is grounded in
  `kernels.cu:500-551` (`[RGB_A RGB_B, t = .5, meshX meshY mulW mulH]`).
  A pass covers **raw engine bytes, bindings and numerics only** —
  `manifestVerified: false`, `prepared: false` — so **nothing is `Ready` or
  `Active`**. The RIFE full/ensemble rejections are **unresolved** and the bounds
  are not to be widened to make them pass. M12 is partially satisfied and **not
  done**: the spatial HD Performance engine now verifies, but RIFE full/ensemble
  do not, so M12 remains incomplete. These numerical/raw-engine observations
  do not establish end-to-end processed-playback acceptance.
- **An earlier native-facing suite run was hermetic.** That historical
  inspection-on suite exited `0` in **15.05 s**, with `TestLiveOracle` skipped by design and the
  junction-permission and missing-`MIKAI_NATIVE_ASSETS` suites still skipping.
  Those suites exercise contracts and vocabulary, not a native chain.
- **The pinned mpv fork core builds and its isolated core loads; the bundle
  candidate does not yet verify.** The fork builds here from the pinned source
  (273 steps, Release), and loading that core as an isolated module **passes**
  (exit `0`) once the stage closure matches the Qt/GNU cohort: client and header
  API both `131077`, mpv `v0.41.0-UNKNOWN`, and swap refusal, transport,
  correlation, `pending 0` and error-end-file all `RESULT: PASS`. The earlier
  `Win127` failure is **unresolved as to the missing symbol** — completing the
  stage closure fixed that scenario, but no particular missing export has been
  identified.
  The earlier **`GGML` initializer abort** on the bundle candidate is
  **superseded**: after a single MinGW-runtime deployment fix (optional
  `MIKAI_MINGW_RUNTIME_DIR`, default EMPTY), a **private coherent Native bundle**
  now **verifies exit `0`** — `138` files, `135` DLL hash+PE closure, `134`
  actual isolated `LoadLibraryEx` child loads, mpv client API `131077`, the core
  accepts `@mikai_native`, `aji` reports ABI 8 — and install/verify/list exit `0`
  as `native-0.1.0-dev.1` (descriptor
  `4f3decfb182632ff1c3654a8a021035ca2ff90e6e675683f3f78c6c8237aa3ac`) under
  `AppLocalData/native/runtime`. That `.1` install predates the later CRT/source
  updates. `0.1.0-dev.2` is now installed beside it (`current-native` points at
  `.2`; `.1` remains for rollback). Installed verify exit `0`: 83 files, client
  API `131077`, core accepts `@mikai_native`
  (`.probe-out/native-crt-closure-verify-installed.log`). A later current-build
  `mpv_surface_smoke` explicitly selects the installed `.2` and passes through
  Qt with processing off: inspected `1280x720` framebuffer, all `921600`
  samples non-black, `297305` changed samples, `24` fps, zero decoder/VO drops
  and zero live filters (`.probe-out/native-bundle-dev2-off-surface.log` and
  `.png`). This is not startup discovery, prepared readiness or inference. The
  old `.2` closure that failed `Win32 127` is superseded, not the installed tree.
  This is **private only**:
  the stored runtime choice was not written, no stock-dir change, no public
  third-party package, and the grant is still unknown. The obsolete prepared
  `1ce…`/plugin `f400…` artifact cannot admit the new module and must be rebuilt.
  Separately, a **Native-off GUI surface** through the owned runtime now
  **passes** (exit `0`) after full candidate hash validation: the Native-kind
  candidate is explicitly selected as `0.41.0-native.1`, a real 320×180 HEVC
  24 fps video plays with `contextReady` true, position `0.542 → 2.542` over a
  12 s duration, **921600/921600** non-black samples and **290264/921600** motion,
  **0** decoder and **0** VO drops, and **0** live `vf` chain entries with Native
  off. The inspected `.probe-out/mpv_surface_smoke.png` (1280×720) is a visibly colored
  motion test pattern. On the same candidate, `widget_pixel_probe` passes
  **21/21** with 0 skips, 0 instrumentation-unavailable markers and exit `0`; the
  inspected 640×360 PNG is colored, GL 4.6 with 32 live GL-logger messages,
  0 `ErrorType`, `glGetError` 0 and 30.44% motion. Late events after close remain
  unknown, and reopening at 256×144 15 fps refreshes the metrics and the
  `vf`-absence check. This is **real Qt/OpenGL through the owned native runtime
  with Native off** — it is **not** the native filter, **not** CUDA interop,
  **not** active inference and **not** task completion. No native frame has been
  rendered, no public bundle is shipped, and nothing is `Ready`.
- **Earlier desktop probe results, not final UI-cleanup acceptance:** with the
  session attached, `interaction_probe` is 149/149
  `RESULT: PASS` and `compare_probe` 30/30 `RESULT: PASS` (frozen-frame proof,
  0.000/255 noise floor); `hls_recovery_probe` and `external_media_probe` report
  `RESULT: PASS` (real expired-signature recovery; real picker, HTTP, same-URL
  retry, file URL). `anime4k_menu_probe` was rebuilt on semantic locators (no
  `objectName`/tooltip selectors) and reran as full PASS with 0 failures: all 24
  preset leaves selectable, effect 1.027/255 against a 0.0000 floor, reopen
  restore 0.999/255. The pixel probes still need the interactive desktop
  session; detached they read black (`hasVideo=0`). A fresh-binary
  `widget_pixel_probe` passed 11 requirements at 640×360 with `dpr=2` and 29.60%
  motion (32 GL messages, no `ErrorType`, `glGetError` 0; perf warnings remain).
  That probe accepts a PNG path argument and otherwise runs one fixed lavfi
  source, so it does not cover alternate-media or dialog flows; its only
  observed failure is a refused PNG save (FAIL, exit code 1). `shell_probe` and
  `dialog_probe` are what observed the missing-input FAIL (exit code 1), the
  static/black inconclusive SKIP (exit code 77), the moving-portrait and the
  1 s-EOF cases. `mpv_surface_smoke` also requires inter-frame motion:
  static near-black is SKIP (exit code 77). `headless_pixel_probe` proves lit
  pixels and a real render without a motion requirement; static near-black
  can pass there, while all-black is inconclusive.
  Separately, the M42 metric gate is not closed, which is a
  probe-observability gap and not a missing player-chip feature: the widget
  probe cannot observe raw `estimated-vf-fps`/drop counters through
  `VideoSurface`, so it prints them as unavailable; reported stream FPS can fall
  back to container FPS, and only the headless-owned `MpvHandle` reads the raw
  properties.

Manifest hashes were computed with `sha256sum` and download accessibility with
`curl -I`; the SDK tree's digests and their self-measured status are recorded in
`sdk-acquisition.md`.

## Consequences for the tasks that follow

- **M04's toolchain prerequisite is met** by the independently acquired SDK
  tree, and the shim builds from the staged, patched tree. What M04 does not
  yet have is a **built and recorded artefact**: the patch series identity in
  `sources.json#patchSeries` is still `PENDING`, and the applied series is not
  the full M14 SHA-256 `buildKey` integration.
- **M05** is `blocked`: `components.json#bundles[id=native]` records
  `shippingDecision: "blocked"` and ESC-1/2/3 stay `blocking`; the 2026-10-04
  grant covers this project's own integration only. What M05 lacks is both an
  input — no assembled closure and no per-file digests for a bundle that does not
  exist — and third-party clearance.
- **M15** has a working in-process helper, a passing probe and one raw plan
  produced, but the verifier **rejects** that plan on `maxAbsoluteError`, and
  there is no Qt preparation runner, no GPU work lease and no publication.
- **Every capability surface must degrade to a named `Unavailable`**, never to a
  silent substitute.

### Which tasks the escalations bind

ESC-1, ESC-2 and ESC-3 are `blocking`; ESC-4 is `RESOLVED`; **ESC-5 is
`UNRESOLVED FOR THE OFFICIAL UPSTREAM-CONFIG PATH`** and is avoided by
architecture on the private prepared-helper path only; ESC-6 is advisory. None of them binds **local build/use** on rights grounds (the
2026-10-04 owner grant covers this project's own integration), but each of
ESC-1/2/3 binds **bundle clearance** for its third-party component. What still
gates those tasks is **build output and capability proof**, plus third-party
redistribution clearance. Both are recorded per component in `components.json`.
Read the `effect` and `residualAccepted` fields there; the table below is a
summary of what remains outstanding, not of what is permitted:

| Area | What is now true | What is still missing |
|---|---|---|
| Shim build (M04) | the staged tree patches and builds locally; ESC-1 stays `blocking` for bundle clearance | patch series identity not recorded in `sources.json#patchSeries`; the applied diff is not the full M14 SHA-256 `buildKey` |
| Fork core (M04) | the pinned fork builds here (273 steps, Release); the isolated core loads in the matching stage closure | the original `Win127` missing symbol on the superseded closure is historical; the installed `0.1.0-dev.2` core loads under restricted search |
| Bundle (M05) | `0.1.0-dev.2` is installed, load-verified and renders moving video through Qt with processing off; `.1` remains for rollback; owner grant 2026-10-04 covers this project's own integration; `bundles[native].shippingDecision: "blocked"`; ESC-1/2/3 `blocking` | no required Standard-bundle production proof; no **public/shipped** bundle or third-party clearance; no prepared readiness or inference proof; the obsolete prepared `1ce…`/`f400…` artifact must be rebuilt for the new module |
| Preparation (M15) | in-process helper built and probed; spatial and all four RIFE raw plans produced; spatial engine verifies on the corrected reference; `0003-managed-prepared-plan.patch` builds in the SDK union; builder publish/reuse/cancel/lease/preserve **EXIT 0** with actual verified artifacts (engine `1ce1e3d9…`, plan `c1747295…`, inference `738ce3b3…`, plugin `sha256:f400b436…`); a prior `717…` wrong-identity artifact was removed from the owned store; `0.1.0-dev.2` is installed, load-verified and plays with processing off, not a preparation or inference proof | RIFE full/ensemble **rejected** on `maxAbsoluteError` (bounds unchanged/unpublished; O5 not run; precision/tactics is a hypothesis); verified engines are still not `Prepared`; SDK prep Describe now works (`.1`, `.probe-out/native-installed-prep-describe-corrected.log`); the obsolete prepared `1ce…`/`f400…` artifact must be rebuilt for the new module; the observed direct-CAPI prepared inference and factory query do not prove an installed-core Qt playback epoch or decoder-to-framebuffer inference |
| RIFE assets (M12) | all four graphs run a real CPU reference; all four serialized on GPU | full and ensemble rejected on `maxAbsoluteError`; lite and lite-ensemble verify but are not `Prepared`/`Ready`; M12 not done |
| Native execution (M15–M32, M43–M45, M48, M49) | rights do not block independent player-side implementation; direct-CAPI prepared inference is observed separately | no complete installed-core Qt prepared recipe or accepted inference framebuffer; CAPI activation is not Qt playback activation |
| Contracts (M08–M10, M13, M14, M33–M42) | unchanged and unaffected | — |
| `trtexec.exe` (ESC-5) | **partially** resolved: the private prepared-helper path builds **in-process** and never invokes it at all, so ESC-5 does not bind that path | the official v0.9.0 upstream-config first-build path still invokes `trtexec.exe`; `components.json` records it `UNRESOLVED FOR THE OFFICIAL UPSTREAM-CONFIG PATH`, so no bundle carrying that path is cleared by architecture alone |

**M50 and M52** depend on a bundle that does not exist yet — a sequencing
consequence of M04/M05 — and, for anything **shipped**, on redistribution
clearance that the private/local determination does not provide.

There is no third route. Reimplementing the ABI-8 shim against its published
header, and substituting a different model, are both off the table: this
project uses **this** inference implementation and **these** weights, or the
capability reports `Unavailable`.
