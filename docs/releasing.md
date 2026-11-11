# Packaging and release

> Dependency SHA-256 values here are 12-char prefixes unless shown in a table; full input digests live in the native manifests, packaging provenance and notices. Product artifact checksums are published with each GitHub release and generated alongside local packages; do not copy them into documentation. A rebuild alone does not require a documentation commit.

This guide covers local package generation, not publication.

- Existing public releases include [v0.2.0](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.2.0), the [player and usability update](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0-update-20261101), and the original [v0.1.0](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0); do not link users to an unofficial binary mirror.
- Source publication and binary publication are separate decisions.
- See the [current observed release status](release-status.md) for build outcomes and open gates; the commands and expected outputs below describe packaging paths, not proof that those paths succeeded.

## Release status and redistribution

- Packaging and CI workflows do not by themselves establish that an artifact was built, tested or published. See [release status](release-status.md) for recorded results and [GitHub releases](https://github.com/smyhlin/Mikai-Ja-Nai/releases) for published packages.
- Further public binary distribution remains blocked pending completion of the pinned mpv/FFmpeg/dependency source and license closure and outstanding release QA. Existing publications and local runtime, installer or gallery checks do not establish redistribution clearance.

Current Windows DEV input:

| Field | Value |
|---|---|
| Release tag | `2026-10-24-b4f9f5bfed` |
| Build run | [40115095739](https://github.com/the-database/mpv-winbuild/actions/runs/40115095739) |
| mpv commit | `b4f9f5bfed18` (full: `packaging/provenance/windows-runtime.json`) |
| Asset | `mpv-dev-x86_64-20261024-git-b4f9f5bfed.7z` (33.0 MB (32,965,168 B); SHA-256 `2efe77b9e302`; full: `native/manifest/components.json`, `native/manifest/sources.json`, `packaging/provenance/windows-runtime.json`, `packaging/third-party.json`, `packaging/licenses/NOTICE.txt`) |
| Extracted DLL | `libmpv-2.dll` (122.6 MB (122,579,968 B); SHA-256 `d22a8a765abe`; full: same locations as the asset) |
| DLL identity | `mpv v0.41.0-dev-gb4f9f5bfe` |
| FFmpeg (runtime-reported) | `N-128018-ga881f0a9a` — not verified source provenance |

Caveats:

- Current workflow logs, dependency versions, patch bytes, FFmpeg source, and mpv source snapshot are not retained here.
- Do not infer the DLL's GPL/LGPL mode or claim complete source/license closure.
- The 19c0fcf895 inputs, run 37965200722, and their logs/source materials are superseded-build evidence only (see `packaging/licenses/NOTICE.txt`), not evidence for the current pin.
- `packaging/provenance/windows-runtime.json` records the official Qt 6.12.0 source archive as retained at `release-artifacts/corresponding-source/qt-everywhere-src-6.12.0.tar.xz` (SHA-256 `98ff4f44bac6`; full digest also in `packaging/provenance/linux-runtime.json`). `release-artifacts/` is ignored and not part of a checkout, so a fresh clone does not carry that file; the source manifest and checksum do not establish complete corresponding-source fulfillment either way.
- Optional AI/native runtimes and models have separate upstream terms and are excluded from base packages.
- See `packaging/third-party.json` and `packaging/licenses/NOTICE.txt`.

- Check the repository's [license](../LICENSE), [native third-party notices](../native/manifest/THIRD_PARTY_NOTICES.md), native manifests and, when present, `packaging/licenses/` and `packaging/third-party.json` for current evidence and specific restrictions.
- A component omitted from a package may still have separate acquisition obligations.
- The Unlicense for Mikai source does not relicense Qt, mpv, FFmpeg, Anime4K, AnimeJaNai/AJI, models, drivers or any other third-party work.
- Preserve their notices and required source/provenance when redistributing.

### Historical (superseded) inputs

Full digests for inputs that do not live in any provenance JSON or `SHA256SUMS` are retained here:

| Input | SHA-256 (full) | Status / location |
|---|---|---|
| Earlier DEV asset + CLI sibling `mpv-x86_64-20261009-git-19c0fcf895.7z` (run 37965200722) | `85134a3e4748b7ab2e9bdb0a2b445a012082c9afa97edc953f1ccfba191d09b6` | superseded-build evidence only; also `packaging/licenses/NOTICE.txt` |
| Sibling FFmpeg archive `ffmpeg-x86_64-git-3fe6c8dee.7z` | `52ecb132577e770eb33d44fb3c45da325397435250b85805434cf1c29759dc72` | not bundled; also `packaging/licenses/NOTICE.txt` |

## Canonical product artifacts

For the published `0.2.0`, the canonical release comprised six product packages, the Windows build receipt, the Arch `.SRCINFO`, and one `SHA256SUMS`:

| Platform | Product artifact | Written by |
|---|---|---|
| Windows x64 | `mikai-0.2.0-windows-x64-portable.zip` | `packaging/windows/package.ps1` |
| Windows x64 | `mikai-0.2.0-windows-x64-setup.exe` | `packaging/windows/package.ps1` |
| Windows x64 | `mikai-0.2.0-windows-x64-release.json` (build receipt) | `packaging/windows/package.ps1` |
| Linux x86_64 | `mikai-0.2.0-linux-x86_64.tar.gz` | `packaging/linux/package.sh` |
| Linux x86_64 | `mikai-0.2.0-x86_64.AppImage` | `appimage` target / `packaging/linux/appimage.sh` |
| Linux x86_64 | `mikai-0.2.0-1-x86_64.pkg.tar.zst` (plus `mikai-0.2.0-1.SRCINFO`) | `packaging/linux/arch-package.sh` |
| Linux x86_64 | `mikai_0.2.0_amd64.deb` | `packaging/linux/deb-package.sh` |
| both | `SHA256SUMS` | regenerated at the top level over the promoted set |

- Each script writes its own `SHA256SUMS` beside its output; promoting several builds into one directory means regenerating the single top-level manifest so it covers every product file and the receipt.
- The `mikai_` (underscore) Debian name, the `-1-` Arch package release and the `AppImage`/`pkg.tar.zst` suffixes come from the respective packaging scripts; all of them derive the version from `project(mikai VERSION ...)` in `CMakeLists.txt`.
- A rebuild keeps the same file names even when the embedded revision differs: the receipt, the `--version` line and the checksums distinguish contents, not the file name.
- Dated, `-review` and `previous-top-level-<timestamp>` subdirectories are staging and backup locations only; nothing consumes them automatically.
- The trusted-tag release assembly (`packaging/ci/release.py`) copies a narrower set: `mikai-<version>-windows-x64-portable.zip`, `mikai-<version>-windows-x64-setup.exe`, `mikai-<version>-linux-x86_64.tar.gz`, the two `mikai-<version>-<profile>-verification.zip` evidence archives, `mikai-<version>-source.tar.gz`, `third-party.json`, `provenance.json`, `RELEASE-NOTES.txt` and `SHA256SUMS`. AppImage, Arch and Debian packages are not members of that CI bundle, although the existing player and usability update published those formats separately.
- Generating any of them does not authorize redistribution; see [Release status and redistribution](#release-status-and-redistribution).

## Windows x64

Use a configured Release MinGW x64 build with Qt 6.12 and Inno Setup 6.3 or later (`package.ps1` also checks the standard `Inno Setup 6` install locations before giving up). The installer smoke script requires PowerShell 7.1 or later (`pwsh`) for its .NET 5 hashing APIs and process-tree termination support. From PowerShell 7 at the repository root:

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

Without `-RequirePublicReady` the script warns and prepares **private review** artifacts. With `-RequirePublicReady` it refuses to run at all while `packaging/third-party.json` `profiles.windows-x64.publicReleaseReady` is `false`, which it currently is for both platform profiles.

Building into a dated or review subdirectory (for example `release-artifacts/windows-runtime-review/`) does **not** refresh existing top-level files in `release-artifacts/`. The canonical delivery paths are the top-level names listed under [Canonical product artifacts](#canonical-product-artifacts). To promote a reviewed build there, back up the previous product files (a `previous-top-level-<timestamp>/` directory is the recorded precedent), copy the verified packages and receipt to the top level, then regenerate the top-level `SHA256SUMS` over the complete product set, the receipt included. Preserve pinned runtime archives, `corresponding-source/` inputs and unrelated output directories.

Verify the executable inside the canonical portable ZIP with `--version`, not only the executable in the build directory. Rebuilds can share the `0.2.0` product number; the embedded source revision and package checksums distinguish their actual contents.

`MIKAI_BUILD_REVISION` is resolved when CMake configures, and the packaging script only builds — it does not reconfigure. A commit made after the last `cmake --preset …` therefore leaves the binary stamped with the previous revision while the `VERSION` file the dist step writes names the current one. Reconfigure before packaging and confirm that the packaged binary's `--version` matches the receipt's committed revision; a mismatch is a stale build, not a receipt error. A cross build that cannot read the checkout states the revision with `-DMIKAI_BUILD_REVISION=<short-rev>` instead.

Expected outputs are:

- `mikai-0.2.0-windows-x64-portable.zip`
- `mikai-0.2.0-windows-x64-setup.exe`
- `SHA256SUMS`
- `mikai-0.2.0-windows-x64-release.json`

- The installer is unsigned and per-user. It does not force associations or enable startup automatically.
- Setup shows its Welcome page and checks this Windows user's registered installation. An older version is offered as **Update**; a matching version is offered for update or repair because different builds can share the same version number. A newer registered version is refused rather than silently replaced.
- Portable and manually copied installations have no installer registration and are not automatically detected. Choose the intended folder explicitly if updating one.
- Upgrades and repairs preserve settings, logs, acquired models and user data; uninstall removes only integrations owned by the installation and does not erase acquired models by default.
- The portable application requires its deployed Qt/image-format/TLS/MinGW/mpv runtime files and a system Vulkan loader supplied with graphics drivers.
- Optional AI components are not part of the base portable package.

For an isolated per-user install/upgrade/uninstall check after packaging, invoke `packaging/windows/smoke.ps1` with the setup artifact and a new, absent output directory. Match the result to the exact installer SHA-256 recorded in its evidence:

```powershell
pwsh -NoProfile -File packaging/windows/smoke.ps1 `
  -Installer release-artifacts/mikai-0.2.0-windows-x64-setup.exe `
  -OutputDir release-artifacts/installer-smoke
```

- The script refuses to proceed if the current user's Mikai uninstall registration exists.
- It installs under a GUID-owned temporary root at a path containing spaces and Unicode, with a unique Start Menu group; checks executable and shortcut metadata; runs the same installer as an upgrade; then silently uninstalls.
- It verifies owned model/config/log sentinel bytes survive upgrade and uninstall, and that a fixed allowlist of Mikai HKCU integration values is unchanged.
- It does not launch Mikai or write real QSettings, enable context-menu/default-player/startup settings, or kill unrelated processes.
- Installer and uninstaller processes have bounded waits; an error or blocked action fails the gate rather than producing a pass.

- `installer-smoke.json` and per-process stdout/stderr logs are the evidence.
- Registry evidence contains only presence/type and hashes of allowlisted values, not raw values or broad dumps.
- Failure evidence preserves the original exception and reports cleanup/termination failures; an incompletely uninstalled owned tree is retained with its recovery evidence.
- This procedure does not establish application runtime health, OS-wide integration behavior, migration of existing user data, or safety beyond those checked values.
- A disposable Windows account is recommended. Never bypass the existing-installation or occupied-Start-Menu-group guards.
- A run certifies only the account and artifact actually exercised, not other users or machine-wide behavior.

- The payload is assembled on Windows only: `package.ps1` reads the generated `build/mikai-package-Release.json` (version, configuration, executable, source directory), refuses a build that does not belong to this checkout/Release configuration, then runs `build/mikai-package-Release.cmake`. That generated script includes `cmake/dist.cmake` with `MIKAI_PUBLIC_RELEASE` set, so the allowlisted Qt plugins, the PE import closure, the MinGW runtime, the pinned `libmpv-2.dll`, benchmark clips, translations, licences and `qt.conf` are validated rather than assumed.
- `package.ps1` then writes an explicit `payload-files.iss` inventory from the assembled tree and compiles `packaging/windows/mikai.iss` with `AppVersion`, `StageDir`, `PayloadFiles` and `OutputDir`, so the installer can only ship files that were actually assembled. No recursive source glob and no AI payload can enter it.
- `packaging/windows/assemble-payload.py` is exercised only by `packaging/ci/test_windows_runtime.py` as a runtime-contract check; it is not the packaging path.

These outputs are local build products, not approval to upload them. Do not publish the Windows binaries while the mpv dependency/source/license closure described above remains unresolved.

The Windows DEV archive supplies headers, an import library and `libmpv-2.dll`; it is not corresponding source. Current input identities and unresolved source/license closure are documented in [Building](building.md#platforms-and-prerequisites) and [Redistribution](redistribution.md#windows-runtime-license-evidence). `MIKAI_DIST_MPV_ARCHIVE` may override the cache copy, but its pinned SHA-256 is always checked.

## Linux x86_64

- The Linux path produces four formats: a dependency-based `tar.gz` with a named install tree, an `AppImage`, an Arch `pkg.tar.zst` and a Debian `.deb`.
- None is a universally self-contained bundle.
- In the release configuration used here every one of them ships the project's own libmpv runtime, because the packaging steps are configured with `MIKAI_LINUX_BUNDLE_MPV=ON`; what varies is Qt — the tarball expects a compatible system Qt 6.12+, the Arch package uses the distribution's, and the AppImage and `.deb` deploy it.
- `libmpv.so.2` (mpv 0.35+ / API 2) is always needed at build time for the developer headers. A build configured with `MIKAI_LINUX_BUNDLE_MPV=OFF` stages no runtime, so its tarball or `dist` tree resolves the host's `libmpv.so.2` at runtime; that profile is a developer package, not a release package.
- Host glibc/loader, desktop OpenGL, graphics drivers, EGL, audio and the codec content statically inside the shipped FFmpeg remain requirements.
- A specific local build/package observation is recorded in [release status](release-status.md); it does not establish distro-wide compatibility or a libc baseline.
- CI's Ubuntu 24.04 + separately supplied Qt 6.12 target is likewise not a claim about all distributions.

- Optional Qt deployment (`MIKAI_LINUX_DEPLOY_QT=ON`) deploys Qt libraries/plugins and relative RPATH.
- `MIKAI_LINUX_BUNDLE_MPV=ON` bundles our pinned libmpv fork — the runtime the player and AnimeJaNai are built against — with its private shared-library closure under `MIKAI_LINUX_MPV_INSTALL_SUBDIR` (`bin/mpv`, or `lib/mikai/mpv` in a distribution package), with owned `$ORIGIN` RUNPATH patching via build-time `patchelf`; it does not bundle the mpv CLI, docs or updaters, and a Linux artifact that sets it never depends on the host's mpv.
- Host glibc/loader, graphics drivers and GL/EGL integration, and audio subsystems remain dependencies.
- CI and release preparation configure both options on, so the package and renderer smoke use the selected bundled runtime rather than host mpv.
- This is not a standalone or universal Linux bundle; Qt/mpv redistribution still requires separate rights review.
- `MIKAI_DIST_MPV_ARCHIVE` is Windows-only and is rejected on Linux.

```bash
cmake --preset linux-release -DMIKAI_LINUX_BUNDLE_MPV=ON
cmake --build --preset app-linux --parallel
bash packaging/linux/package.sh \
  --build-dir build/linux-release \
  --output-dir /fresh/output
```

`-DMIKAI_LINUX_BUNDLE_MPV=ON` is what makes the archive carry the runtime; the preset alone is a developer configuration that selects the host's `libmpv.so.2`. Configure fails when the TLS runtime archive is absent, so a release build cannot quietly produce a network-less player.

- The build must be configured and built as Release.
- The shown Unix Makefiles preset is single-configuration; for a multi-configuration build, pass `--config Release` to the package script.
- The output directory must not already exist; the package script creates it. CI creates only the output parent and leaves final output creation to the package script; supplying a populated output directory, user data or an existing deployment is refused.
- The script writes `mikai-0.2.0-linux-x86_64.tar.gz`, `SHA256SUMS`, and a named install tree.
- The archive extracts to a directory containing `mikai-0.2.0-linux-x86_64/bin/mikai`; run that executable with compatible Qt plugins and desktop OpenGL available.
- Its libmpv runtime and private closure sit in `bin/mpv`, so no system mpv is required.
- When using a custom Qt installation, configure `LD_LIBRARY_PATH` for the Qt libraries only during development; packaged runtime discovery uses relative RPATH and does not require a global `LD_LIBRARY_PATH` for the bundled mpv closure.

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

- The configure step writes `build/mikai-runtime-Release.json` (`mikai.runtime.bundle/1`) with the selected absolute `module`, private `dependencyRoot`, `bundled` flag and target `platform`.
- Inspect that generated receipt before building; CI uses the same receipt to stage the configured module and its private `.so` closure for renderer smoke, and package installation deploys that selected runtime.
- The receipt's absolute paths are build-local and are not portable between machines.

The pinned Linux runtime and its TLS rebuild are described in [Building](building.md#platforms-and-prerequisites) and [Redistribution](redistribution.md#current-linux-mpv-input). The runtime archive is not corresponding source; dependency/source/license closure remains outstanding.
- The bundle requires `patchelf` (build-only dependency), uses extraction stamp `.mikai-linux-mpv.sha256` keyed by the archive SHA, creates an owned regular `libmpv.so.2` because symlink module paths are refused by the runtime trust policy, removes duplicate aliases, runs `patchelf --set-rpath "$ORIGIN"` on every owned `*.so*`, and installs the closure under `bin/mpv`.
- `build/mikai-linux-release.cmake` (from `packaging/linux/release-config.cmake.in`) carries what the staging/package scripts read: `MIKAI_LINUX_MPV_BUNDLE_MODE`, `MIKAI_LINUX_MPV_BUNDLE`, `MIKAI_LINUX_MPV_TLS`, `MIKAI_LINUX_MPV_RUNTIME_IDENTITY`, `MIKAI_LINUX_MPV_MODULE_RELATIVE`, `MIKAI_LINUX_MPV_INSTALL_SUBDIR`, `MIKAI_LINUX_MPV_ARCHIVE_SHA256`, `MIKAI_LINUX_MPV_PATCHED_RUNPATH`, the build type, compiler and Qt version, and the three feature switches the package-time refusals test.
- The runtime resolver uses the existing candidate path `<executable>/mpv`, finds `libmpv.so.2`, and does not search system directories when that hint is present.
- `describeStandardRuntime` refuses symlink module paths and bundle roots containing `bundle.json`; `NativePathTrust` refuses symlink/reparse module paths.

The default system profile remains valid: `MIKAI_LINUX_BUNDLE_MPV=OFF` uses distribution `libmpv-dev` headers and `libmpv.so.2`. Requested bundle failures are fatal configure errors; there is no silent fallback.

### AppImage (single file, local)

- An AppImage is a packaging format, not a portability guarantee: a release image carries Qt and, with `MIKAI_LINUX_BUNDLE_MPV=ON`, the libmpv runtime, while glibc, the dynamic loader, graphics drivers, EGL and the audio subsystem stay host dependencies.
- The image inherits the build host's glibc baseline through the bundled Qt, so a build produced on a rolling distribution does not run on an older LTS.
- The Vulkan loader is bundled with the runtime (1.4.363, the SDK the bundle was built against), so an older host loader cannot downgrade mpv's Vulkan path; the ICD still comes from the installed driver.

- The image bundles our own runtime: the pinned libmpv fork plus its private library closure, rebuilt with an FFmpeg that has a TLS backend (`packaging/mpv/build-tls-archive.sh`, documented in the build guide).
- The archive published at the pinned commit was configured without one, so it could not open any `https` URL.
- Configure detects the backend from libavformat's own dynamic requirements (`MIKAI_LINUX_MPV_TLS`), and AppImage packaging still refuses a runtime without it rather than ship a player that fails on real catalogue playback.
- Built without `MIKAI_LINUX_BUNDLE_MPV=ON`, the packaging step only warns: the image then carries Qt but requires the host's `libmpv.so.2` (client API 2, built with TLS). That variant still refuses optional native/model-inspection builds, and it is described as host-dependent rather than standalone.

```bash
cmake -S . -B build/appimage -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DMIKAI_LINUX_DEPLOY_QT=ON \
  -DMIKAI_LINUX_BUNDLE_MPV=ON
cmake --build build/appimage --target appimage
```

For a tree that is already built, `bash packaging/linux/appimage.sh --build-dir build/appimage --output-dir /fresh/output` runs the same packaging step.

Requirements: `patchelf` and `appimagetool` on `PATH`, or `-DMIKAI_APPIMAGE_TOOL=/path/to/appimagetool`. Packaging refuses a build configured without Qt deployment, refuses a pinned mpv bundle whose FFmpeg has no TLS backend, and refuses an output directory that already exists. The artifact is `mikai-<version>-x86_64.AppImage` and, with its `SHA256SUMS`, is written under `build/appimage/_appimage/`.

Qt's deployment omits the QPA platform plugins when the distribution Qt carries no qpa description (as on Arch), so the packaging copies the Qt plugin tree and gives every plugin an `$ORIGIN`-relative RUNPATH. The image deliberately excludes the C library, the loader and the GL/EGL driver libraries: a private libc beside the host loader is not portable, and GL dispatch belongs to the installed driver.

To lower the glibc baseline, run the same commands in an older base image with the provisioned Qt instead of on the current host; no lower-baseline artifact is claimed here.

#### AnimeJaNai gate for a Linux artifact

An artifact that ships the fork is only shippable if the filter can actually start inference. Run this
against what was packaged, with the user store present and the preset selectable:

1. Open a local file, choose **AnimeJaNai → Balanced**, and let it build. The toast must not raise
   `animejanai.core.unsupported`, `animejanai.backend.failed` or `animejanai.build.failed`.
2. Confirm the engine appeared and no build log carries a shell error:

```bash
S="$HOME/.local/share/Mikai Wrapper/native/animejanai/models/spatial"
ls -l "$S"/*.engine
grep -l "No such file or directory" "$S"/*.build.log      # must print nothing
```

3. Use a store path that contains a space — the default `…/Mikai Wrapper/…` — because that is the
   case an unquoted `LD_LIBRARY_PATH` breaks. A space-free store passes even when the shell-safety
   contract is broken, so it is not a substitute.
4. Check the session log exists for the packaged form. An AppImage mounts its payload read-only, so
   the log has to land in `AppLocalDataLocation` (`~/.local/share/mikai.log` here), not beside the
   executable inside the mount.

The two contracts this exercises — closure preload and shell-safe `trtexec-libdir` — are stated in
[`animejanai.md`](animejanai.md); they are silent until a GPU build runs, which is why this is a
release step rather than a developer note.

### Distribution packages (Arch, Debian)

Two distribution formats are produced from the same source tree. Neither is a publication approval, and neither replaces the tarball: the tarball stays the dependency-based reference layout.

Published Arch and Debian package checksums are available in each release's `SHA256SUMS`; locally rebuilt packages have their own checksums. Both drivers read the runtime archive from `release-artifacts/mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst` unless `--mpv-archive` points elsewhere, and stop with the build script's name if it is missing.

**Arch** builds from a git-exported tarball, not from a built tree, so the package and its revision cannot drift apart.

- It bundles the pinned runtime under `/usr/lib/mikai/mpv` — a directory named `mpv` in `/usr/bin` would collide with the distribution's `mpv` package — and depends on `qt6-base` and `libglvnd` rather than `mpv`.

```bash
bash packaging/linux/arch-package.sh --output-dir /fresh/output
```

- The driver refuses a modified worktree and a non-empty output directory, exports `HEAD` with `git archive`, substitutes the project version, the tarball digest and revision into `packaging/arch/PKGBUILD`, and runs `makepkg`.
- The driver requires `makepkg`, `git`, `cmake`, `ninja`, `sha256sum`, `patchelf`, and checks that `qt6-base` and `libglvnd` are installed; the configure step still needs mpv/libmpv development headers on the build host. `check()` runs this repository's CTest suite, so a package is only produced after the tests pass.
- The result is `mikai-<version>-<pkgrel>-x86_64.pkg.tar.zst` plus the generated `.SRCINFO`.
- Both Linux drivers resolve the pinned Anime4K tarball (`--anime4k-archive`, then `MIKAI_ANIME4K_ARCHIVE`, then `~/.cache/mikai/anime4k/`, then any `build/*/_dist-cache/anime4k/`) and hand it to the build, because the Debian builder runs without a guaranteed network path and the Arch package builds from a `git archive` that carries no shader blobs. Every package installs the 49 staged shaders beside the binary.
- The package depends at runtime on `qt6-base` and `libglvnd`, installs the executable as `/usr/bin/mikai` (with `/usr/bin/mikai-<version>` as its real name and `/usr/bin/shaders` linking to the shader directory), and relies on the distribution's Qt 6.12 — which is why this format only applies to Arch, where Qt 6.12 is current. `mpv` is a build-host dependency only.

**Debian** is built inside Debian 13 (trixie) because the binary's glibc floor is the build base's, and no Debian-family distribution ships Qt 6.12.

```bash
bash packaging/linux/deb-package.sh \
  --output-dir /fresh/output \
  --qt-dir /path/to/Qt/6.12.0/gcc_64
```

- The driver builds `packaging/debian/docker/Dockerfile` into `mikai-deb-builder:trixie` (Ubuntu-style builds are not interchangeable: trixie is the oldest base the package is meant for).
- It mounts the checkout read-only, the Qt SDK at `/qt` and the runtime archive at `/mpv/runtime.tar.zst`, and runs `packaging/debian/in-container.sh`.
- That script configures with `MIKAI_LINUX_DEPLOY_QT=ON` and `MIKAI_LINUX_BUNDLE_MPV=ON`, installs into `/usr` with the runtime at `/usr/lib/mikai/mpv`, derives `Depends` from what each shipped object can actually resolve (the runtime is bundled, so no distribution mpv is required), and builds `mikai_0.2.0_amd64.deb` with `dpkg-deb`.

- The Qt 6.12 SDK links ICU 73, which Debian 13 does not provide (it ships ICU 76).
- ICU 73 is compiled once into the builder image and shipped beside the Qt runtime under `/usr/lib`; every library there resolves its siblings through `$ORIGIN`.
- The build-time `LD_LIBRARY_PATH` is cleared before packaging and testing so a missing private dependency cannot be masked.
- The package carries `libc6 (>= 2.38)` from the shipped objects, which excludes Debian 12.

- The Arch driver does not install the produced package; it runs CTest inside `makepkg` and stages the payload into `$pkgdir`.
- The Debian script verifies by installing the `.deb`: it prints `--version`, starts the application as an unprivileged user with `QT_QPA_PLATFORM=offscreen`, and removes it, failing if the Qt platform plugin cannot load or the removal leaves the executable behind.
- A clean `debian:trixie-slim` container with only `apt-get install ./mikai_0.2.0_amd64.deb` was used for the recorded artifact, without the builder image's ICU directory.

- What neither format establishes: signing, publication, a physical GPU or audio acceptance run, or playback of provider streams.
- Both packages bundle the same pinned libmpv runtime; they differ in Qt — the Arch package uses the distribution's Qt 6.12, the Debian package deploys the 6.12 SDK and its ICU closure — so the two are not interchangeable builds of the same binary and must be tested as separate artifacts.

## Local source preview

To prepare a source archive from the current worktree without staging or committing it, use a fresh output directory:

```bash
python packaging/source/package.py \
  --source-dir . \
  --output-dir /tmp/mikai-source-preview
```

- The command writes `mikai-0.2.0-source.tar.gz`, `SHA256SUMS`, and `source-provenance.json`.
- It packages tracked files present in the worktree plus nonignored untracked files, so local changes and intended new assets are represented; ignored content, build/release outputs, user stores, logs, archives, and secret paths are excluded.
- Tracked sensitive paths and symlinks fail closed.
- The output must not exist and must not overlap the source tree.
- While archiving, each source file is hashed as its bytes are streamed, so provenance describes the bytes actually archived.
- Short reads, unexpected trailing bytes, changed file sizes and read failures abort packaging.
- This is not an atomic filesystem snapshot: a same-size edit before a member is read is represented by the newly archived content, not detected as a mutation.
- Review provenance and archive contents before sharing.

- Provenance records `format` 1, `artifact` `mikai-0.2.0-source.tar.gz`, `version` `0.2.0`, `snapshot_kind` `local-worktree-source-snapshot`, `head`, `dirty`, `commit_equivalence` `not-claimed`, `tree_sha256`, and `files[{path, sha256}]`.
- The tree digest is computed from those archived-byte hashes after archive creation.
- Selection is tracked files plus nonignored untracked files minus deleted files; exclusions include `.git`, `.omp`, build/release outputs, logs, caches, user stores, native runtime/model data, sensitive paths, symlinks, archives and logs.
- Production code under `src/player/native/models` is retained.
- The integrity guarantee is that hashes describe bytes actually archived; size/read checks do not detect every concurrent mutation or provide an atomic filesystem snapshot, and a dirty snapshot is not claimed to equal a commit or tag.

- This is explicitly a local preview: provenance records `HEAD`, whether the worktree is dirty, the sorted file hashes, and a deterministic tree digest, but does not claim that a dirty snapshot equals a commit or tag.
- It does not stage, commit, or tag anything.
- It is not a substitute for the trusted-tag CI archive, which is assembled from the exact validated tag commit described below, nor does preparing either archive authorize publication.

## CI and publication

The CI workflow handles pushes to `main`, pull requests and manual runs. Its `mikai-windows-x64` and `mikai-linux-x86_64` artifacts contain packages and evidence (including test logs/results and renderer evidence); they are workflow outputs, not permanent download URLs.

- The two profile lanes always run. The sanitizer matrix (TSan, UBSan, ASan+UBSan) and the packaged-runtime renderer smoke run only on manual dispatch, because they cost several times a normal build.
- `.github/workflows/nightly.yml` is a separate scheduled soak canary (software GL, null audio sink) that uploads `mikai-nightly-soak-evidence`; it is not a release gate, so its absence does not block packaging.
- The Windows CI lane configures with `-DMIKAI_DISABLE_IPO=ON` and passes its MinGW `libwinpthread-1.dll` for the dist contract; the Linux lane configures the bundled runtime plus Qt deployment and records the digest of the archive it built (`evidence/linux-mpv-runtime.json`).

The release workflow accepts `v*` tag pushes or a manual run for an existing default-branch tag, validates version/ancestry, and produces a `mikai-source-provenance` artifact containing package binaries, a versioned source archive (for v0.2.0: `mikai-0.2.0-source.tar.gz`), per-profile verification archives, `provenance.json`, `third-party.json`, `SHA256SUMS` and `RELEASE-NOTES.txt`.

Inspect the workflow files and run record for the exact contents and outcome of a particular run. Do not describe a workflow as passing unless its run c…

- The assembly step refuses unless each profile's `evidence/verification.json` reports `status: passed` for the checked-out commit with `optionalNativeIncluded: false`, and every listed package still matches its recorded SHA-256. Only the portable ZIP, setup EXE and Linux tarball are copied into the release bundle; the rest of each profile's output goes into its verification archive.
- Manual publication defaults to disabled.
- Tagging or enabling publication enters the protected `release` environment and must also pass `profiles.publicReleaseReady` in `packaging/third-party.json`; `python packaging/ci/release.py gate --directory <release-dir>` enforces it over the assembled bundle.
- That gate is currently `false` for both `windows-x64` and `linux-x86_64`, so public release is blocked; the optional-native manifest blocks are separate and stay excluded either way.
- Before enabling publication, configure required reviewers and version-tag restrictions on the environment and resolve the documented rights blockers.
- Workflows and scripts are preparation only until an actual run produces evidence; no publication is authorized here.

Before any public release, confirm the version and source revision, inspect package contents for secrets and user state, complete dependency/license/source obligations, retain checksum and provenance files, and record real build/test/package results. No publishing, tagging or upload is authorized by this document.
### Pushing the source repository

Only the source repository is pushed; artifacts, build trees, stores and local scratch are ignored by
`.gitignore` and must not be forced in. Preflight on a clean worktree:

```bash
git status --short                                 # must be empty
git ls-files | xargs du -h 2>/dev/null | sort -rh | head -5   # no binary the history should not carry
git grep -lI "/home/" -- . | grep -v "^docs/"       # absolute user paths belong to no one else
git log --oneline --decorate -5
```

- `main` is the branch CI builds on (`ci.yml` triggers on pushes to `main` and on pull requests); it
  is the branch to push.
- The published repository is <https://github.com/smyhlin/Mikai-Ja-Nai> (default branch `main`). A
  fresh checkout adds it explicitly; `ssh` needs a key registered on the account, so the configured
  remote uses HTTPS with `gh` as the credential helper (`gh auth setup-git`):

```bash
git remote add origin https://github.com/smyhlin/Mikai-Ja-Nai.git
git push -u origin main
git push origin --tags          # only if those tags are meant to be public
```

- The other local branches (`benchmark`, `migrate-to-qt-6.12`, `refactor/modular-structure`, …) are
  work in progress. Pushing `main` does not publish them; push a branch deliberately or leave it.
- The repository slug is `mikai-janai` and the display name is *Mikai Ja Nai* (`janai` is じゃない,
  "is not" — the client is unofficial, not the service). Set that in the repository description; the
  application's own identity stays `Mikai Wrapper`, because it is the Qt application name and
  therefore the native store path (see [Contributing](../CONTRIBUTING.md#naming)). Renaming the
  local checkout directory is optional and costs nothing in git, but every configured build tree
  records absolute source paths: reconfigure, or rebuild, before trusting a cached `build/` after
  such a move.

- Keep the repository history free of third-party binaries. A `squashfs-root/` tree from
  `--appimage-extract` reached one commit and was removed in `5869f5c`; if a clone must also drop it
  from history, rewrite once with Git's own filter before the first push:

```bash
git filter-repo --path squashfs-root --invert-paths
```

  That rewrites commit identities, so do it before anyone has cloned the repository; the `--version`
  strings stamped into local artifacts will then name commits that no longer exist.
- Never commit `.env*`, `.mcp.json`, API keys, Telegram tokens, wine prefixes, `release-artifacts/`,
  `build*/` or a populated `native/` store. `git status --ignored` is the check that none of them is
  being added by a broad `git add -A`.
