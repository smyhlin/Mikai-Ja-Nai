<p align="center">
  <img src="resources/app/mikai.png" width="128" alt="Mikai Ja Nai app icon">
</p>

<h1 align="center">Mikai Ja Nai</h1>

<p align="center">An unofficial Mikai Ja Nai desktop client for the Mikai public anime catalogue API, with a local video player and optional video-processing tools.</p>

<p align="center">
  <a href="docs/user-guide.md">Quick start & catalogue</a> ·
  <a href="docs/player-guide.md">Player & Compare</a> ·
  <a href="docs/processing-guide.md">Processing & benchmarks</a> ·
  <a href="docs/settings-guide.md">Settings & privacy</a> ·
  <a href="docs/building.md">Build from source</a> ·
  <a href="docs/releasing.md">Packaging and release</a> ·
  <a href="docs/architecture.md">Architecture</a> ·
  <a href="CHANGELOG.md">Changelog</a> ·
  <a href="https://api.mikai.me/public/docs">Public API reference</a> ·
  <a href="CONTRIBUTING.md">Contributing</a> ·
  <a href="SECURITY.md">Security</a>
</p>

> Mikai Ja Nai is an independent, unofficial API client. It is not the Mikai service and is not affiliated with or endorsed by the service operators. Catalogue availability and provider playback depend on the upstream service.

## What it does

- Browse and search the Mikai catalogue with Quick Categories, Grid/List views, tri-state Genre/Tag filters, saved filter presets, License Holders, a full Teams Directory with Team Profiles and Ukrainian Poster galleries, schedules, and a local recently-opened list.
- Play provider streams or open local video files in an integrated **libmpv** player, through the picker, a drag-and-drop or an Open With hand-off. Local playback is independent of catalogue sign-in/API availability, and the app can offer itself as a per-user default-player candidate without changing an existing default on its own.
- Compare a paused frame across Original, Anime4K, AnimeJaNai and prepared Native candidates using true source replay, frame-stepping and difference metrics; export flicker comparisons as GIF, animated WebP or silent MP4.
- Cut a short video moment (MP4) from seekable media using an integrated filmstrip sheet bounded to ±2 minutes around the current frame, with an independent preview playhead, live audio preview, and stream-bitrate size estimates.
- Optionally acquire GPU-specific components and models for AI processing: the official AnimeJaNai v0.24.0 pair, and on Windows an opt-in TensorRT for RTX runtime. Ordinary Original playback does not require AI components; enhancement is opt-in.
- Skip the openings, endings and recaps the provider publishes for an episode, with accent-tinted timeline ranges and per-segment skip buttons.
- Preview the timeline on hover with decoded frame thumbnails, and move playback into a floating picture-in-picture window.
- Change how the picture fills the window: Fit, Fill, Stretch, 16:9, 4:3, original size, zoom, upscaler and downscaler.
- Load local subtitle files into local videos and network streams, search OpenSubtitles, inspect local media metadata, manage tracks, and customize the interface in English or Ukrainian.
- Resume where you stopped: playback starts at 00:00 and offers a stored position via an in-app themed prompt, the episode is marked watched at 90 % of its length, and the title page shows the same progress.
- Read the source at a glance in the player footer: HDR, Dolby Vision, Dolby Atmos and the video codec, based on what the container reports.
- Identify the exact binary you are running with `--version`, which prints the commit and build time without needing a desktop session.

## Screenshots

Application captures and their provenance are documented in [Screenshot provenance](docs/images/README.md).

<table>
  <tr>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/catalogue.png" alt="Catalogue home (0.2.4.1) with live Mikai titles and posters, sidebar, Ctrl+K search bar, Quick Categories chips and request quota badge"></td>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/search-results-grid.png" alt="Filtered Search Results Grid (0.2.4.1) with live Mikai Action titles, active filter chip, format pills, TitleCards and RangePager"></td>
  </tr>
  <tr>
    <td align="center"><em><strong>Catalogue Home (0.2.4).</strong> Sidebar, Ctrl+K search, Quick Categories and quota badge.</em></td>
    <td align="center"><em><strong>Search Grid (0.2.4).</strong> Active filter chips, format pills, TitleCards and compact RangePager.</em></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/title-page.png" alt="Live Title Page (0.2.4.1) with poster, synopsis, Dubbed chips, Provider tabs, clickable Teams pills and episode grid"></td>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/filters.png" alt="Catalogue filters dialog (genres, tags, years, format, status, season, studio, sort)"></td>
  </tr>
  <tr>
    <td align="center"><em><strong>Title page (0.2.4).</strong> Release chips, Provider tabs, clickable Teams pills and episode grid.</em></td>
    <td align="center"><em><strong>Filters.</strong> Genres, tags, years, format, status, season, studio and sort.</em></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/player.png" alt="Qt Quick/QML player paused on a 1080p fixture with timeline, transport controls and codec badges"></td>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/player-hover-preview.png" alt="Timeline hover thumbnail preview card with a decoded frame"></td>
  </tr>
  <tr>
    <td align="center"><em><strong>Player.</strong> Paused on a 1080p fixture with timeline, transport controls and codec badges.</em></td>
    <td align="center"><em><strong>Hover preview.</strong> Timeline hover thumbnail preview card with a decoded frame.</em></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/player-pip.png" alt="Redesigned compact Picture-in-Picture window"></td>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/media-info.png" alt="Media Information dialog showing container and stream properties"></td>
  </tr>
  <tr>
    <td align="center"><em><strong>PiP.</strong> Redesigned compact window with auto-hide and full-surface drag.</em></td>
    <td align="center"><em><strong>Media info.</strong> Container, video, audio and subtitle stream properties.</em></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/clip.png" alt="Cut Moment / Clip editor window with sequential filmstrip, trim handles, quality selector and size estimate"></td>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/compare.png" alt="Side-by-side Compare window (Original vs Anime4K A Fast)"></td>
  </tr>
  <tr>
    <td align="center"><em><strong>Clip.</strong> Cut Moment editor with filmstrip, trim handles, quality selector and size estimate.</em></td>
    <td align="center"><em><strong>Compare.</strong> Original vs Anime4K A Fast, side by side.</em></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/player-settings-menu.png" alt="Player Settings popover menu (quality, speed, audio, subtitles, scaling)"></td>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/player-processing-menu.png" alt="Player Enhancements/Processing popover menu (Original, Anime4K presets, AnimeJaNai, Native)"></td>
  </tr>
  <tr>
    <td align="center"><em><strong>Settings menu.</strong> Quality, speed, audio, subtitles and scaling.</em></td>
    <td align="center"><em><strong>Processing menu.</strong> Original, Anime4K presets, AnimeJaNai and Native.</em></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/settings-general.png" alt="Settings Application Access and General (interface language, HDR policy, Stable/Nightly channel, auto-update, scaling, API key)"></td>
    <td width="50%"><img src="docs/images/gallery-2026-11-24/settings.png" alt="Settings Customization Appearance (accent, corner radius, background gradient)"></td>
  </tr>
  <tr>
    <td align="center"><em><strong>Settings general.</strong> Interface language, HDR policy, Stable/Nightly channel, auto-update, scaling and API key.</em></td>
    <td align="center"><em><strong>Settings appearance.</strong> Accent, corner radius and background gradient.</em></td>
  </tr>
</table>

These automated Linux x86_64 captures (`gallery-2026-11-24/`, `mikai 0.2.4.1`, Qt 6.12.0, xcb/Xvfb) show the English interface with the shipped dark theme. Catalogue Home (36/36 live posters), Search Results Grid (50 live Action posters), Filters, and Title Page use the live anonymous Mikai public API; Player, Hover Preview, PiP, Clip, Compare, and Media Information use a 15-second 1080p24 HEVC/AAC synthetic `testsrc2` local fixture. Catalogue artwork and availability can change upstream. The images do not establish provider playback, AI inference, performance, or release QA.

## Download and install

Builds are published at [github.com/smyhlin/Mikai-Ja-Nai/releases](https://github.com/smyhlin/Mikai-Ja-Nai/releases). Available formats vary by release: v0.2.1 has a Windows portable ZIP and per-user setup, and Linux `tar.gz` and AppImage packages. Arch and Debian packaging paths are also available in the source tree.

Every release ships `SHA256SUMS`. Verify downloads against it before running them, and avoid unofficial mirrors.

The artifacts are unsigned, and [release status](docs/release-status.md) records what remains unverified — including the third-party source and redistribution obligations in [redistribution](docs/redistribution.md).

Linux packages include the project's libmpv runtime, but still require compatible host graphics, audio and system libraries. They are not universal Linux bundles. The original Windows v0.1.0 packages have a startup-runtime issue; the [player and usability update](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0-update-20261101) includes the fix.

As checked on 2026-11-24, the latest published release is **[v0.2.3.1](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.2.3.1)**. Current source is **0.2.4.1** (Categories, Search, and Teams overhaul with tri-state filters, Grid/List search results, Teams Directory & Profiles, Ukrainian Posters gallery, crisp vertical wheel scrolling, `TitlePage` episode-grid fixes, Qt Quick player cleanup, and live Mikai API query fixes). See the [application state and release status](docs/release-status.md) for observed checks, open issues and publication blockers.

## Quick start

For Windows, the portable ZIP is extracted and launched from its own folder; the setup executable is per-user. On Linux, install the Arch or Debian package with the distribution's package manager, run the AppImage, or unpack the tarball anywhere. All four bring their own mpv runtime; the Arch package uses the distribution's Qt 6.12, the `.deb` and AppImage carry Qt 6.12 themselves, and the tarball expects a compatible host Qt.

See the [Linux packaging notes](docs/releasing.md#linux-x86_64) and [distribution packages](docs/releasing.md#distribution-packages-arch-debian). The API key is optional; it raises API rate limits and can be configured in **Settings → Application → Access**. Keep any key private.

To build from source, see [Building](docs/building.md). On first startup, the app can offer optional component setup; decline it to use ordinary playback.

Search the catalogue, open a title and select a provider episode, or choose **Open video** to play a local file. Choose an enhancement explicitly from the player's processing menu when the required runtime and models are installed.

## HDR playback verification

The player supports HDR-source-to-SDR playback policies (Auto, Stable and
Vivid); this is not a claim of native HDR display output.

Dedicated probe
instructions for natural footage, explicit natural-media mode, bounded scene
selection, fullscreen aging, strict fixture assertions and diagnostic-only
black-candidate interpretation are in the [fixture and probe guide](tests/fixtures/README.md).

Original-film evidence and captures remain external to the repository. See
[release status](docs/release-status.md) for the scoped verification and
publication hold.

## Platform support

The project has Windows x64 MinGW and Linux x86_64 build/package paths using Qt 6.12+, Qt Quick/QML, C++20 and desktop OpenGL. Linux can optionally deploy Qt and/or bundle a pinned private `libmpv`; otherwise the configured runtime uses host Qt and system `libmpv`. The player presentation is built with Qt Quick and a C++ controller over the existing `VideoSession`/libmpv core; PiP keeps that same window and session. Compare, Clip, Media Information, Settings and Components remain separate Widgets interfaces.

These options do not establish broad distribution portability: host glibc/loader, graphics drivers and audio remain relevant, and graphics support varies by machine. Windows development uses the pinned mpv DEV archive containing headers/import library/`libmpv-2.dll`; the separate CLI/updater archive is not used or bundled.

The pinned DEV and Linux runtime archives are the 2026-10-24 builds; see [platform-specific build and runtime requirements](docs/building.md#platforms-and-prerequisites) for identities and the DEV-vs-CLI distinction.

TensorRT for RTX is a Windows-only opt-in: it needs a supported NVIDIA GPU and compatible driver, and its separately acquired runtime does not replace classic TensorRT. Upstream publishes no Linux RTX asset, so Linux builds keep classic TensorRT. The component and admission contracts are described under [optional enhancement paths](docs/architecture.md#optional-enhancement-paths).

## Credits and licensing

Mikai Ja Nai uses [mpv/libmpv](https://mpv.io/), [Anime4K](https://github.com/bloc97/Anime4K), [mpv-AnimeJaNai](https://github.com/the-database/mpv-AnimeJaNai), [animejanai-inference](https://github.com/the-database/animejanai-inference), Qt and other third-party components; these remain subject to their own terms. Its source is released under the [Unlicense](LICENSE), which does not relicense third-party code, models or runtime packages.

See [third-party notices and rights records](native/manifest/THIRD_PARTY_NOTICES.md) and the component manifests before redistributing a build.

The catalogue/API belongs to the Mikai service. This client is unofficial and does not claim ownership of service content or provider streams. More at [Mikai](https://mikai.me) and its [public API documentation](https://api.mikai.me/public/docs).

## More documentation

- [Quick start & catalogue](docs/user-guide.md): first launch, catalogue browsing and common workflows.
- [Player & Compare](docs/player-guide.md): playback controls, local media, track selection and frame comparison.
- [Processing & benchmarks](docs/processing-guide.md): optional processing components, models, profiles and benchmark reports.
- [Settings & privacy](docs/settings-guide.md): application settings, access credentials, customization and local data.
- [Building](docs/building.md): toolchains, dependencies, configure/build/test commands and runtime requirements.
- [Releasing](docs/releasing.md): local package creation, artifact contents, legal and publication limitations.
- [Application state and release status](docs/release-status.md): current feature boundaries, scoped verification, local artifact identity and open release gates.
- [Architecture](docs/architecture.md): application layers, background work, playback/rendering, enhancement runtimes and data boundaries.
- [AnimeJaNai troubleshooting](docs/animejanai.md): the fork's filter admission contract and the failures it produces.
- [Public API v1 reference](https://api.mikai.me/public/docs); [benchmark evidence and limitations](bench_results.md); [Native build and bundle documentation](native/README.md) and [MSVC build guide](native/build-msvc.md).