# Redistribution evidence and source fulfillment

> SHA-256 values in prose are 12-character prefixes; resolve each against the full-digest ledger below. Full digests also live in `corresponding-source/SHA256SUMS`, `corresponding-source/source-materials.json` and `packaging/provenance/*.json` where present — never trust a prefix alone.

This document records the source/artifact evidence required to accompany release binaries. It is not legal advice, a legal-approval statement, or a determination that a particular binary is cleared for redistribution. The platform manifests deliberately keep `publicReleaseReady` false where evidence is incomplete:

- `../packaging/third-party.json` — common rights/readiness record (including Windows and Linux blockers).
- `../packaging/provenance/linux-runtime.json` — observed Linux release tree, pinned input, recipe, hashes, notices, and unresolved mappings.
- `../packaging/licenses/linux/notices.json` — notice files observed in the staged Linux product and their limits.

## Linux profile evidence

| Artifact | Size | SHA-256 pin | Full-digest location |
|---|---:|---|---|
| `release-artifacts/linux-prepush-review/mikai-0.1.0-linux-x86_64.tar.gz` | 74.5 MB (74,538,064 B) | `2b54f328aab6` | Full digest ledger below; artifact checksum sidecar |
| `release-artifacts/corresponding-source/qt-everywhere-src-6.12.0.tar.xz` | 443.4 MB (443,394,020 B) | `98ff4f44bac6` | Full digest ledger below; `corresponding-source/SHA256SUMS` |

- The latest staged product includes the updated Linux legal notices, and its checksum sidecar was written.
- The profile is Ubuntu 24.04 / glibc 2.39; its `VERSION` identifies Release, GNU 13.3.0, Qt 6.12.0 deployed, and bundled mpv.
- `SHA256SUMS` within the tree hashes the staged regular payload files.
- This is build/package identity and inventory evidence, not a runtime-QA result or complete redistribution clearance.

- The staged Qt 6.12.0 runtime consists of deployed Qt shared modules, plugins, translations and ICU 73.2; module attributions and Qt license texts are included under `share/licenses/mikai/third-party/`.
- The official source archive matches the official upstream checksum.
- Its acquisition is recorded in `source-materials.json`, whose `completeCorrespondingSource` field is `false`.
- This retained archive is evidence of that source material only, not a complete corresponding-source claim.
- Exact SDK build inputs, external ICU source/build correspondence, modifications, deployment scripts and component-specific source/relinking fulfillment still require evidence.

### Current Linux mpv input

| Field | Value |
|---|---|
| Release tag | `2026-10-24-c493f1c` |
| Source commit | `c493f1c7e10a` (full commit: full digest ledger below) |
| Shipped archive | `mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst` — 41.5 MB (41,533,948 B), SHA-256 `6796d922cfd3` (full digest: full digest ledger below) |
| Published archive | `mpv-linux-x64-2026-10-24-c493f1c.tar.zst` — 41.7 MB (41,650,081 B), SHA-256 `b6281a2c9241` (full digest: full digest ledger below) |
| Rebuild changes | Built by `packaging/mpv/build-tls-archive.sh`. Only the FFmpeg family is replaced (same tag `n9.0.2`, same configure flags plus `--enable-openssl`) and OpenSSL 3.0 from the Ubuntu 22.04 baseline is added; the mpv fork, libplacebo, shaderc, libass and Vulkan bytes are the published archive's. |
| Evidence limits | Neither archive contains headers or notices, and neither is corresponding source. Current workflow logs, dependency versions, patch bytes, FFmpeg source, mpv source snapshot and `ci/build-linux-portable.sh` at the pinned commit are not retained here. The exact current dependency/source/license closure and corresponding-source delivery remain outstanding; do not treat the superseded 19c0fcf895 recipe or dependency versions below as current evidence. |

### Superseded Linux mpv evidence

- Release 2026-10-09, not current: the former archive `mpv-linux-x64-2026-10-09-19c0fcf.tar.zst` (SHA-256 `55af9ddbd694`; full digest: full digest ledger below) used source commit `19c0fcf89595` (full commit: full digest ledger below).
- The previously documented upstream workflow and `ci/build-linux-portable.sh` belong only to that superseded commit.
- The script selected Ubuntu 22.04 baseline container `ghcr.io/the-database/animejanai-linux-build:ubuntu2204`; Wayland 1.23.1; wayland-protocols 1.41; nv-codec-headers n13.0.19.0; FFmpeg n9.0.2 configured with GPL and version3; shaderc v2024.0; Vulkan SDK 1.3.296.0; libplacebo v7.360.1 with a source edit; a mutable libass `master`; and an mpv meson build enabling libmpv, CUDA, Vulkan and Lua.
- These values and recipe are not evidence for current assets. Do not identify a final object by SONAME alone.

- The project Linux packaging recipe verifies the original archive digest, removes excluded aliases/host libraries, copies only owned shared objects, checks ELF magic and sets each retained private object's RUNPATH to `$ORIGIN` with `patchelf`.
- Final hashes in staged `SHA256SUMS` are postprocessed-object hashes; the archive digest identifies the original bundle.
- `patchelf` version/hash and final-object-to-original-member build-ID/hash crosswalk were not retained.
- Keep original-asset hashes separate from final packaged hashes, and do not substitute a same-SONAME host library as source/build evidence.

- The product does not ship glibc or the ELF loader, and the release recipe excludes host graphics/display/driver-coupled libraries (including GL/EGL/DRM/GBM and CUDA/NVIDIA driver files).
- The host graphics/GPU and session/audio services remain dependencies.
- The build host's glibc 2.39 is recorded but is not a minimum runtime guarantee.
- These distinctions do not establish compatibility with a particular host.

## Windows runtime license evidence

### Current Windows mpv input

| Field | Value |
|---|---|
| DEV asset | `mpv-dev-x86_64-20261024-git-b4f9f5bfed.7z` |
| Release tag | `2026-10-24-b4f9f5bfed` |
| Run link | [40115095739](https://github.com/the-database/mpv-winbuild/actions/runs/40115095739) |
| mpv commit | `b4f9f5bfed18` (full commit: full digest ledger below) |
| DEV asset bytes | 33.0 MB (32,965,168 B) |
| DEV asset SHA-256 | `2efe77b9e302` (full digest: full digest ledger below) |
| Extracted DLL | `libmpv-2.dll` — 122.6 MB (122,579,968 B); SHA-256 `d22a8a765abe` (full digest: full digest ledger below) |
| DLL identity | `mpv v0.41.0-dev-gb4f9f5bfe`; FFmpeg `N-128018-ga881f0a9a` is its runtime-reported version string only, not source verification. |
| Evidence limits | Current workflow logs, dependency versions, patch bytes, FFmpeg source and mpv source snapshot are not retained here. No GPL/LGPL build-mode claim or complete dependency/source/license closure is supported for this current DLL. Source/license closure and corresponding-source delivery remain outstanding. |

### Current CLI sibling

| Artifact | Size | SHA-256 pin | Full-digest location | Selection |
|---|---:|---|---|---|
| `mpv-x86_64-20261024-git-b4f9f5bfed.7z` | 33.8 MB (33,755,796 B) | `3ba568335b83` | Full digest ledger below; `corresponding-source/SHA256SUMS` | Contains mpv.exe/media/resources but no libmpv DLL; not selected. |

### Superseded Windows evidence

- Release 2026-10-09, not current: the earlier Windows mpv-winbuild run 37965200722 and the 19c0fcf895 DEV asset are not evidence for the current pin.
- All subsequent configuration, FFmpeg, dependency and source-material claims in this subsection refer only to that superseded build.
- The matching `mpv-winbuild` run `37965200722` configuration logs, retained under `.probe-out/release-tools/windows-run-37965200722-logs` and `.probe-out/release-tools/mpv19c0-source-logs`, identify the selected `19c0fcf895` libmpv build as GPL-enabled, not the LGPL variant. Its static FFmpeg configuration reports GPL version 3 or later.
- A startup/version probe of the exact deployed DLL (SHA-256 `f7913c65750a`; full digest: full digest ledger below) reports FFmpeg `N-127263-g3fe6c8dee`; GitHub's primary commit API resolves this to `3fe6c8dee248` (full commit: full digest ledger below).
- The matching FFmpeg source archive and upstream license files are retained under `release-artifacts/corresponding-source/` (SHA-256 `29a6a1e99859`; full digest: full digest ledger below).
- This establishes the runtime-reported FFmpeg revision, not absence of downstream patches or the full static dependency/license/source closure.
- The selected runtime must not be presented as LGPL-only.
- `packaging/provenance/windows-runtime.json` and the shared rights manifest retain this evidence; release readiness remains false.

## Source acquisition since the prior review (superseded mpv build materials)

- The mpv, FFmpeg, Windows workflow and Linux recipe materials enumerated below relate to the superseded 19c0fcf895 / run 37965200722 build only.
- They do not correspond to the current Windows or Linux asset pins.
- Current Windows/Linux workflow logs, dependency versions, patch bytes, FFmpeg source and mpv source snapshots are not retained here; current source/license closure and corresponding-source delivery remain outstanding.

The following primary-upstream materials have been retained under `release-artifacts/corresponding-source/`; mpv-related entries are superseded-build materials only:

| Material | Status | Evidence |
|---|---|---|
| Official Qt 6.12.0 source archive | Retained | Remains retained and matches Qt's published SHA-256. |
| GitHub codeload source snapshot of `the-database/mpv` at commit `19c0fcf89595`, with upstream copyright/license files in the source tree | Superseded | SHA-256 `8b09edc8e410` (full digest: full digest ledger below); not a current pinned source snapshot. |
| Linux portable build recipe from that commit, including its inline libplacebo source edit, dependency selection, and collection procedure | Superseded | SHA-256 `f7c36af3bd1e` (full digest: full digest ledger below); not the current Linux recipe. |
| `mpv-winbuild` workflow YAML at run head `597bab33931c` | Superseded | SHA-256 `cd90a2562022` (full digest: full digest ledger below); documents only the superseded run recipe entry point, not the current build. |
| FFmpeg source archive at commit `3fe6c8dee248`, including upstream licensing files | Superseded | SHA-256 `29a6a1e99859` (full digest: full digest ledger below); relates only to the superseded binary's runtime-reported version, not the current FFmpeg string. |

- The files and digests are listed in `corresponding-source/source-materials.json` and `corresponding-source/SHA256SUMS`.
- The mpv snapshot is source for the superseded 19c0fcf895 repository commit only, not either current pin; it does not include separate dependency trees/submodules or establish that superseded binaries were built from that exact snapshot and dependency closure.
- The superseded Linux recipe has a mutable libass `master` input, unpinned submodule/dependency state, and uses a mutable image tag; the superseded Windows run's source cache and matching source trees are not retained.
- These materials do not establish corresponding-source completion or a valid source offer for the current binaries.

- The exact Linux inline libplacebo edit described here belongs only to the superseded 19c0fcf895 recipe (the replacement of `pool->idx_queues` with `0` in `src/vulkan/swapchain.c`); it is not evidence of a patch in the current build.
- The exact source tree on which that superseded edit ran is not retained.
- Do not label that recipe line as a standalone, matching patch artifact or infer an original-to-patched binary member mapping from the recipe.
- For Linux final files modified by `patchelf`, only identify a source member when an actual archive-member-to-final-file mapping is recorded; SONAME similarity is insufficient.

## LGPL replacement and delivery scope

- The LGPL-3.0 text requires corresponding source for the LGPL-covered library, including modifications and the scripts needed to control compilation and installation, but it does not make exact toolchain/container image digests, byte-for-byte reproducibility, or reproducible-build certification a general legal prerequisite. Those details can be valuable technical evidence and may be necessary to explain or reproduce a particular build; they must not be conflated with the LGPL's source and replacement conditions.
- For an LGPL-covered library used through a suitable shared-library mechanism, the distributor must allow replacement with a modified compatible library and not impose restrictions that prevent exercising that right; the applicable LGPL terms also address reverse engineering for debugging modifications.
- Installation information is relevant when the work is conveyed in or for a User Product within GPLv3's definition and the conditions there apply; it is not a blanket requirement for every ordinary desktop distribution.
- Assess the actual product/distribution and the selected LGPL version rather than asserting either automatic applicability or automatic exemption. In particular, the shipped Windows Qt DLLs and Linux Qt shared libraries are dynamically loaded, but package evidence alone does not establish all replacement, source, notice, or installation-information conditions.

- A practical distributor-controlled route for a future release is a versioned companion corresponding-source archive offered alongside each exact binary release, with immutable digests and a retained copy for the required term.
- That archive can include the exact source, modifications, applicable license/notice texts, and required scripts; a release URL must remain under distributor control and actually serve the bundle.
- The retained Qt and mpv files are not yet that complete bundle, and nothing here claims a source offer has been published.
- Do not use external upstream links or binary SDKs as substitutes for a distributor-controlled offer.

## Combined practical source fulfillment

Prepare one release-specific source bundle for the exact Windows and Linux artifacts. Keep each binary's hash and package path associated with the corresponding component record; keep both source trees separate where they differ. Include, as applicable:

1. Exact corresponding upstream sources (including generated or vendored sources and submodules), version/revision, and the source archive's own digest.
2. Every patch, modification, configuration option, compiler/linker command, dependency source and build/install script used to produce or postprocess the shipped binary. Include the Qt deployment steps and the Linux `$ORIGIN` RUNPATH/patchelf step.
3. The applicable upstream copyright and license notices for every shipped component, preserved in the product and the fulfillment bundle. For Qt, map every deployed module/plugin to its own source package, attributions and license election; do not apply one blanket Qt license label.
4. The complete corresponding source required by each applicable license, including modifications and scripts needed to control compilation and installation where required. Build instructions/toolchain details help establish technical provenance and support rebuilding, but exact toolchain/container digests and bit-for-bit reproducibility are not blanket legal prerequisites. For libraries subject to relinking/replacement terms, demonstrate the actual permitted replacement mechanism and assess whether installation information applies to the conveyed product.
5. A source delivery path that remains available for the distribution's required period and is bound to the specific binary release by immutable digests. Check source-availability obligations and delivery terms against the actual selected license text; do not treat a general link to upstream or a binary SDK as corresponding source.

- The retained Qt and mpv source archives, recipe files, and final payload inventories do not complete the corresponding-source bundle for either platform.
- Windows still needs the exact linked FFmpeg/dependency sources, patches, linked license closure, and matching Qt/MinGW source/build and replacement fulfillment.
- Linux still needs the exact private-library source trees and per-component notices, matching asset-to-source evidence for its mutable dependency inputs, Qt/ICU source/build correspondence, and the final-to-original member crosswalk.
- Do not clear either platform's readiness blocker until the specific artifact and fulfillment evidence is retained and reviewed.

- The distributor-controlled mechanism identified here is a release-specific companion source archive attached to the same versioned release as each binary package, with an immutable digest and retention under distributor control.
- It is supported by actual retained Qt/mpv source and recipe materials, but cannot yet contain the complete corresponding source and notices for the supplied runtimes, so it is not an active compliant source offer.
- The conservative alternative is to withhold these binary packages until the missing runtime source/license materials are recovered or the runtime is rebuilt with a complete, controlled source and notice closure while preserving the intended portable playback feature set.
- Do not silently substitute host libraries, disable bundled playback, or claim readiness.

## Full digest ledger

Full digests and commit IDs corresponding to the pins above. SHA-256 values are also in `corresponding-source/SHA256SUMS` and the applicable provenance/source-material records.

| Artifact / identifier | Full SHA-256 digest or commit ID |
|---|---|
| Linux staged product archive | `2b54f328aab67c9cb3a710314bfe3dcb754948db91702861d5becfb66ad85eca` |
| Qt 6.12.0 source archive | `98ff4f44bac6ec3e1e62ee2a4316ae0e3d15badb015d753cc0268a20db52f165` |
| Current Linux mpv source commit | `c493f1c7e10a6b2301653596e7deeb91295d3148` |
| Current Linux shipped archive | `6796d922cfd3ffb9bcd8948f77c609a4055ac343a9898f2433d2aa4b0eb6a17d` |
| Current Linux published archive | `b6281a2c924157a01f4a66f49dda3029b11378ffd982a974cdcbdb474c0befc7` |
| Superseded Linux archive | `55af9ddbd694366ba016b9e421a6abae78f8559887b7591f7820eca8d5b2bf37` |
| Superseded Linux mpv source commit | `19c0fcf89595e6ab33908299207c555b67e81558` |
| Current Windows DEV asset mpv commit | `b4f9f5bfed18d5ce610af28024986410dd9bdaae` |
| Current Windows DEV asset archive | `2efe77b9e3028c32cd74c9d871c5d2cd8315758d0da3f59bf64c867c8e7e6d0c` |
| Current Windows extracted `libmpv-2.dll` | `d22a8a765abe26ac94614b0f09d1d78b3ceafb4f2d70fd904909232e0784c4a3` |
| Current Windows CLI sibling archive | `3ba568335b83fa19f2b3c2a63b5264ffd57406e173e0401faebb1ac2f59b3154` |
| Superseded Windows DLL | `f7913c65750a53365a67ac0296baab5f8dcc1d406c697fa9249e83ed8391e090` |
| Superseded FFmpeg source commit | `3fe6c8dee248df6732c400bced46d8d0557d3444` |
| Superseded FFmpeg source archive | `29a6a1e9985937f9c5b48e04784fb223eec5872f71fc6feae2c97403821b75cb` |
| Superseded mpv source snapshot | `8b09edc8e4107595c2ed27ae57d30a18e65d19f5c64910856633f0f992bf1ecf` |
| Superseded Linux build recipe | `f7c36af3bd1e02fb1e0a027cd758a132cbad0baed35ac01a552e44831226067a` |
| Superseded mpv-winbuild workflow head | `597bab33931cbd2ec29abe9d44a29e6363da7499` |
| Superseded mpv-winbuild workflow YAML | `cd90a256202223e5eaeb176a541fda24f860ed65414cff7199bc0e5d5973b2f9` |
