# Release preparation status

Status: **published, with open rights and QA work**. The owner published [v0.1.0](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0) with the six artifacts recorded below; `profiles.publicReleaseReady` remains false for `windows-x64` and `linux-x86_64`, and the third-party source/redistribution obligations in [`redistribution.md`](redistribution.md) are still unresolved. A successful live gallery capture was recorded on 2026-10-30, but it is not a release QA pass, rights clearance or authorization to distribute.

## Implemented in this source tree

- Original Mikai Ja Nai full-color application icon and its packaging/integration wiring, plus a themed transparent monochrome header icon.
- Windows x64 portable ZIP and per-user installer generation scripts, payload inventories, checksums and provenance records.
- Linux x86_64 packaging with system-mpv and private bundled-mpv profiles, optional Qt deployment and relative runtime search paths.
- Source-snapshot packaging that hashes archived bytes, excludes private/runtime data and records dirty worktree state.
- CI and protected release-preparation workflows, trusted-tag ancestry checks and redistribution gates.
- Documentation for build, packaging, redistribution, gallery capture and source provenance.

## Build evidence

**Local Haruna source review (2026-10-31, uncommitted working tree):** Windows release application, tests and rendering probes built successfully. Checks ran from owned staged executable directories, not beside an installed native store.

| Check | Observed result |
| --- | --- |
| Complete CTest | 58/58 passed, including aggregate and per-suite entries |
| Native playback controls | 19 passed, 0 failed, 0 skipped |
| Native application UI / pagers | 21 and 17 passed respectively, with no failures or skips |
| Native long-controls layout | Passed at 860 and 1080 logical pixels |
| Render lifecycle | 114 requirements passed in each run at actual DPR 1.75 and 3.0625 |
| Original fullscreen/PiP | 70 seconds; 2,165 sampled compositions; 0 black, 0 stalled seconds; maximum GUI gap 181 ms |
| Anime4K Fast A fullscreen/PiP | 70 seconds; 2,134 sampled compositions; 0 black, 0 stalled seconds; maximum GUI gap 173 ms; core shader list/files confirmed |

Local JSON/logs and functional fixture captures are under `.probe-out/haruna-review/`. The full suite passed after temporary layout diagnostics were removed. These bounded checks do not establish that every intermittent black flash is resolved, certify each shader or inference backend, or refresh the public gallery. Linux was not rebuilt/run because the registered WSL VHDX is missing. No TensorRT build, inference benchmark, installer check, package regeneration, signing or publication approval is claimed for this source review. The historical artifact records below remain unchanged.

- **Published release (2026-10-30, revision `488b928`):** [v0.1.0](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0) carries six artifacts and `SHA256SUMS` — AppImage, Linux `tar.gz`, Arch `pkg.tar.zst`, Debian `.deb`, Windows portable ZIP and Windows setup. The retained historical record in commit `cb8d4ac` attributes those six artifacts and historical runtime checks to revision `488b928`; it records the downloaded `tar.gz` re-hashing to its published digest. On 2026-10-30, direct inspection of the Linux tarball and Windows portable ZIP `VERSION` files identified revision `488b928`; the other four artifacts were not independently inspected in that check. The release page states the same limitations as this document: no signing, no universal Linux package, and unresolved third-party source/redistribution obligations.

- **Windows product build (2026-10-29, revision `fe4db7e`):** cross-built with MinGW-w64 GCC 13 (`-DMIKAI_DISABLE_IPO=ON`, `-DMIKAI_BUILD_REVISION=fe4db7e`); `windeployqt.exe` crashes under wine, so the payload (132 files) is assembled from the `dist.cmake` allowlist with Qt 6.12, the MinGW runtime and the pinned `mpv/libmpv-2.dll` (`d22a8a765abe`, full: `packaging/provenance/windows-runtime.json`, the dev archive's DLL). `mikai-0.1.0-windows-x64-portable.zip` (`b31c3877`, truncated historical 8-character digest prefix; full digest not retained here, 77.8 MB (77,767,851 B)) answers `mikai 0.1.0 (fe4db7e, …)` under wine from its own folder; `mikai-0.1.0-windows-x64-setup.exe` (`dd52285b`, truncated historical 8-character digest prefix; full digest not retained here, 58.8 MB (58,819,163 B)) silently installed to a wine prefix, ran with the same version, carried the same DLL and uninstalled cleanly.
- **Windows product build (older record):** an earlier snapshot's build is recorded as having succeeded; this document does not rerun builds.
- **Linux product build:** native Ubuntu 24.04 WSL2 ext4 source build with GCC 13.3 and Qt 6.12 completed target `mikai` with exit code 0.
- **Source snapshot (2026-10-29, revision `9372c03`):** Linux `mikai` builds with GCC on Arch, the hermetic CTest suite passes **55/55** (`ctest --test-dir build/linux-release --output-on-failure`), and the AppImage and dependency-based archive are built locally from that revision. The Windows x64 binary is cross-built with MinGW-w64 GCC 13 (`-DMIKAI_DISABLE_IPO=ON`, `-DMIKAI_BUILD_REVISION=9372c03`) and its payload is assembled from the `dist.cmake` allowlist; `windeployqt.exe` crashes under wine, so it is not used. The installer and portable ZIP are produced by Inno Setup 6.7.3 under wine. Each artifact answers `--version` with `mikai 0.1.0 (9372c03, built …)`.
- **Bundled runtime (2026-10-30, revision `488b928`):** every artifact ships the pinned libmpv runtime — `bin/mpv` in the tarball and AppImage, `usr/lib/mikai/mpv` in the Arch and Debian packages, `mpv\libmpv-2.dll` on Windows. The Linux runtime is the fork rebuilt with an FFmpeg TLS backend by `packaging/mpv/build-tls-archive.sh` (`mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst`, 41.5 MB (41,533,948 B), SHA-256 `6796d922cfd3`, full: `packaging/provenance/linux-runtime.json`); the published archive could not open any https URL, and the rebuilt one decoded an https HLS stream through `MpvHandle` (634.58 s, h264). AnimeJaNai identifies that fork on Unix by its `animejanai` filter and client API 2.6 (the Windows fork is 2.5), which a harness confirmed against both the bundled module and the distribution's mpv. The retained historical record in commit `cb8d4ac` attributes all six artifacts and historical runtime checks to revision `488b928`; it does not attribute them to `e608684`. The Linux tarball and Windows portable ZIP `VERSION` files inspected on 2026-10-30 both identify `488b928`:

| Artifact | Size | SHA-256 |
| --- | ---: | --- |
| `mikai-0.1.0-linux-x86_64.tar.gz` | 44.6 MB (44,555,221 B) | `3602d85f4d82` |
| `mikai-0.1.0-x86_64.AppImage` | 165.1 MB (165,075,448 B) | `e6d41f573585` |
| `mikai-0.1.0-1-x86_64.pkg.tar.zst` | 42.6 MB (42,593,993 B) | `3f804f11e469` |
| `mikai_0.1.0_amd64.deb` | 65.7 MB (65,658,032 B) | `9b2e31f58f45` |
| `mikai-0.1.0-windows-x64-portable.zip` | 78.1 MB (78,110,642 B) | `e4024d5a51a3` |
| `mikai-0.1.0-windows-x64-setup.exe` | 58.8 MB (58,824,161 B) | `46eeaf00c769` |

The published Linux digests remain in [`release-artifacts/SHA256SUMS`](../release-artifacts/SHA256SUMS); the original published Windows digests are recorded in the table below. The local Windows files and checksum entries have since been replaced by private-review builds and are not the published release.

  The historical record in commit `cb8d4ac` records runtime checks for the six artifacts, including the tarball, AppImage, wine-run executable, both Linux packages and Windows installer. Those are historical results, not fresh runtime verification; only the tarball and Windows portable ZIP `VERSION` files were directly inspected in the 2026-10-30 check.
- **Distribution packages (2026-10-29, revision `525afc9`; superseded by the bundled-runtime entry above):** `mikai-0.1.0-1-x86_64.pkg.tar.zst` is built by `makepkg` from a git export against Arch's Qt 6.12 and `mpv` (SHA-256 `cd54270dd0e7`, 4.6 MB (4,604,413 B); full: [historical package digest table](#historical-distribution-package-digests)); it installed, answered `--version` and removed cleanly in an `archlinux` container. `mikai_0.1.0_amd64.deb` is built inside Debian 13 with the Qt 6.12 SDK plus ICU 73 under `/opt/mikai` and the distribution's `libmpv2` (SHA-256 `6f93a280d366`, 25.7 MB (25,707,292 B); full: [historical package digest table](#historical-distribution-package-digests)); it installed from a clean `debian:trixie-slim` with `apt-get install ./mikai_0.1.0_amd64.deb`, answered `--version`, started as an unprivileged user with the offscreen platform plugin, and removed cleanly. Both report `mikai 0.1.0 (525afc9, …)`. No playback run, signing or publication approval is implied.

### Historical distribution package digests

| Artifact | Full SHA-256 |
| --- | --- |
| `mikai-0.1.0-windows-x64-portable.zip` (published revision `488b928`) | `e4024d5a51a3fbd6eceff0944dab33d981b616a3ebe1c15628cf4c1b2eabaee8` |
| `mikai-0.1.0-windows-x64-setup.exe` (published revision `488b928`) | `46eeaf00c769da3bd093f851856d6cc7b72c117ee6781e6d61300b1d5a140775` |
| `mikai-0.1.0-1-x86_64.pkg.tar.zst` (revision `525afc9`) | `cd54270dd0e7e9b556a3c99b12e4f1c6b0c89ef6823fad97029fa293f9cde9a8` |
| `mikai_0.1.0_amd64.deb` (revision `525afc9`) | `6f93a280d366b44a61eccceedfa9fe9e390593c098134abb94edf7b6b7b48082` |
- **What that is not:** these are local builds and a local test result. No GPU inference run, no gallery capture, no signing and no publication gate run is claimed here. Windows installer upgrade/uninstall evidence recorded earlier belongs to the preceding revision and is not restated for `9372c03`.

- **Windows startup runtime fix (2026-10-30):** the released `Qt6Core.dll` imported `__cxa_call_terminate` from `libstdc++-6.dll`, but the released runtime did not export it. The fixed distribution prefers SDK runtime files; both Windows assembly paths check this specific import against the selected runtime's exports. A local private-review ZIP and extracted setup payload each passed `--version` with `PATH` limited to Windows system directories. The setup was not installed or uninstalled, and this is not a clean-machine test. No Linux verification is claimed; WSL was unavailable because its VHDX was missing. These local review outputs are not a public release or rights-clearance evidence.
  - The current local private-review ZIP (71.3 MB (71,327,637 B), SHA-256 `deaaa39f2281`) and setup (54.2 MB (54,175,060 B), SHA-256 `0d610b7c367d`) are in `release-artifacts/`; full digests are in `release-artifacts/SHA256SUMS`. They were built from clean commit `c039960`; `VERSION` records `c039960367f6c2929e60f9221937a79492163597` and staging at `2026-10-31T20:45:44Z`. The extracted portable payload matched all 138 files and hashes in `mikai-0.1.0-windows-x64-release.json`, including all five benchmark clips, and its executable reported `mikai 0.1.0 (c039960, built 2026-10-31T20:43:59Z)` with `PATH` limited to Windows system directories. These packages carry the benchmark-clip packaging fix and both volume fixes.
  - An isolated copy of the preceding portable payload (`bbe452a`) completed 1920×1080 baseline and Anime4K Fast A benchmark rows using its own packaged clip; no TensorRT build or inference was run. At the 940×820 default Settings size the benchmark UI had no horizontal overflow. At the 580×600 minimum, both rows completed but the benchmark page overflowed horizontally; that separate layout issue remains open. The current setup was not installed or uninstalled, and no upgrade/repair or clean-machine result is claimed. The packages are unsigned and remain blocked from public redistribution.
- **AnimeJaNai engine-build fix (2026-10-30, revision `affc0ff`):** once the loader path was fixed the build failed with `sh: line 1: Wrapper/native/components/aji/v0.17.1/trt: No such file or directory` (exit 127). The fork prefixes its trtexec invocation with `LD_LIBRARY_PATH=<trtexec-libdir>` **unquoted** and hands the line to `/bin/sh`, so the store folder `Mikai Wrapper` split the word. `detail::shellSafeLibDir()` now returns a symlink mirror of the closure under a space-free temp root when the real path is splittable, and both plan builders pass it as `trtexec-libdir`; the verified closure is untouched and Windows (no shell) is unchanged. Evidence: with the mirror, `trtexec` reported `&&&& PASSED TensorRT.trtexec [TensorRT v110300]`, wrote `aji-ac41ab87…engine` (1,796,604 B) and playback upscaled 1920x1080 → 3840x2160; the same command with the space-containing dir reproduces the build-log error. Test: `TestAnimeJanaiRuntime::shellSafeLibDirAvoidsASplitLibraryPath`.
- **AnimeJaNai inference-context fix + AppImage logging (2026-10-30, revision `8a3b2cd`):** the fork's `aji_trt` backend resolves `libnvinfer.so.11` by bare soname, so on Linux the TensorRT closure folder is now preloaded `RTLD_GLOBAL` before the `animejanai` filter attaches (`AnimeJanaiPlan::backendLibDirs`, consumed in `VideoSurfaceAnimeJanai.cpp`). Without it the filter failed with the opaque `Failed to create inference context`; the fork CLI reproduces both states (`libnvinfer.so.11: cannot open shared object file` vs `backend: TensorRT (aji_trt)` initialised and streaming). The session log also falls back to `AppLocalDataLocation` when the executable folder is read-only, so AppImage runs now write a readable `mikai.log` (verified from an extracted AppImage with a read-only `usr/bin`), and AJI failures are logged under `[animejanai]`. Tests: `TestAnimeJanaiRuntime::tensorRtPlanCarriesTheBackendLoaderDir`, `TestDiagnostics::aBlockedPreferredLogFolderFallsBackToThePerUserFolder` now covers a blocked preferred write probe without relying on Windows directory permission attributes.

## CI status

- **Linux job green** at `f0d2122` (run 41008891086): provisioned Qt 6.12, built the TLS runtime in-run and bundled it via `-DMIKAI_LINUX_MPV_EXPECTED_SHA256` (digest recorded in `evidence/linux-mpv-runtime.json`), passed the hermetic suite and packaged.
- **Windows job red** at the same revision, in `DistContracts` (test 2/54): the fixture assembly that must strip a forbidden payload instead fails, and every dependent check then reports `FAIL`. This is the first run of that Windows-only contract on a GitHub runner and it is unrelated to the app or to the release artifacts, which were produced with the local MinGW cross build and verified under wine. It is not claimed as passing; the contract needs a Windows-host run to reproduce.

## Playback observation tracking

Two owner-reported playback observations from the 2026-10-11 audit (KB-29 and KB-30) are under continued measurement. They are **not static defects** and are not release blockers for the current scope (Original playback, UI, component acquisition).

| Observation | Status | What remains |
|---|---|---|
| KB-29 — HDR dark-scene instability | Under measurement | Controlled hardware A/B comparison pending; FBO attachment and tone-mapping behavior documented; Stable (`bt.2390` + `clip`) and Vivid (`mobius`) policies implemented; Anime4K/AJE/AJI processing gates in place |
| KB-30 — Periodic frame substitution | Under measurement | Client-side pacing implemented with module-bound clock and version-aware deadline conversion; causal proof pending; controlled uninterrupted aging run needed |

Both observations require physical hardware runs to close. Neither invalidates the previously recorded Linux hermetic test run (**55/55** PASS) or the documented UI/component-acquisition behavior.

## Blocked or not verified

- **Release QA:** not passed under the current policy. The 2026-10-30 Windows CTest run passed 52 of 55 entries; the aggregate suite, `TestMpvAsync`, and `TestDiagnostics` failed. The failures were a timed-out event-backlog test and a read-only-log-folder assertion. The publication gate in `packaging/ci/release.py` has no green run recorded, so neither the passing checks nor the successful gallery capture constitute release approval.
- **Windows installer smoke:** not claimed as passed here; it is handled separately by the Windows installer smoke gate.
- **Windows mpv corresponding-source/license closure:** unresolved. Logs retained for the superseded `19c0fcf895` build establish a GPL-enabled, non-LGPL build; the currently pinned `b4f9f5bfed` build has no retained logs, so its license mode is not established and must not be inferred from the earlier one. Either way public redistribution remains blocked.
- **Linux build/package:** a local build, test run and package for `9372c03` are recorded above; a Linux runtime acceptance pass on target hardware and a distribution review are still not claimed.
- **Further public binary distribution:** blocked pending resolution of the third-party source/redistribution obligations and release QA. The existing v0.1.0 publication is not approval for further publication, tagging or upload.

## Gallery capture record

The 2026-10-30 Windows `release_gallery_probe` run recorded all five real widget surfaces and all three supporting appearance captures, with successful application shutdown. Its unmodified `gallery-evidence.json` and full-resolution, non-duplicate PNG files are in `docs/images/screenshots/`; the raw JSON retains the appearance-reset record and SHA-256 even though its PNG is omitted as byte-identical to appearance-before. The historical 2026-10-24 run remains distinct in `docs/images/gallery-release-0.1.0/`, with the same duplicate-reset handling and unchanged historical JSON. This is capture evidence only; open QA and third-party rights/redistribution issues continue to block further public distribution.

Capture procedure and provenance requirements: [docs/images/README.md](images/README.md).

## Source snapshot documentation

Source packaging remains documented and must be preserved:

- Local source preview: `packaging/source/package.py`
- Source packaging notes: `packaging/source/README.md`
- Provenance records `format` 1, artifact `mikai-0.1.0-source.tar.gz`, version `0.1.0`, `snapshot_kind` `local-worktree-source-snapshot`, `head`, `dirty`, `commit_equivalence` `not-claimed`, `tree_sha256`, and per-file hashes.
- A dirty worktree snapshot is not commit-equivalent and does not authorize publication.

## Downloads

The existing public download is the [published v0.1.0 release](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0), with its six artifacts and checksums listed above. Do not download binaries from an unofficial mirror. The publication does not constitute approval for further distribution; the open rights and QA gates remain. See [docs/releasing.md](releasing.md) and [docs/redistribution.md](redistribution.md) for packaging and rights limitations. Historical artifact identities and checks remain historical evidence, not current verification or publication approval.