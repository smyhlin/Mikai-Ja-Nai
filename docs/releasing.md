# Packaging and release

This guide covers local package generation, not publication. There is no configured public repository/download URL in this checkout; do not link users to an invented release page or unofficial binary mirror. Source publication and binary publication are separate decisions. See the [current observed release status](release-status.md) for build outcomes and open gates; the commands and expected outputs below describe packaging paths, not proof that those paths succeeded.

## Release status and redistribution

The package scripts and CI/release workflow are preparation infrastructure, not evidence that an artifact was built, tested or published. Public binary release remains blocked because the current pinned mpv/FFmpeg/dependency source and license closure is incomplete. Local runtime, installer and gallery acceptance are recorded separately in the current release status; they do not establish redistribution clearance. Current Windows DEV input: release tag `2026-10-24-b4f9f5bfed`, run [40115095739](https://github.com/the-database/mpv-winbuild/actions/runs/40115095739), mpv commit `b4f9f5bfed18d5ce610af28024986410dd9bdaae`, asset `mpv-dev-x86_64-20261024-git-b4f9f5bfed.7z` (32,965,168 bytes; SHA-256 `2efe77b9e3028c32cd74c9d871c5d2cd8315758d0da3f59bf64c867c8e7e6d0c`); extracted `libmpv-2.dll` is 122,579,968 bytes (SHA-256 `d22a8a765abe26ac94614b0f09d1d78b3ceafb4f2d70fd904909232e0784c4a3`). Its identity string is `mpv v0.41.0-dev-gb4f9f5bfe`; FFmpeg `N-128018-ga881f0a9a` is the runtime-reported version string, not verified source provenance. Current workflow logs, dependency versions, patch bytes, FFmpeg source and mpv source snapshot are not retained here. Do not infer the DLL's GPL/LGPL mode or claim complete source/license closure. The 19c0fcf895 inputs, run 37965200722, and its logs/source materials documented in the notice are explicitly superseded-build evidence, not evidence for the current pin. See `packaging/third-party.json` and `packaging/licenses/NOTICE.txt`. Qt 6.12.0 source is retained at `release-artifacts/corresponding-source/qt-everywhere-src-6.12.0.tar.xz` (SHA-256 `98ff4f44bac6ec3e1e62ee2a4316ae0e3d15badb015d753cc0268a20db52f165`), but the source manifest and checksums do not establish complete corresponding-source fulfillment. Optional AI/native runtimes and models have separate upstream terms and are excluded from base packages.

Check the repository's [license](../LICENSE), [native third-party notices](../native/manifest/THIRD_PARTY_NOTICES.md), native manifests and, when present, `packaging/licenses/` and `packaging/third-party.json` for current evidence and specific restrictions. A component omitted from a package may still have separate acquisition obligations. The Unlicense for Mikai source does not relicense Qt, mpv, FFmpeg, Anime4K, AnimeJaNai/AJI, models, drivers or any other third-party work. Preserve their notices and required source/provenance when redistributing.

## Windows x64

Use a configured Release MinGW x64 build with Qt 6.12 and Inno Setup 6 available. The installer smoke script requires PowerShell 7.1 or later (`pwsh`) for its .NET 5 hashing APIs and process-tree termination support. From PowerShell 7 at the repository root:

```powershell
cmake -S . -B build/release -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER="C:/path/to/mingw64/bin/g++.exe" `
  -DCMAKE_MAKE_PROGRAM="C:/path/to/mingw64/bin/mingw32-make.exe" `
  -DCMAKE_PREFIX_PATH="C:/path/to/Qt/6.12.x/mingw_64"

powershell -NoProfile -File packaging/windows/package.ps1 `
  -BuildDir build/release -OutputDir release-artifacts
```

If Inno Setup's `ISCC.exe` is not on `PATH`, pass its actual path with `-ISCC`. The script builds `mikai` and stages packages freshly from that build, rather than copying an existing populated `dist-mikai/`. Use a new/empty output directory; do not point it at user data or a populated release directory.

Expected outputs are:

- `mikai-0.1.0-windows-x64-portable.zip`
- `mikai-0.1.0-windows-x64-setup.exe`
- `SHA256SUMS`
- `mikai-0.1.0-windows-x64-release.json`

The installer is unsigned and per-user. It does not force associations or enable startup automatically. Upgrades preserve settings, logs, acquired models and user data; uninstall removes only integrations owned by the installation and does not erase acquired models by default. The portable application requires its deployed Qt/image-format/TLS/MinGW/mpv runtime files and a system Vulkan loader supplied with graphics drivers. Optional AI components are not part of the base portable package.

For an isolated per-user install/upgrade/uninstall check after packaging, invoke `packaging/windows/smoke.ps1` with the setup artifact and a new, absent output directory. Match any result to the exact installer SHA-256 recorded in its evidence; the checked artifacts and account scope are listed in the current release status:

```powershell
pwsh -NoProfile -File packaging/windows/smoke.ps1 `
  -Installer release-artifacts/mikai-0.1.0-windows-x64-setup.exe `
  -OutputDir release-artifacts/installer-smoke
```

The script refuses to proceed if the current user's Mikai uninstall registration exists. It installs under a GUID-owned temporary root at a path containing spaces and Unicode, with a unique Start Menu group; checks executable and shortcut metadata; runs the same installer as an upgrade; then silently uninstalls. It verifies owned model/config/log sentinel bytes survive upgrade and uninstall, and that a fixed allowlist of Mikai HKCU integration values is unchanged. It does not launch Mikai or write real QSettings, enable context-menu/default-player/startup settings, or kill unrelated processes. Installer and uninstaller processes have bounded waits; an error or blocked action fails the gate rather than producing a pass.

`installer-smoke.json` and per-process stdout/stderr logs are the evidence. Registry evidence contains only presence/type and hashes of allowlisted values, not raw values or broad dumps. Failure evidence preserves the original exception and reports cleanup/termination failures; an incompletely uninstalled owned tree is retained with its recovery evidence. This procedure does not establish application runtime health, OS-wide integration behavior, migration of existing user data, or safety beyond those checked values. A disposable Windows account is recommended. Never bypass the existing-installation or occupied-Start-Menu-group guards. A run certifies only the account and artifact actually exercised, not other users or machine-wide behavior.

These outputs are local build products, not approval to upload them. Do not publish the Windows binaries while the mpv dependency/source/license closure described above remains unresolved. Windows builds consume a pinned development archive that supplies `include/mpv/*`, `libmpv.dll.a` and `libmpv-2.dll`. The current DEV archive is release `2026-10-24-b4f9f5bfed` (mpv source commit `b4f9f5bfed18d5ce610af28024986410dd9bdaae`, build run `40115095739`); `mpv-dev-x86_64-20261024-git-b4f9f5bfed.7z` has SHA-256 `2efe77b9e3028c32cd74c9d871c5d2cd8315758d0da3f59bf64c867c8e7e6d0c` and size 32,965,168 bytes. Its extracted `libmpv-2.dll` is 122,579,968 bytes, SHA-256 `d22a8a765abe26ac94614b0f09d1d78b3ceafb4f2d70fd904909232e0784c4a3`. The separate current CLI sibling `mpv-x86_64-20261024-git-b4f9f5bfed.7z` (33,755,796 bytes; SHA-256 `3ba568335b83fa19f2b3c2a63b5264ffd57406e173e0401faebb1ac2f59b3154`) contains `mpv.exe` and resources but no libmpv DLL and is not selected. Current DLL identity string: `mpv v0.41.0-dev-gb4f9f5bfe`; runtime-reported FFmpeg version: `N-128018-ga881f0a9a`. That FFmpeg string is not verified source provenance. The current workflow logs, dependency versions, patch bytes, FFmpeg source and mpv source snapshot are not retained; source/license closure and corresponding-source delivery remain outstanding. Do not assert a GPL/LGPL build mode for the current DLL. The earlier 19c0fcf895 DEV asset, CLI sibling `mpv-x86_64-20261009-git-19c0fcf895.7z` (SHA-256 `85134a3e4748b7ab2e9bdb0a2b445a012082c9afa97edc953f1ccfba191d09b6`) and run 37965200722 materials are superseded-build evidence only (see the notice), not current identities. That earlier CLI archive contains no `libmpv` DLL and is not bundled or executed. A sibling FFmpeg archive `ffmpeg-x86_64-git-3fe6c8dee.7z` (SHA-256 `52ecb132577e770eb33d44fb3c45da325397435250b85805434cf1c29759dc72`) is also not bundled. The pinned archive and cache interface are documented in `cmake/FindOrFetchMpv.cmake`; `MIKAI_DIST_MPV_ARCHIVE` may override the cache copy, but its SHA-256 is always checked. It is a developer binary/header archive, not a corresponding-source release.

## Linux x86_64

The Linux path produces four formats: a dependency-based `tar.gz` with a named install tree, an `AppImage`, an Arch `pkg.tar.zst` and a Debian `.deb`. None is a universally self-contained bundle. Every one of them ships the project's own libmpv runtime; what varies is Qt — the tarball expects a compatible system Qt 6.12+, the Arch package uses the distribution's, and the AppImage and `.deb` deploy it. `libmpv.so.2` (mpv 0.35+ / API 2) is needed at build time for the developer headers but is never taken from the host at runtime in a packaged artifact. Host glibc/loader, desktop OpenGL, graphics drivers, EGL, audio and the codec content statically inside the shipped FFmpeg remain requirements. A specific local build/package observation is recorded in [release status](release-status.md); it does not establish distro-wide compatibility or a libc baseline. CI's Ubuntu 24.04 + separately supplied Qt 6.12 target is likewise not a claim about all distributions.

Optional Qt deployment (`MIKAI_LINUX_DEPLOY_QT=ON`) deploys Qt libraries/plugins and relative RPATH. `MIKAI_LINUX_BUNDLE_MPV=ON` bundles our pinned libmpv fork — the runtime the player and AnimeJaNai are built against — with its private shared-library closure under `MIKAI_LINUX_MPV_INSTALL_SUBDIR` (`bin/mpv`, or `lib/mikai/mpv` in a distribution package), with owned `$ORIGIN` RUNPATH patching via build-time `patchelf`; it does not bundle the mpv CLI, docs or updaters, and a Linux artifact that sets it never depends on the host's mpv. Host glibc/loader, graphics drivers and GL/EGL integration, and audio subsystems remain dependencies. CI and release preparation configure both options on, so the package and renderer smoke use the selected bundled runtime rather than host mpv. This is not a standalone or universal Linux bundle; Qt/mpv redistribution still requires separate rights review. `MIKAI_DIST_MPV_ARCHIVE` is Windows-only and is rejected on Linux.

```bash
cmake --preset linux-release -DMIKAI_LINUX_BUNDLE_MPV=ON
cmake --build --preset app-linux --parallel
bash packaging/linux/package.sh \
  --build-dir build/linux-release \
  --output-dir release-artifacts/linux
```

`-DMIKAI_LINUX_BUNDLE_MPV=ON` is what makes the archive carry the runtime; the preset alone is a developer configuration that selects the host's `libmpv.so.2`. Configure fails when the TLS runtime archive is absent, so a release build cannot quietly produce a network-less player.

The build must be configured and built as Release. The shown Unix Makefiles preset is single-configuration; for a multi-configuration build, pass `--config Release` to the package script. The output directory must not already exist; the package script creates it. CI creates only the output parent and leaves final output creation to the package script; supplying a populated output directory, user data or an existing deployment is refused. The script writes `mikai-0.1.0-linux-x86_64.tar.gz`, `SHA256SUMS`, and a named install tree. The archive extracts to a directory containing `mikai-0.1.0-linux-x86_64/bin/mikai`; run that executable with compatible Qt plugins and desktop OpenGL available. Its libmpv runtime and private closure sit in `bin/mpv`, so no system mpv is required. When using a custom Qt installation, configure `LD_LIBRARY_PATH` for the Qt libraries only during development; packaged runtime discovery uses relative RPATH and does not require a global `LD_LIBRARY_PATH` for the bundled mpv closure.

```bash
cmake --install build/linux-release --prefix /desired/prefix
```

Do not call the tarball self-contained: system Qt/plugin, mpv and graphics-driver dependencies remain in the default profile, and the bundled profile still depends on host glibc, graphics and audio. Optional AI component/runtime support has additional hardware and licensing prerequisites.

The bundled profile's exact configure/build/package recipes are:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.12+/gcc_64 \
  -DCMAKE_CXX_COMPILER=/path/to/Qt/6.12+/gcc_64/bin/g++ \
  -DMIKAI_LINUX_BUNDLE_MPV=ON \
  -DMIKAI_LINUX_DEPLOY_QT=ON \
  -DMIKAI_LINUX_MPV_ARCHIVE=/path/to/mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst
python -m json.tool build/mikai-runtime-Release.json
cmake --build build --parallel 2 --target mikai
bash packaging/linux/package.sh --build-dir build --output-dir /fresh/output --config Release
```

The configure step writes `build/mikai-runtime-Release.json` (`mikai.runtime.bundle/1`) with the selected absolute `module`, private `dependencyRoot`, `bundled` flag and target `platform`. Inspect that generated receipt before building; CI uses the same receipt to stage the configured module and its private `.so` closure for renderer smoke, and package installation deploys that selected runtime. The receipt's absolute paths are build-local and are not portable between machines.

The current Linux runtime is `mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst`, from release tag `2026-10-24-c493f1c` and mpv commit `c493f1c7e10a6b2301653596e7deeb91295d3148`, rebuilt by `packaging/mpv/build-tls-archive.sh`. SHA-256 `6796d922cfd3ffb9bcd8948f77c609a4055ac343a9898f2433d2aa4b0eb6a17d`; size 41,533,948 bytes; 79 files (the published 78, minus the `mpv` CLI, plus `libssl.so.3` and `libcrypto.so.3`). Only the FFmpeg family (same tag `n9.0.2`, same flags plus `--enable-openssl`) and OpenSSL 3.0 differ from the published archive `mpv-linux-x64-2026-10-24-c493f1c.tar.zst` (41,650,081 bytes, SHA-256 `b6281a2c924157a01f4a66f49dda3029b11378ffd982a974cdcbdb474c0befc7`, 78 members), whose `libmpv.so`, `libmpv.so.2` and `libmpv.so.2.6.0` (3,708,024 bytes, SHA-256 `f50c5195aececc060202c2128113d88a9fc0a463c2e5fc9694f8e40349e96768`) are carried unchanged. Neither contains headers or notices. Current workflow logs, dependency versions, patch bytes, FFmpeg source and mpv source snapshot are not retained here; the archive is not corresponding source, and source/license closure and corresponding-source delivery remain outstanding. The 19c0fcf895 Linux archive and its run/recipe evidence are superseded-build material, not the current pin. The bundle requires `patchelf` (build-only dependency), uses extraction stamp `.mikai-linux-mpv.sha256` keyed by the archive SHA, creates an owned regular `libmpv.so.2` because symlink module paths are refused by the runtime trust policy, removes duplicate aliases, runs `patchelf --set-rpath "$ORIGIN"` on every owned `*.so*`, and installs the closure under `bin/mpv`. Generated configuration variables are `MIKAI_LINUX_MPV_MODE`, `MIKAI_LINUX_MPV_BUNDLE`, `MIKAI_LINUX_MPV_ARCHIVE_SHA256` and `MIKAI_LINUX_MPV_PATCHED_RUNPATH`; `MIKAI_LINUX_MPV_VERSION` is set to the bundled marker when bundle mode is enabled. The runtime resolver uses the existing candidate path `<executable>/mpv`, finds `libmpv.so.2`, and does not search system directories when that hint is present. `describeStandardRuntime` refuses symlink module paths and bundle roots containing `bundle.json`; `NativePathTrust` refuses symlink/reparse module paths.

The default system profile remains valid: `MIKAI_LINUX_BUNDLE_MPV=OFF` uses distribution `libmpv-dev` headers and `libmpv.so.2`. Requested bundle failures are fatal configure errors; there is no silent fallback.

### AppImage (single file, local)

An AppImage is a packaging format, not a portability guarantee: it carries Qt and the libmpv runtime, while glibc, the dynamic loader, graphics drivers, EGL and the audio subsystem stay host dependencies. The image inherits the build host's glibc baseline through the bundled Qt, so a build produced on a rolling distribution does not run on an older LTS. The Vulkan loader is bundled with the runtime (1.4.363, the SDK the bundle was built against), so an older host loader cannot downgrade mpv's Vulkan path; the ICD still comes from the installed driver.

The image bundles our own runtime: the pinned libmpv fork plus its private library closure, rebuilt with an FFmpeg that has a TLS backend (`packaging/mpv/build-tls-archive.sh`, documented in the build guide). The archive published at the pinned commit was configured without one, so it could not open any `https` URL; configure detects the backend from libavformat's own dynamic requirements (`MIKAI_LINUX_MPV_TLS`) and AppImage packaging still refuses a runtime without it rather than ship a player that fails on real catalogue playback.

```bash
cmake -S . -B build/appimage -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DMIKAI_LINUX_DEPLOY_QT=ON \
  -DMIKAI_LINUX_BUNDLE_MPV=ON
cmake --build build/appimage --target appimage
```

For a tree that is already built, `bash packaging/linux/appimage.sh --build-dir build/appimage --output-dir /fresh/output` runs the same packaging step.

Requirements: `patchelf` and `appimagetool` on `PATH`, or `-DMIKAI_APPIMAGE_TOOL=/path/to/appimagetool`. Packaging refuses a build configured without Qt deployment, refuses a pinned mpv bundle whose FFmpeg has no TLS backend, and refuses an output directory that already exists. The artifact and its `SHA256SUMS` are written under `build/appimage/_appimage/`.

Qt's deployment omits the QPA platform plugins when the distribution Qt carries no qpa description (as on Arch), so the packaging copies the Qt plugin tree and gives every plugin an `$ORIGIN`-relative RUNPATH. The image deliberately excludes the C library, the loader and the GL/EGL driver libraries: a private libc beside the host loader is not portable, and GL dispatch belongs to the installed driver.

To lower the glibc baseline, run the same commands in an older base image with the provisioned Qt instead of on the current host; no lower-baseline artifact is claimed here.

### Distribution packages (Arch, Debian)

Two distribution formats are produced from the same source tree. Neither is a publication approval, and neither replaces the tarball: the tarball stays the dependency-based reference layout.

**Arch** builds from a git-exported tarball, not from a built tree, so the package and its revision cannot drift apart. It bundles the pinned runtime under `/usr/lib/mikai/mpv` — a directory named `mpv` in `/usr/bin` would collide with the distribution's `mpv` package — and depends on `qt6-base` and `libglvnd` rather than `mpv`:

```bash
bash packaging/linux/arch-package.sh --output-dir /fresh/output
```

The driver refuses a modified worktree and a non-empty output directory, exports `HEAD` with `git archive`, substitutes the tarball digest and revision into `packaging/arch/PKGBUILD`, and runs `makepkg`. The build host needs `qt6-base`, `mpv` and `libglvnd` installed; `check()` runs this repository's CTest suite, so a package is only produced after the tests pass. The result is `mikai-0.1.0-1-x86_64.pkg.tar.zst` plus the generated `.SRCINFO`. The package depends on `qt6-base`, `mpv` and `libglvnd`, installs the executable as `/usr/bin/mikai` (with `/usr/bin/mikai-0.1.0` as its real name and `/usr/bin/shaders` linking to the shader directory), and relies on the distribution's Qt 6.12 — which is why this format only applies to Arch, where Qt 6.12 is current.

**Debian** is built inside Debian 13 (trixie) because the binary's glibc floor is the build base's, and no Debian-family distribution ships Qt 6.12:

```bash
bash packaging/linux/deb-package.sh \
  --output-dir /fresh/output \
  --qt-dir /path/to/Qt/6.12.0/gcc_64
```

The driver builds `packaging/debian/docker/Dockerfile` into `mikai-deb-builder:trixie` (Ubuntu-style builds are not interchangeable: trixie is the oldest base the package is meant for), mounts the checkout read-only, the Qt SDK at `/qt` and the runtime archive at `/mpv/runtime.tar.zst`, and runs `packaging/debian/in-container.sh`. That script configures with `MIKAI_LINUX_DEPLOY_QT=ON` and `MIKAI_LINUX_BUNDLE_MPV=ON`, installs into `/usr` with the runtime at `/usr/lib/mikai/mpv`, derives `Depends` from what each shipped object can actually resolve (the runtime is bundled, so no distribution mpv is required), and builds `mikai_0.1.0_amd64.deb` with `dpkg-deb`.

The Qt 6.12 SDK links ICU 73, which Debian 13 does not provide (it ships ICU 76). ICU 73 is compiled once into the builder image and shipped beside the Qt runtime under `/usr/lib`; every library there resolves its siblings through `$ORIGIN`. The build-time `LD_LIBRARY_PATH` is cleared before packaging and testing so a missing private dependency cannot be masked. The package carries `libc6 (>= 2.38)` from the shipped objects, which excludes Debian 12.

Both scripts verify the package by installing it: the Debian script installs, prints `--version`, starts the application as an unprivileged user with `QT_QPA_PLATFORM=offscreen`, and removes it, failing if the Qt platform plugin cannot load or the removal leaves the executable behind. A clean `debian:trixie-slim` container with only `apt-get install ./mikai_0.1.0_amd64.deb` was used for the recorded artifact, without the builder image's ICU directory.

What neither format establishes: signing, publication, a physical GPU or audio acceptance run, or playback of provider streams. The Arch package inherits Arch's Qt and libmpv, the Debian package inherits trixie's libmpv and carries its own Qt — the two are not interchangeable builds of the same binary.

## Local source preview

To prepare a source archive from the current worktree without staging or committing it, use a fresh output directory:

```bash
python packaging/source/package.py \
  --source-dir . \
  --output-dir /tmp/mikai-source-preview
```

The command writes `mikai-0.1.0-source.tar.gz`, `SHA256SUMS`, and `source-provenance.json`. It packages tracked files present in the worktree plus nonignored untracked files, so local changes and intended new assets are represented; ignored content, build/release outputs, user stores, logs, archives, and secret paths are excluded. Tracked sensitive paths and symlinks fail closed. The output must not exist and must not overlap the source tree. While archiving, each source file is hashed as its bytes are streamed, so provenance describes the bytes actually archived. Short reads, unexpected trailing bytes, changed file sizes and read failures abort packaging. This is not an atomic filesystem snapshot: a same-size edit before a member is read is represented by the newly archived content, not detected as a mutation. Review provenance and archive contents before sharing.

Provenance records `format` 1, `artifact` `mikai-0.1.0-source.tar.gz`, `version` `0.1.0`, `snapshot_kind` `local-worktree-source-snapshot`, `head`, `dirty`, `commit_equivalence` `not-claimed`, `tree_sha256`, and `files[{path, sha256}]`. The tree digest is computed from those archived-byte hashes after archive creation. Selection is tracked files plus nonignored untracked files minus deleted files; exclusions include `.git`, `.omp`, build/release outputs, logs, caches, user stores, native runtime/model data, sensitive paths, symlinks, archives and logs. Production code under `src/player/native/models` is retained. The integrity guarantee is that hashes describe bytes actually archived; size/read checks do not detect every concurrent mutation or provide an atomic filesystem snapshot, and a dirty snapshot is not claimed to equal a commit or tag.

This is explicitly a local preview: provenance records `HEAD`, whether the worktree is dirty, the sorted file hashes, and a deterministic tree digest, but does not claim that a dirty snapshot equals a commit or tag. It does not stage, commit, or tag anything. It is not a substitute for the trusted-tag CI archive, which is assembled from the exact validated tag commit described below, nor does preparing either archive authorize publication.

## CI and publication

The CI workflow handles pushes to `main`, pull requests and manual runs. Its `mikai-windows-x64` and `mikai-linux-x86_64` artifacts contain packages and evidence (including test logs/results and renderer evidence); they are workflow outputs, not permanent download URLs. The release workflow accepts `v*` tag pushes or a manual run for an existing default-branch tag, validates version/ancestry, and produces a `mikai-source-provenance` artifact containing package binaries, `mikai-0.1.0-source.tar.gz`, per-profile verification archives, `provenance.json`, `third-party.json`, `SHA256SUMS` and `RELEASE-NOTES.txt`. Inspect the workflow files and run record for the exact contents and outcome of a particular run. Do not describe a workflow as passing unless its run c…

Manual publication defaults to disabled. Tagging or enabling publication enters the protected `release` environment and must also pass the `profiles.publicReleaseReady` gate. That gate is currently false, so public release is blocked. Before enabling publication, configure required reviewers and version-tag restrictions on the environment and resolve the documented rights blockers. Workflows and scripts are preparation only until an actual run produces evidence; no publication is authorized here.

Before any public release, confirm the version and source revision, inspect package contents for secrets and user state, complete dependency/license/source obligations, retain checksum and provenance files, and record real build/test/package results. No publishing, tagging or upload is authorized by this document.