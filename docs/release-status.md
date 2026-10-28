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

- **Windows product build:** recorded as having succeeded for the current source snapshot. This status document does not rerun the build.
- **Linux product build:** native Ubuntu 24.04 WSL2 ext4 source build with GCC 13.3 and Qt 6.12 completed target `mikai` with exit code 0.
- **Current source snapshot (2026-10-29, revision `9372c03`):** Linux `mikai` builds with GCC on Arch, the hermetic CTest suite passes **55/55** (`ctest --test-dir build/linux-release --output-on-failure`), and the AppImage and dependency-based archive are built locally from that revision. The Windows x64 binary is cross-built with MinGW-w64 GCC 13 (`-DMIKAI_DISABLE_IPO=ON`, `-DMIKAI_BUILD_REVISION=9372c03`) and its payload is assembled from the `dist.cmake` allowlist; `windeployqt.exe` crashes under wine, so it is not used. The installer and portable ZIP are produced by Inno Setup 6.7.3 under wine. Each artifact answers `--version` with `mikai 0.1.0 (9372c03, built …)`.
- **What that is not:** these are local builds and a local test result. No GPU inference run, no gallery capture, no signing and no publication gate run is claimed here. Windows installer upgrade/uninstall evidence recorded earlier belongs to the preceding revision and is not restated for `9372c03`.

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