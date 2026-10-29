# Release preparation status

Status: **partial and blocked**. This document records source-first preparation and local packaging tooling only. It does not claim a public release, publication approval, current QA pass, or current gallery capture. `profiles.publicReleaseReady` remains false for `windows-x64` and `linux-x86_64`.

## Implemented in this source tree

- Original Mikai application icon and its packaging/integration wiring.
- Windows x64 portable ZIP and per-user installer generation scripts, payload inventories, checksums and provenance records.
- Linux x86_64 packaging with system-mpv and private bundled-mpv profiles, optional Qt deployment and relative runtime search paths.
- Source-snapshot packaging that hashes archived bytes, excludes private/runtime data and records dirty worktree state.
- CI and protected release-preparation workflows, trusted-tag ancestry checks and redistribution gates.
- Documentation for build, packaging, redistribution, gallery capture and source provenance.

## Build evidence

- **Windows product build (2026-10-29, revision `fe4db7e`):** cross-built with MinGW-w64 GCC 13 (`-DMIKAI_DISABLE_IPO=ON`, `-DMIKAI_BUILD_REVISION=fe4db7e`); `windeployqt.exe` crashes under wine, so the payload (132 files) is assembled from the `dist.cmake` allowlist with Qt 6.12, the MinGW runtime and the pinned `mpv/libmpv-2.dll` (`d22a8a76…`, the dev archive's DLL). `mikai-0.1.0-windows-x64-portable.zip` (`b31c3877…`, 77,767,851 B) answers `mikai 0.1.0 (fe4db7e, …)` under wine from its own folder; `mikai-0.1.0-windows-x64-setup.exe` (`dd52285b…`, 58,819,163 B) silently installed to a wine prefix, ran with the same version, carried the same DLL and uninstalled cleanly.
- **Windows product build (older record):** an earlier snapshot's build is recorded as having succeeded; this document does not rerun builds.
- **Linux product build:** native Ubuntu 24.04 WSL2 ext4 source build with GCC 13.3 and Qt 6.12 completed target `mikai` with exit code 0.
- **Current source snapshot (2026-10-29, revision `9372c03`):** Linux `mikai` builds with GCC on Arch, the hermetic CTest suite passes **55/55** (`ctest --test-dir build/linux-release --output-on-failure`), and the AppImage and dependency-based archive are built locally from that revision. The Windows x64 binary is cross-built with MinGW-w64 GCC 13 (`-DMIKAI_DISABLE_IPO=ON`, `-DMIKAI_BUILD_REVISION=9372c03`) and its payload is assembled from the `dist.cmake` allowlist; `windeployqt.exe` crashes under wine, so it is not used. The installer and portable ZIP are produced by Inno Setup 6.7.3 under wine. Each artifact answers `--version` with `mikai 0.1.0 (9372c03, built …)`.
- **Bundled runtime (2026-10-30, revision `e608684`):** every artifact ships the pinned libmpv runtime — `bin/mpv` in the tarball and AppImage, `usr/lib/mikai/mpv` in the Arch and Debian packages, `mpv\libmpv-2.dll` on Windows. The Linux runtime is the fork rebuilt with an FFmpeg TLS backend by `packaging/mpv/build-tls-archive.sh` (`mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst`, 41,533,948 B, SHA-256 `6796d922…6a7aa4b`); the published archive could not open any https URL, and the rebuilt one decoded an https HLS stream through `MpvHandle` (634.58 s, h264). AnimeJaNai identifies that fork on Unix by its `animejanai` filter and client API 2.6 (the Windows fork is 2.5), which a harness confirmed against both the bundled module and the distribution's mpv. All six artifacts report `mikai 0.1.0 (e608684, …)`:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `mikai-0.1.0-linux-x86_64.tar.gz` | 44,551,342 | `29ff87b9…a9bf75` |
| `mikai-0.1.0-x86_64.AppImage` | 165,071,352 | `aff25842…b2ad7e4` |
| `mikai-0.1.0-1-x86_64.pkg.tar.zst` | 42,593,869 | `68f30014…63bb68` |
| `mikai_0.1.0_amd64.deb` | 65,657,404 | `0f7d2847…37656e` |
| `mikai-0.1.0-windows-x64-portable.zip` | 77,767,864 | `3c9505f0…cebe70` |
| `mikai-0.1.0-windows-x64-setup.exe` | 58,820,789 | `c9b348c3…d2eba2` |

  Verified by running each: the tarball, AppImage and wine-run executable answer `--version` from their own tree or prefix; both packages were installed in Arch and Debian containers, and the Debian start log names the bundled runtime; the Windows installer silently installed, ran the same revision, carried `libmpv-2.dll` `d22a8a76…` and uninstalled cleanly.
- **Distribution packages (2026-10-29, revision `525afc9`; superseded by the bundled-runtime entry above):** `mikai-0.1.0-1-x86_64.pkg.tar.zst` is built by `makepkg` from a git export against Arch's Qt 6.12 and `mpv` (SHA-256 `cd54270dd0e7e9b556a3c99b12e4f1c6b0c89ef6823fad97029fa293f9cde9a8`, 4,604,413 B); it installed, answered `--version` and removed cleanly in an `archlinux` container. `mikai_0.1.0_amd64.deb` is built inside Debian 13 with the Qt 6.12 SDK plus ICU 73 under `/opt/mikai` and the distribution's `libmpv2` (SHA-256 `6f93a280d366b44a61eccceedfa9fe9e390593c098134abb94edf7b6b7b48082`, 25,707,292 B); it installed from a clean `debian:trixie-slim` with `apt-get install ./mikai_0.1.0_amd64.deb`, answered `--version`, started as an unprivileged user with the offscreen platform plugin, and removed cleanly. Both report `mikai 0.1.0 (525afc9, …)`. No playback run, signing or publication approval is implied.
- **What that is not:** these are local builds and a local test result. No GPU inference run, no gallery capture, no signing and no publication gate run is claimed here. Windows installer upgrade/uninstall evidence recorded earlier belongs to the preceding revision and is not restated for `9372c03`.
- **AnimeJaNai inference-context fix (2026-10-30, working tree on top of `e608684`):** the fork's `aji_trt` backend resolves `libnvinfer.so.11` by bare soname, so on Linux the TensorRT closure folder is now preloaded `RTLD_GLOBAL` before the `animejanai` filter attaches (`AnimeJanaiPlan::backendLibDirs`, consumed in `VideoSurfaceAnimeJanai.cpp`). Without it the filter failed with the opaque `Failed to create inference context`; the fork CLI reproduces both states (`libnvinfer.so.11: cannot open shared object file` vs `backend: TensorRT (aji_trt)` initialised and streaming). The session log also falls back to `AppLocalDataLocation` when the executable folder is read-only, so AppImage runs now write a readable `mikai.log`, and AJI failures are logged under `[animejanai]`. Tests: `TestAnimeJanaiRuntime::tensorRtPlanCarriesTheBackendLoaderDir`, `TestDiagnostics::aReadOnlyExeFolderFallsBackToAWritableLogFolder`.

## Playback observation tracking

Two owner-reported playback observations from the 2026-10-11 audit (KB-29 and KB-30) are under continued measurement. They are **not static defects** and are not release blockers for the current scope (Original playback, UI, component acquisition).

| Observation | Status | What remains |
|---|---|---|
| KB-29 — HDR dark-scene instability | Under measurement | Controlled hardware A/B comparison pending; FBO attachment and tone-mapping behavior documented; Stable (`bt.2390` + `clip`) and Vivid (`mobius`) policies implemented; Anime4K/AJE/AJI processing gates in place |
| KB-30 — Periodic frame substitution | Under measurement | Client-side pacing implemented with module-bound clock and version-aware deadline conversion; causal proof pending; controlled uninterrupted aging run needed |

Both observations require physical hardware runs to close. Neither invalidates the current hermetic test suite (**55/55** PASS) or the documented UI/component-acquisition behavior.

## Blocked or not verified

- **Gallery capture:** blocked/pending. No fresh captures are claimed. Historical 2026-10-24 captures under `docs/images/gallery-release-0.1.0/` are prior-run evidence only; see [docs/images/README.md](images/README.md).
- **Release QA:** not passed under the current policy. The hermetic CTest entries, including `LauncherContracts`, `DistContracts`, `NativeStageContracts`, `BundleCliContracts`, `mikai_translation_completeness` and `mikai_provenance_check`, passed on the last recorded Windows run; the publication gate in `packaging/ci/release.py` and the live-network gallery procedure have no green run recorded, so a passing contract suite is not a release approval.
- **Windows installer smoke:** not claimed as passed here; it is handled separately by the Windows installer smoke gate.
- **Windows mpv corresponding-source/license closure:** unresolved. Logs retained for the superseded `19c0fcf895` build establish a GPL-enabled, non-LGPL build; the currently pinned `b4f9f5bfed` build has no retained logs, so its license mode is not established and must not be inferred from the earlier one. Either way public redistribution remains blocked.
- **Linux build/package:** a local build, test run and package for `9372c03` are recorded above; a Linux runtime acceptance pass on target hardware and a distribution review are still not claimed.
- **Public binary distribution:** blocked. No publication, tagging or upload is authorized.

## Intended full gallery deliverable

The full gallery deliverable remains pending and is not represented as complete:

- Five real widget captures: `catalogue.png`, `player.png`, `compare.png`, `media-info.png`, `settings.png`.
- Three supporting appearance captures: `appearance-before.png`, `appearance-preview.png`, `appearance-reset.png`.
- Produced by the opt-in `release_gallery_probe` on an interactive desktop with working desktop OpenGL, an original local video of at least eight seconds, the installed Standard mpv runtime module, and network access to the anonymous catalogue API.

Capture procedure and provenance requirements: [docs/images/README.md](images/README.md).

## Source snapshot documentation

Source packaging remains documented and must be preserved:

- Local source preview: `packaging/source/package.py`
- Source packaging notes: `packaging/source/README.md`
- Provenance records `format` 1, artifact `mikai-0.1.0-source.tar.gz`, version `0.1.0`, `snapshot_kind` `local-worktree-source-snapshot`, `head`, `dirty`, `commit_equivalence` `not-claimed`, `tree_sha256`, and per-file hashes.
- A dirty worktree snapshot is not commit-equivalent and does not authorize publication.

## Downloads

No public download URL is configured in this source checkout. Do not download binaries from an unofficial mirror. Private local package artifacts, if retained, are historical QA outputs only; they are not public downloads and are not approved for publication. See [docs/releasing.md](releasing.md) and [docs/redistribution.md](redistribution.md) for packaging and rights limitations. Historical local artifact identities may be retained in prior evidence records, but they are not current verification or publication approval.