# Known bugs and audit caveats

Audit date: 2026-10-11. Scope: static, solo repository audit requested by the owner.

No fixes, builds, tests, probes, application launches, installs, cleanups or commits were performed. This report is the only repository file created by this audit. Read-only work included source/document inspection, graph queries, local-reference checks, recorded patch/DLL hashing, binary import inspection and primary library documentation lookup.

**Status:** findings are open/scouted, not newly reproduced runtime failures. **Static defect** means the stated branch, mismatch or counterexample follows from current source. **Runtime risk** means a reachable-looking lifetime/concurrency failure still requires reproduction. **Caveat** records a deployment, provenance or workflow limitation. `[INFERENCE]` marks effects not observed during this audit.

Priority: **P1** potential data loss, undefined behavior or violation of a distribution safety boundary; **P2** correctness, responsiveness, reproducibility or materially misleading documentation; **P3** maintenance/DX weakness. Priority is not a claim that an installed app currently fails.

## Remediation status (2026-10-11, later session)

All 28 findings were fixed and the fixes were verified by running the affected
paths. The audit text below is preserved unchanged as the record of what was
found; this section records what was done about it.

**Build and suite.** `cmake --preset release` configures with CMake 4.4.3;
`mikai`, `mikai_tests` and every affected probe build with zero errors and zero
warnings under `-Wall -Wextra`. `ctest --test-dir build/release` is **4/4
PASS**: `mikai_tests` (all suites PASS, `TestLiveOracle` SKIP without a key),
`NativeStageContracts`, `LauncherContracts` and `DistContracts`.

| Finding | Fix (files) | Observed verification |
| --- | --- | --- |
| KB-01, KB-13 | `Downloads.cpp`: `WriteOnly\|NewOnly` exclusive creation; a refusal writes only the reason, and only this invocation's own partial is ever removed; every `write()` result compared to the chunk, then `flush`/`close`/`error()` checked before any size or digest verdict | `TestDownloads` PASS — a pre-existing destination survives a refusal byte-for-byte with its mtime intact, a racing destination is refused, digest/size/cancel failures remove only the owned partial, and a storage failure cannot return verified success |
| KB-02 | `src/probes/shared/ProbeIsolation.hpp` plus `SpatialAssetsProbe`, `NativeAssetsProbe`, `NativePlaybackAssemblyProbe`, `RifeSettingsProbe`, `NativeSettingsProbe`: self-copy into an owned scratch, marker-verified child, `isOwnedScratch()` gating every wipe and cleanup | Ran `native_playback_assembly_probe` beside the populated `build/release/native`: it refused (exit 2, reparse-point scratch) and the store's 25 files were SHA-256-identical afterwards |
| KB-03, KB-16, KB-17, KB-18 | `native/cmake/StageInference.cmake` (ownership and overlap refusals before any delete, full-content staging fingerprint, strict/warn/off semantics) and the manifest reconciliation | `NativeStageContracts` PASS 86/86, deterministic across three clean runs; the external source tree stayed byte-identical |
| KB-04, KB-19 | `cmake/dist.cmake`: forbidden payload removed by own-name over the whole copied tree with a post-sweep re-glob, both overlap guards moved before the first write, populated output refused, executable-local native root, `MIKAI_DIST_NATIVE_ROOT` forwarded | `DistContracts` PASS 83 checks, including byte-preservation of a populated output and a valid assembly that strips direct and nested payload while ordinary runtime survives |
| KB-05, KB-08, KB-09, KB-10 | `MikaiClient.{hpp,cpp}`, new `Errors.{hpp,cpp}`, `SettingsDialog.cpp`: key mutex, credential+generation snapshot taken after the limiter grants the slot, envelope-first HTTP classification, `QSaveFile` atomic `setApiKey` returning `ApiKeySaveResult` | `TestApiClient` and `TestRateLimiter` PASS: a 403 envelope keeps `api_key_disabled` and status 403, a parked caller resumes with the new key and the new generation, a failed save keeps the old key and reports it, cancellation unwinds a parked caller |
| KB-06, KB-11 | `Runner.{hpp,cpp}` (no blocking teardown, `cancel()`, `shutdownAll`, `post()` binder), `NativeComponentProvisioner`, `NativeAssetsPanel`, `ComponentStoreTab` | `TestRunner` PASS 21/21: destroying a Runner with work in flight returns in well under two seconds, cancel retires queued work and stops a running one at the real backend gate, `shutdownAll` drains while the GUI keeps answering, and no delivery reaches a dead owner |
| KB-07, KB-28 | `CrashLog.{hpp,cpp}` (fixed report block, Win32-only fault path, no Qt allocation, retained redacted tail) and new `Diagnostics.hpp` (`redactDiagnostic`), wired into `VideoSurface` and `Downloads` | `TestDiagnostics` PASS; `crash_probe --crash` produced a real report through the unhandled-exception filter (exception, address, module, access type, thread, tail) that contained **none** of the planted tokens while carrying the redaction markers |
| KB-12 | `ComponentStoreTab` (off-GUI `VerifyRequest`, indeterminate progress, non-reentrant controls), `main.cpp` (startup resolution as Runner work), `NativeRuntimeSelection` (`ResolutionLease`, `runtimeResolutionPending`, `whenRuntimeResolutionFinishes`), `MainWindow`/`TitlePage`/`PlayerDialog` (join the pending resolution, truthful pending line, answer after the commit) | `TestRuntimeSelection`, `TestNativeComponents` and `TestRunner` PASS; the app's own log reaches its runtime line ~0.7 s after `mikai started`, and a 25 s smoke run produced no crash report |
| KB-14 | The five setup-prompt literals ride `QT_TRANSLATE_NOOP` through the file's `translate()` helper; `RuntimeSelection`'s new strings use the same form | `lupdate` extracted 1016 strings (15 new, 3 obsolete removed); every new entry was translated and `lrelease` reports **1016 finished, 0 unfinished** |
| KB-15, KB-21, KB-24, KB-26, KB-27 | Root `CMakeLists.txt`, `CMakePresets.json`, `cmake/MikaiTestLauncher.cpp`: minimum 3.25, `-march=x86-64`, real build presets in the comments, launcher deadline plus suspended-child job join, six duplicated native TUs compiled once | `LauncherContracts` PASS against a real hung child: exit 3, the child **and** its descendant gone by PID+creation-time identity, the owned scratch removed, neighbouring files byte-identical; the six TUs appear once in the target lists |
| KB-20 | `AnimeJanaiInstallation.cpp` + `NativeComponentProvisioning.cpp`: the runtime archive is downloaded once, digest-verified, handed to provisioning through `VerifiedComponentArchive` and then reused for the `trtexec` extraction; `animeJanaiMissingDownloads`/`…Bytes` answer from stored-pack and install-record state only | `TestAnimeJanaiRuntime` PASS; the Components prompt prices the same queue the installer builds, with the Aji archive included and the runtime counted once |
| KB-22 | `MpvApi::load` reads the module's own PE imports and refuses by name when a required Windows system driver (the Vulkan loader) is missing from `%SystemRoot%\System32`; nothing is copied and no fallback module is tried | `TestRuntimeSelection::aModuleSystemDriverImportIsReadFromItsOwnImage` PASS; docs record the prerequisite as a supporting-loader fact, not a renderer |
| KB-23, KB-25 | `native/README.md`, `native/sdk-acquisition.md`, `readme.md`, `plan.md`, `native/build-msvc.md`, `native/manifest/*`, the catalogue comment: ESC-5 scoped to the private prepared-helper path with the official `trtexec` path left unresolved; landed header acquisition, executable-local roots, two-part shim provenance and `nvinfer_11` documented | Documentation inspected against the current sources; no remaining statement claims a resolved official path, a manual `MPV_INCLUDE_DIR`, a stock download, a global ESC-5 clearance or a `v2` CPU baseline |

## Current AJI custom-model integration status (2026-10-18)

The later custom-model/profile integration and its checks are separate from the
historical 28-finding audit below; this note does not imply that those original
findings were rerun. The `pathWithinRoot` volume-root containment correction in
`NativePaths.cpp` was covered by
`TestNativeManifest::volumeRootsContainDescendantsAndRemainInTheTrustChain`;
the regression checks volume-root ancestor handling and retained reparse-point
checks. It fixes root containment and does not bypass trust validation.

The recorded release build covered `mikai`, `mikai_tests`,
`native_settings_probe` and `benchmark_probe`, and the recorded CTest result
was 4/4 PASS. The Windows native-settings probe passed 220 checks, including
real file-dialog/user-action and configuration persistence flows, menus, the
Benchmark catalog, shader-only Compare, and English/Ukrainian compact/wide
layouts. These are functional UI/configuration checks. Runtime unit coverage
also exercises custom ONNX import, hash-pinned staging, planner/profile
serialization, and refusal when a registered source graph changes.

No AJI GPU inference, TensorRT engine build, RIFE benchmark, or Linux runtime
was executed for this integration. The tiny Identity fixture, planner/staging
tests, and UI probe are not inference or throughput evidence.

## Safety and lifetime

### KB-01 — P1 — Existing download destination is deleted on an overwrite refusal

**Static defect.** `downloadToFile()` defines a failure helper which unconditionally calls `QFile::remove(destination)`. Its first guard detects an already-existing destination and returns through that same helper. The refusal therefore attempts to delete a file this invocation did not create. The subsequent `WriteOnly | Truncate` open also does not enforce exclusive creation against a file appearing after the existence check.

Evidence: `src/backend/Downloads.cpp:100-119`.

[INFERENCE] A writable pre-existing file can be lost while the caller receives “refusing to overwrite”. Typical unique scratch callers reduce exposure, but the exported transport's refusal/cleanup contract is unsafe. No existing file was passed to this function in this audit.

### KB-02 — P1 — Several “isolated” probes still recursively delete the real adjacent native store

**Static defect.** Production `nativeRoot()` is `<applicationDir>/native`, independent of organization name, application name and CWD. The following probes change those names, assume `AppLocalDataLocation` isolation, then recursively remove `nativeRoot()`:

- `src/probes/native/SpatialAssetsProbe.cpp:159-170,307,396`.
- `src/probes/native/NativeAssetsProbe.cpp:348-367,687` — after the optional inspector runtime check succeeds.
- `src/probes/native/NativePlaybackAssemblyProbe.cpp:231-232,347,549-556` — the default isolated mode.

Root contract: `src/player/native/bundle/NativePaths.cpp:302-306`. The newer test-executable guard in `tests/cpp/main.cpp:79-109` and physical CTest launcher do not protect these probe entry points. Current guidance in `readme.md` and `native/README.md` correctly warns that renaming/changing CWD is insufficient.

[INFERENCE] Launching one of these probes next to an installed or development native store can delete real assets/components/models. A physically isolated executable directory avoids that exposure; the probe itself does not establish it. None was launched here.

### KB-03 — P1 — Custom native staging path can erase its read-only source input

**Static defect.** `AJI_STAGE_DIR` is a user-overridable cache path. Restaging executes `file(REMOVE_RECURSE "${AJI_STAGE_DIR}")` before copying `AJI_SOURCE_DIR`, without rejecting equality, overlap or an ancestor of the source directory. The source existence check occurs before this deletion.

Evidence: `native/CMakeLists.txt:73-75,113-121,151-182`; deletion also occurs on a failed patch at line 241.

[INFERENCE] `AJI_STAGE_DIR == AJI_SOURCE_DIR`, or a staging path that contains the source tree, can destroy the external input. The default build-local staging path is not itself problematic. No configure or destructive-path experiment was run.

### KB-04 — P1 — Distribution asset stripping does not include directories

**Static defect.** The optional Native copy is followed by `file(GLOB_RECURSE stray "${DIST_DIR}/native/*/${forbidden}")` for `components` and `assets`. CMake's recursive glob omits directories unless `LIST_DIRECTORIES true` is requested. These forbidden payload roots are directories, so this expression does not establish the promised removal boundary.

Evidence: `cmake/dist.cmake:147-172`; primary CMake `file(GLOB_RECURSE)` contract linked below.

[INFERENCE] An optional copied Native bundle containing these trees can retain downloadable/licence-restricted bytes in the assembled folder. This is a packaging-path defect, not evidence that any public shipment occurred. The `dist` target was not run.

### KB-05 — P1 — Shared API key is mutated without synchronization

**Static defect.** `MikaiClient` advertises cross-thread sharing, but `apiKey_` is an ordinary mutable `QString`. Worker requests read it while Settings Save can replace it on the GUI thread. The rate limiter's mutex does not protect this field.

Evidence: `src/backend/MikaiClient.hpp:71-76,109-110`; `src/backend/MikaiClient.cpp:433,559-573,584-587,638`; `src/app/dialogs/SettingsDialog.cpp:1488-1491`.

[INFERENCE] Saving a key while background calls copy/read it can produce a C++ data race. QString reentrancy covers different instances, not concurrent mutation and access to one instance. No race or crash was reproduced here.

### KB-06 — P2 — Worker progress dispatch still depends on a weak-pointer check across destruction

**Runtime risk.** Multiple workers read a `QPointer`, obtain a raw QObject receiver and call `QMetaObject::invokeMethod` on it. The later GUI lambda checks the weak pointer again, but that does not retain the receiver through the dispatch call. `NativeComponentProvisioner` additionally submits success/error closures capturing raw `this` through an externally owned Runner.

Evidence:

- `src/app/dialogs/ComponentStoreTab.cpp:879-905,983-1009,1123-1149`.
- `src/app/dialogs/NativeAssetsPanel.cpp:2097-2113`.
- `src/app/state/NativeComponentProvisioner.cpp:222-263`; Runner reference in `src/app/state/NativeComponentProvisioner.hpp:53-58`.

[INFERENCE] Destroying an owner during progress publication or before a queued completion can expose a receiver/closure lifetime window. Current normal close sequencing may mask it; this is not a newly observed crash. `src/player/PlayerDialog.cpp:1567-1611` already uses a different, GUI-local guard/cancellation approach, so protection is not uniform across the app.

### KB-07 — P2 — Crash handler allocates through Qt after an unhandled fault

**Static contract mismatch / runtime risk.** Final report writing uses Win32, but report construction still creates/concatenates QStrings, formats QDateTime values, calls QCoreApplication accessors, allocates a QByteArray for the log tail and converts the report to UTF-8. The code's own rationale says heap allocation in a fault handler is unsafe.

Evidence: `src/app/CrashLog.cpp:49-79,121-176,258-267`.

[INFERENCE] Heap corruption, allocator-lock failure or stack exhaustion can prevent a report or cause a secondary failure. A healthy-heap access-violation probe passing does not establish safety for those cases. No fault was triggered here.

## Backend and UI behavior

### KB-08 — P2 — HTTP API failures lose their stable error envelopes

**Static defect.** `runBlocking()` records a Qt reply error while retaining the response body. `apiGet()` throws `network_error` before parsing that body; normal API HTTP failures such as 403/404 are Qt reply errors. The stable `error.code` and server message therefore cannot reach `get()` on that path. `apiGet()` also substitutes HTTP status 0. `validateApiKey()` keeps the status but still classifies these replies as generic network errors before envelope parsing. A separate `get()` failure-envelope branch always constructs status 0.

Evidence: `src/backend/MikaiClient.cpp:196-219,441-468,598-610`; stable contract in `docs.md:72-106`; Qt QNetworkReply error classifications linked below. HTTP 429 has a separate retry branch and is not the same unhandled path.

[INFERENCE] Invalid/disabled keys, missing resources and invalid parameters can be shown as transport failures rather than their documented API cause. No API failure request was sent during this audit.

### KB-09 — P2 — Queued requests can attach old credentials to a new quota generation

**Static counterexample.** `apiGet()` constructs the request and copies its key before `acquireSlot()` waits. Saving a new key resets the limiter generation and wakes parked calls. Such a parked old-key request then acquires the new generation, is sent with the old header and supplies quota updates tagged as current.

Evidence: `src/backend/MikaiClient.cpp:269-293,329-343,395-403,430-443,561-573`.

[INFERENCE] Old-key/keyless quota headers can contaminate the new key's RPM window. This remains a logical epoch problem even if the key-field data race is independently removed. No timing scenario was exercised.

### KB-10 — P2 — Settings Save cannot report API-key persistence failure

**Static defect.** `setApiKey()` updates memory first, returns void, silently ignores failed opens and does not check the write result. Clearing the file also ignores the open result. Settings Save has no persistence outcome to inspect and continues saving/closing.

Evidence: `src/backend/MikaiClient.cpp:561-573`; `src/app/dialogs/SettingsDialog.cpp:1488-1501`.

[INFERENCE] A read-only/full/unavailable `.env` location can yield a seemingly successful Save but a different key after restart. Truncation plus a partial write can also damage the previous persisted key. No permissions or disk-full condition was induced.

### KB-11 — P2 — Runner's 30-second wait is not a shutdown bound

**Static lifecycle mismatch.** Runner owns four QObject-child QThreadPools. Its destructor ignores the result of `waitForDone(30000)`. Qt then destroys those pools, whose destructors wait for outstanding work regardless of the preceding timed wait. API quota waits have no cancellation condition.

Evidence: `src/app/workers/Runner.cpp:113-138,163-173`; `src/backend/MikaiClient.cpp:269-293`; page disposal in `src/app/pages/MainWindow.cpp:776-790`. Qt QThreadPool destructor behavior linked below.

[INFERENCE] Navigation/closing that destroys a Runner can block the GUI past 30 seconds while queued network/rate-limit work drains. Callback invalidation alone does not cancel this work. No long-running shutdown was exercised.

### KB-12 — P2 — Components verification hashes large inventories on the GUI thread

**Static responsiveness weakness.** The Verify button directly invokes `verifyAll()`, which synchronously verifies compatible packs, the backend closure, installed RIFE graphs and the spatial catalogue. The pack/RIFE verification functions read/hash actual files; no worker handoff occurs in that call.

Evidence: `src/app/dialogs/ComponentStoreTab.cpp:379,648-738`; `src/player/native/components/NativeComponentStore.cpp:560-572,743-788`.

[INFERENCE] A populated installation can freeze repaint/input during verification. No verification timing was measured. Related documented limitation: the zero-delay callback in `src/app/main.cpp:61-64` still performs runtime selection/hash work on the GUI thread; deferring it is not asynchronous execution.

### KB-13 — P2 — Downloader verifies received bytes, not successful file writes

**Static defect.** `downloadToFile()` ignores `file.write(chunk)`'s result, hashes the incoming chunk and increments `received` by the chunk size regardless of what reached disk. It closes the file without inspecting write/flush errors and returns success if network bytes/digest match.

Evidence: `src/backend/Downloads.cpp:139-150,191-226`; Qt QIODevice write-result contract linked below.

[INFERENCE] A short write or storage failure can be reported as a successful verified download even when the destination is incomplete. Later archive/model verification can catch some downstream cases; it does not make this transport verdict truthful. No disk failure was induced.

### KB-14 — P2 — Three setup-prompt strings are missing from the Ukrainian catalogue

**Static extraction/catalogue mismatch.** These literals are passed through a local `translate(const char*)` wrapper without extraction markers and are absent from the `mikai::app::ComponentStoreTab` context in `translations/mikai_uk.ts`:

- `DirectML backend`.
- `Components recommended for this PC are not installed:`.
- `Download and install them now (%1 MB)?`.

Evidence: `src/app/dialogs/ComponentStoreTab.cpp:122-123,151,184-186`; direct context lookup in the translation source. The newer spatial-pack prompt is present and is not included in this finding.

[INFERENCE] These paths can show English text in Ukrainian UI. Zero unfinished translations does not detect strings that were never extracted. No catalogue rebuild or UI run was performed.

## Build, dependency and deployment contracts

### KB-15 — P2 — Declared CMake minimum cannot parse the preset format

**Static version mismatch.** `CMakePresets.json` declares schema version 6 and minimum CMake 3.24; root CMake also declares 3.24. Preset schema version 6 was introduced in CMake 3.25.

Evidence: `CMakePresets.json:1-3`; `CMakeLists.txt:1`; CMake 3.25 preset manual linked below.

[INFERENCE] The documented preset workflow fails on part of its claimed supported CMake range. A non-preset configure may have a different minimum. No older CMake binary was run.

### KB-16 — P2 — Patch ledger is incomplete and names the wrong shim base

**Static provenance mismatch.** The root shim series contains five patch files, while `sources.json#patchSeries.seriesFiles` records three. Its recorded three-file digest is correct for those three files but does not describe the current sorted CMake input set. The separate mpv inventory similarly omits an available fourth patch:

- Unlisted shim inputs: `native/patches/0004-wide-win32-paths-and-loaders.patch`, `native/patches/0005-rife-heavy-model-mapping.patch`.
- Unlisted mpv-series file: `native/patches/mpv/0003-wide-utf8-library-load.patch`.

The heavy-mapping patch is mentioned elsewhere under `localBuildExtension`; the issue is its absence from the series identity, not total absence from the manifest. The shim series `baseCommit` is `63d127…`, which is the recorded **mpv-fork** revision. The native shim build instructions name `738ce3…`, while the current inference source entry records the official v0.9.0 baseline `c7f56…` plus authorized mutable local changes.

Evidence: `native/CMakeLists.txt:113-121,144-159`; `native/manifest/sources.json:194-243,277-281,1226-1265,1336-1338`; actual patch-file inventory and hashes inspected directly.

[INFERENCE] A recipient cannot reconstruct the currently intended shim input from the claimed exact-series/base identity. Individual listed digests passing does not establish complete provenance. No patches were applied and no claim is made that every available mpv patch is present in the current packaged author DLL.

### KB-17 — P2 — Native staged-source identity misses almost all source edits

**Static counterexample.** Restaging identity consists of patch-file hashes, the source path and only the first 16 bytes of `src/aji_trt.cpp`. It does not fingerprint the complete source input or enforce the claimed pinned revision. A changed header, dispatcher, DML source or later TRT code can leave both stamps valid and retain the old staged tree.

Evidence: `native/CMakeLists.txt:151-176,194-205`; current mutable-input boundary in `native/manifest/sources.json:226-243`.

[INFERENCE] Reconfiguration can silently build stale source after authorized edits. No external tree was modified to demonstrate this.

### KB-18 — P2 — Native patch-policy behavior disagrees with its advertised modes

**Static defects.** `strict` advertises refusal without a series, but an empty glob only warns and bypasses the fatal condition because it is gated by `AJI_PATCH_FILES`. `off` advertises skipping the series, but the application loop is not gated on that mode. The loop tracks whether any patch applied, not whether every patch did; a mixture containing skipped description-only patches can still receive a full-series stamp.

Evidence: `native/CMakeLists.txt:70-72,208-257,263-278`.

[INFERENCE] A supposedly strict build can admit no/partial patches, while an off build can still apply patches. No configuration combinations were run.

### KB-19 — P2 — Optional Native distribution searches an obsolete root; root override is not forwarded

**Static mismatch / workflow caveat.** `dist.cmake` defaults to `%LOCALAPPDATA%/Mikai/Mikai Wrapper/native`, while production ownership is executable-local. The script recognizes `MIKAI_DIST_NATIVE_ROOT`, but the normal root `dist` target does not forward it to the child `cmake -P` invocation.

Evidence: `cmake/dist.cmake:146-158`; `src/player/native/bundle/NativePaths.cpp:302-306`; `CMakeLists.txt:831-859`.

[INFERENCE] `MIKAI_DIST_NATIVE=ON` can quietly skip the actual runtime, and setting the similarly named parent cache variable does not supply the intended override through that target.

Separate, already documented safety caveat: `cmake/dist.cmake:97` unconditionally recreates the output directory, whose default is the user's installed `dist-mikai`. Running it there destroys adjacent installed state. Current in-place executable-only update guidance is intentional; no dist/cleanup operation was performed here.

### KB-20 — P2 — Cold TensorRT setup downloads the runtime archive twice and underestimates setup bytes

**Static redundant acquisition.** If the component store lacks the runtime pack, `provisionAnimeJanaiRuntime()` first provisions it through `provisionTensorRtComponents()`. That function downloads the archive, installs its selected files, then removes the archive. The caller subsequently downloads the same runtime archive again to extract `trtexec.exe`. The pack is 186,822,775 bytes. The setup prompt counts the runtime once and omits the mandatory 657,798-byte Aji archive; its DirectML estimate omits that Aji archive too.

Evidence: `src/player/native/components/AnimeJanaiInstallation.cpp:40-42,718-727,757-799`; `src/player/native/components/NativeComponentProvisioning.cpp:182-216`; `src/player/native/components/NativeComponentCatalog.cpp:37-40`; `src/app/dialogs/ComponentStoreTab.cpp:130-153,182-187`.

[INFERENCE] A cold install incurs an unnecessary second large transfer, and the confirmation estimate does not match the actual acquisition queue. A warm component store avoids the first runtime download. No download was started in this audit.

### KB-21 — P2 — Release CPU baseline is stronger than “any Windows x64 box”

**Static compatibility/documentation mismatch.** Release targets use `-march=x86-64-v2`, while the adjacent build rationale claims compatibility with any Windows x64 box. x64 alone does not imply the v2 instruction set; GCC distinguishes the baseline explicitly.

Evidence: `CMakeLists.txt:866-878,910-921`; GCC x86 options linked below.

[INFERENCE] Older x64 CPUs can be unsupported even though they satisfy the advertised architecture wording. The current RTX-machine result does not cover them; no older CPU was tested.

### KB-22 — P2 — Packaged mpv has a hard Vulkan-loader dependency despite OpenGL rendering

**Observed static binary fact / deployment caveat.** Read-only `objdump -p` inspection of `dist-mikai/mpv/libmpv-2.dll` lists `vulkan-1.dll` as an import. `native/manifest/THIRD_PARTY_NOTICES.md:592-595` already records this requirement. Qt deployment runs before the mpv DLL is copied, so that step is not evidence that the later DLL's dependency is provisioned.

Evidence: `cmake/dist.cmake:104-110,137-144`; static PE import inspection; the third-party notice.

[INFERENCE] A target system without the Vulkan loader can refuse the module before playback, despite the player using OpenGL. This is a supporting DLL requirement, not a claim that the app renders through Vulkan. No clean-machine launch was performed.

## Documentation and developer workflow

### KB-23 — P2 — ESC-5 is described as resolved where the ledger says the official path is unresolved

**Static documentation contradiction.** The native guide's status heading/table and the catalogue comment imply `trtexec` is no longer needed. The current escalation/notice distinguishes the private in-process helper from the official-config TensorRT first-build, which still requires and receives `trtexec.exe`.

Evidence: `native/README.md:1036-1040,1595-1615`; `native/manifest/components.json` escalation `ESC-5`; `native/manifest/THIRD_PARTY_NOTICES.md:225-242,627`; `src/player/native/components/NativeComponentCatalog.cpp:36`; `src/player/native/components/AnimeJanaiRuntime.cpp:254-260,352-354`; copying in `src/player/native/components/AnimeJanaiInstallation.cpp:914-919`.

[INFERENCE] Unqualified “resolved” wording can misstate the official path's dependency/redistribution clearance. The owner's project-code grant does not establish third-party rights. This report does not make a new legal determination or claim a public shipment.

### KB-24 — P2 — Maintained probe/fixture instructions contain non-working build and launch paths

**Static command/reference mismatch.** Root CMake comments use `cmake --build --preset release --target …`, but `release` is a configure preset, not one of the four build presets. The fixture guide still uses removed `build-cmake` and launches `build-cmake/mikai_http_fixture.exe` from the build root. The target is declared in the `tests/fixtures` subdirectory, with no root output-directory override; current readme examples use that nested location.

Evidence: `CMakeLists.txt:112-123`; `CMakePresets.json:45-50`; `tests/fixtures/README.md:89-99`; `tests/fixtures/CMakeLists.txt:11-17`; `readme.md` fixture instructions.

[INFERENCE] Copying these commands fails or targets an obsolete tree even though the current build works. No invalid command was executed merely to confirm the source mismatch.

### KB-25 — P2 — Landed/provisioning documentation still mixes removed and current contracts

**Static documentation drift.** `plan.md` §2.1 labels the loader landed but still requires manual `MPV_INCLUDE_DIR`. Current root CMake acquires the pinned archive and derives matching headers. Later plan sections describe live latest-release stock downloads/fallback storage, while current runtime discovery explicitly uses packaged/local-hint modules only. The proposed storage hierarchy also retains `AppLocalDataLocation`, unlike the landed executable-local implementation. The native pin table says all nine shim inputs match `738ce3…`, but current source reconciliation explicitly records authorized mutable changes and `matchesPinnedCommit: false`. Its `AJI_NVINFER` row describes a default of 10 while CMake's default is 11.

Evidence: `plan.md:65-73,929-944,1427-1432,1603-1605`; `CMakeLists.txt:138-193`; `src/backend/Runtime.cpp:34-46`; `native/README.md:1068-1070,1257-1264`; `native/manifest/sources.json:194-243`; `native/CMakeLists.txt:58-61`.

[INFERENCE] Developers can select wrong build inputs/storage assumptions or overstate reproducibility. Explicitly marked historical §2.2–§2.6 snapshots and PROPOSED interface paths in `tasks.md` are not treated as current missing implementations. This finding does not claim the removed stock downloader still runs.

### KB-26 — P2 — Test launcher has no internal deadline for a hung child

**Static DX weakness.** The launcher calls `waitForFinished(-1)`; its kill branch is entered only after that wait returns unsuccessfully. The CTest registration supplies environment configuration but no per-test TIMEOUT. The launcher relies on an external timeout/termination policy, and scratch removal happens only after the child returns.

Evidence: `cmake/MikaiTestLauncher.cpp:76-88,108-125`; `CMakeLists.txt:625-641`.

[INFERENCE] Direct launcher use can wait indefinitely on a hung child. External CTest/global timeout settings may bound a particular invocation, but are not an internal launcher deadline. Forced-parent-termination cleanup/child lifetime was not exercised. The normal isolated test cutover remains valid prior evidence.

### KB-27 — P3 — Six native translation units have two compilation owners

**Static maintenance/DX weakness.** `NativeRuntimeSelection`, `NativeStatus`, `NativePlaybackController`, `NativePrefs`, `NativeCapabilities` and `NativeMenu` are in both `mikai_native_support` and `PLAYER_SOURCES`; the player also links the support library. The two targets have different release-tuning coverage.

Evidence: `CMakeLists.txt:232-240,267-272,330-338,415-420,910-921`.

[INFERENCE] This adds redundant compilation and makes future compile-definition/link-ownership differences harder to reason about. Static archive extraction often prevents duplicate-definition errors; this is not a claim of an observed linker failure or duplicated live state.

### KB-28 — P2 — “Tokens never enter diagnostics” is not enforced at the logging boundary

**Security caveat, not a confirmed secret disclosure.** VideoSurface forwards received mpv log text verbatim to Qt logging; CrashLog persists that text and includes it in fault reports. Neither boundary redacts media URLs. Transport error strings also embed the full request URL. The plan states that URL tokens never enter logs/exported diagnostics.

Evidence: `src/player/VideoSurface.cpp:294-300`; `src/app/CrashLog.cpp:207-230,164-176`; `src/backend/Downloads.cpp:93-96,201-209`; `plan.md:1593-1598`.

[INFERENCE] If a backend emits a signed media URL, its token can be preserved in shared diagnostic files. No user's token, `.env` contents or private URL was copied into this report; no new leakage was reproduced.

## Static checks with no issue in the inspected set

| Check | Observed result |
| --- | --- |
| Maintained content documentation | 19 Markdown documents inspected; additional targeted mandatory repository guidance read |
| Parsed local Markdown link destinations | 158 found; no missing destination, heading-anchor or path-case mismatch in that parsed set |
| Plain source/path mentions | 101 mentions / 47 distinct strings; 20 non-literal repository-path candidates examined in context, not counted as 20 broken links: external shim paths, runtime-created records, shorthand and explicitly proposed interfaces account for them |
| Root CMake source/header/script references | 282 parsed actual path entries found; comment examples excluded from file-existence accounting |
| Manifest source/component references | No dangling inspected `sourceId` or bundle component ID |
| Individually recorded patch hashes | All six recorded shim/mpv patch-file digests match their present files; inventory incompleteness is KB-16 |
| Packaged API DLL identity | Present `dist-mikai/mpv/libmpv-2.dll` is 122,146,816 bytes and matches the recorded author-module SHA-256 `90de8f89fa1421eaec510e5cfe11de75846b445489864220ec811fd0f08b4649` |
| Public/private contract | Current top-level native guidance correctly separates official ABI-8 filtering, explicit local custom import and private prepared-Native admission |

These checks do not establish that every external URL is live, every third-party dependency is redistributable, every source path was audited exhaustively, or the current application passes a new runtime check.

## Existing evidence and remaining boundaries

The current leading acceptance sections in `AGENTS.md`, `readme.md` and `native/README.md` supersede older player/Compare failure snapshots. This audit does not reopen the already diagnosed captioned blend corruption or pretend the earlier successful HLS/built-in, fresh-engine pause/progress/resume, Compare, Settings and isolated-suite runs were rerun. Their exact prior artifacts remain in the native guide.

Numerical fidelity, all-model/all-GPU inference, independent engine-to-active-model identity, private Ready/Active admission and third-party redistribution remain scoped limitations. Component acquisition/configuration is not inference or rights clearance. The stat-only validation cache is intentionally a performance cache; unchanged path/size/mtime is not fresh byte-integrity proof after externally preserving metadata.

Graph project: `F-ZKS-mikai_janai`, generation `2026-10-11T01:24:21Z`. Coverage queries reported changed metadata for most audited paths and several partial C++ ranges; material claims use current source reads rather than stale graph output. Qt name collisions produced implausible call edges, and Qt translation/media `.ts` files were misclassified as TypeScript. Those are audit-tool limitations, not backend layering bugs. No LSP was configured. No claim of whole-repository graph completeness, dead code or complete impact is made.

Excluded from broad maintained-doc/source accounting: generated build/external dependency trees, large diagnostic archives and private environment contents. Installed runtime/component/model state was not mutated. Historical or proposed text was checked for scope before being classified as a current contradiction.

## Primary documentation consulted

- [CMake 3.25 preset schema history](https://cmake.org/cmake/help/v3.25/manual/cmake-presets.7.html).
- [CMake file command — recursive glob directory handling](https://cmake.org/cmake/help/latest/command/file.html#filesystem).
- [Qt 6.8 reentrancy versus thread safety](https://doc.qt.io/qt-6.8/threads-reentrancy.html).
- [Qt 6.8 QNetworkReply error classifications](https://doc.qt.io/qt-6.8/qnetworkreply.html#NetworkError-enum).
- [Qt 6.8 QThreadPool destruction and timed waiting](https://doc.qt.io/qt-6.8/qthreadpool.html).
- [Qt 6.8 QIODevice write return value](https://doc.qt.io/qt-6.8/qiodevice.html#write).
- [GCC x86 architecture options](https://gcc.gnu.org/onlinedocs/gcc/x86-Options.html).
