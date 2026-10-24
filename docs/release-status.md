# Release preparation status

The selected delivery is **source-first preparation plus locally tested packages**. The supplied runtimes are retained; public binary publication is on hold until their downstream source/license requirements are fulfilled. The installer is unsigned. No tag, push, upload or public binary release has been performed.

## Local verification

The following checks were exercised on the integrated source, not inferred from generated scripts:

| Check | Observed result | Scope |
| --- | --- | --- |
| Windows build and CTest | 5/5 CTest groups passed | Launcher, distribution, native staging, hermetic C++ suites and real bundle CLI contracts |
| Linux build and CTest | 5/5 CTest groups passed | Launcher, package, native staging, hermetic C++ suites and real bundle CLI contracts |
| Source packaging contracts | 5 passed | Public-file selection, privacy and hashes of actual archived bytes |
| CI/release Python contracts | 11 passed, 1 skipped | Local trusted-tag, ancestry and package/source behavior; Windows symlink creation was unavailable |
| Windows installer | Install, upgrade and uninstall passed | Current-user account; all 132 manifest-listed payload files, Unicode shortcut metadata, data sentinels and monitored integration values |
| Windows OS integration | Registry sandbox suite passed | Real WinAPI operations in GUID-owned HKCU hives; unsafe Default Apps launch is intercepted only in the test executable |
| Windows playback lifecycle | 0/114 requirements failed | Real NVIDIA OpenGL rendering, deferred pause, context recreation, stopped-source and refusal/recovery paths |
| Linux selected-bundle renderer | 0/114 requirements failed | Xvfb/llvmpipe; exact published Standard module and private dependency identities; fractional-DPR instrumentation unavailable |
| Linux runtime recovery | Passed | Exact named published bundle, media clock and nonblack framebuffer; no host-SONAME substitution |
| System-mpv configuration | Configure and product build passed | Separate Linux `MIKAI_LINUX_BUNDLE_MPV=OFF` path; not a hardware-playback claim |
| Real application gallery | Passed | Five core surfaces plus three appearance captures; anonymous live catalogue, Original playback, non-AI comparison and clean worker shutdown |
| Ukrainian catalogue | 1,738 finished, 0 unfinished | Refreshed and compiled with Qt 6.12 |

A 1,200.006-second same-context steady run recorded 30,135 GL compositions, zero black compositions, zero playback stalls, no owner/context/source-epoch changes and a maximum GUI timer gap of 764 ms. Its desktop samples were interrupted by other foreground windows, so this is **renderer/context-aging evidence, not a passing continuous visible-fullscreen certification**. A separate 75-second transition run passed with three advancing PiP-owner samples, zero black desktop/FBO samples and zero stalls. The strict visible-window pilot passed for 35 seconds; longer visible runs failed their desktop validity conditions when Explorer or the terminal host took over. A probe-only topmost experiment also failed validity and was removed. Those failed observations remain recorded, not reclassified as passes. A controlled uninterrupted desktop session is still needed to close that narrower visual-certification gap.

No GPU inference, model-quality result, RIFE throughput or universal codec/driver compatibility is claimed. Linux evidence uses software graphics, not a physical Linux NVIDIA display. Windows integration and installer checks do not certify another user, a fresh Windows account or machine-wide registration.

## Tested local package files

All paths below are local, ignored build outputs. They are not public download links.

| Artifact | Location | SHA-256 |
| --- | --- | --- |
| Windows portable ZIP | `release-artifacts/windows-verified/mikai-0.1.0-windows-x64-portable.zip` | `84bab8fcb09321c3521e19409f6de2531991e2ae4dfdb739a85a6579794b9efa` |
| Windows per-user setup | `release-artifacts/windows-verified/mikai-0.1.0-windows-x64-setup.exe` | `60ba8054321420f2439eefca11784738bd76d1672cac4688828ae6852451b4b3` |
| Linux bundled Qt/mpv archive | `release-artifacts/linux-verified/mikai-0.1.0-linux-x86_64.tar.gz` | `31c594ac5a33376af9c5dac99b3b5d621490f3460f8c9fc04c85212528ad3ab5` |

The Linux archive is dependency-based: host libc/loader, graphics drivers and audio facilities remain required. It is not a universal standalone Linux package. Both platform directories include checksum files; the Windows directory also includes the producer's release JSON. Older review/candidate directories are historical evidence, not the current handoff.

These binaries were produced from the verified worktree. Their producer records are retained as generated; they are not presented as clean tagged builds or bit-for-bit reproducible releases. A preparation commit does not retroactively change their recorded build provenance.

## Source, history and privacy

- Generated `dist-mikai/` files were removed from the unpublished history through a separately verified mirror. All 297 source commits, author/committer identities and message bytes were retained; commit IDs changed. Eleven refs were updated atomically.
- Current source-tree and index contents were preserved. All 649 tracked/nonignored working files at cutover were verified unchanged; actual distribution, native/model data and preferences were not deleted.
- The rewritten reachable history has no generated distribution paths and no blobs exceeding GitHub's 100 MiB limit. Full backups and the commit map are retained outside the repository for recovery.
- Redacted secret scanning reviews the reachable history and the exported public source. One synthetic `X-API-Key` redaction-test fixture has exact history/source fingerprints allowlisted; the rule itself is not disabled. No working `.env` was read or included.
- The source snapshot hashes each member as it is actually archived. Size/read guards are not an atomic filesystem snapshot and do not detect every possible concurrent edit. See [source packaging](../packaging/source/README.md).
- Public gallery evidence is under `docs/images/gallery-release-0.1.0/`. Screenshots are real widget captures; the video is project-generated and contains no third-party footage. The media-information image shows an owned scratch path, not user data; its JSON text redacts that scratch root.

The source archive, source-only secret scan and preparation commit are recorded in the local handoff. The preparation commit does not certify the unresolved uninterrupted-desktop observation. No unverified generated executable, installer, model store or cache is intended for the source commit.

## Evidence locations

- Windows acceptance: `.probe-out/release-acceptance-226b5e67d71e466c94f7b46d1489dfef/` (builds, final CTest log/JUnit, source/CI contracts, redacted history reports, Linux receipts).
- Exact final installer: `.probe-out/installer-smoke-windows-verified-20261024/installer-smoke.json` (installer hash, process exits, payload/sentinel/integration checks).
- GUI gallery: `docs/images/gallery-release-0.1.0/gallery-evidence.json` and its eight PNGs.
- Playback investigation: owned `mikai-playback-acceptance-20261024/evidence` scratch tree outside the repository; failed/invalid-context observations remain distinguishable from passing runs.

## Public binary distribution hold

`profiles.publicReleaseReady` remains **false** for `windows-x64` and `linux-x86_64`. Successful builds, tests and screenshots do not clear the source/license gate.

The matching Windows `mpv-winbuild` run `37965200722` logs show GPL-enabled libmpv and static FFmpeg GPL version 3 or later, not an LGPL-only runtime. The exact deployed Windows library reports mpv `v0.41.0-dev-g19c0fcf89` and FFmpeg `N-127263-g3fe6c8dee`.

Retained materials in `release-artifacts/corresponding-source/` include official Qt 6.12.0 source, pinned mpv source, the runtime-reported FFmpeg base commit `3fe6c8dee248df6732c400bced46d8d0557d3444`, and the primary Windows/Linux build recipes, with hashes and acquisition records. `completeCorrespondingSource` is **false**: matching downstream patches, transitive static/private-library source and license closure, and relevant provider/build fulfillment remain incomplete. Source versions and downloaded base archives are not proof of unmodified binary-to-source correspondence.

Optional native components/models remain excluded from the base packages and retain their own rights restrictions. See [redistribution](redistribution.md), [release procedure](releasing.md) and the component manifests before any public binary release. No publication flag should be cleared merely because local QA passed.
