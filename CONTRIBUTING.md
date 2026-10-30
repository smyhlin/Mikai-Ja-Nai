# Contributing

Mikai is a C++20 / Qt Widgets desktop client. Before making a change, read the relevant user/build documentation and existing code conventions. Keep changes focused, preserve platform-specific behavior, and do not commit generated builds, release packages, local runtime stores, credentials, or personal media.

## Naming

- **Repository slug:** `mikai-janai` — lowercase, hyphenated, the form GitHub shows and the one to use
  in clone URLs and remotes. Do not reintroduce the underscore spelling or a bare `Mikai`.
- **Display name:** *Mikai Ja Nai* in prose where a human reads it; `mikai-janai` in identifiers.
  *janai* (じゃない) means "is not", which is the point: an unofficial client that is not the service.
- **GitHub description** (one line, shown under the repo name):
  `Unofficial mikai.me desktop client — Qt 6 + libmpv player with Anime4K and AnimeJaNai upscaling, HDR, and a Compare workstation for Linux and Windows.`
- **Application identity stays `Mikai Wrapper`.** `QCoreApplication::applicationName()` is that
  string, so `QStandardPaths` resolves to `~/.local/share/Mikai Wrapper/…` — the native store path.
  Renaming it relocates every installed component, model and setting, and that path's space is what
  the AnimeJaNai fork's unquoted `LD_LIBRARY_PATH` cannot survive (see
  [`docs/animejanai.md`](docs/animejanai.md)). A repository rename must not touch it.

## Changes and review

- Base changes on the current project branch and keep each change narrowly scoped.
- Explain the user-visible effect and any important design trade-offs in the pull request. Link related issues when applicable; do not include private user data or secrets.
- For bug reports, include reproducible steps, expected and actual behavior, platform, and relevant sanitized logs. See the issue templates.
- Add or update focused automated coverage for behavior changes where appropriate. Do not claim checks you did not run; list the exact checks and results in the pull request.
- Documentation claims: a README or doc sentence of the form "X is the only copy", "N finished, 0 unfinished", or any other countable invariant about the tree must name the command that verifies it — the ctest entry, script or probe that fails when the sentence stops being true (for example, `mikai_provenance_check` is the ctest entry that fails when the mpv archive pin in `cmake/FindOrFetchMpv.cmake` stops matching the record in `packaging/provenance/windows-runtime.json`; a doc section that restates that pin should name it). A claim no command can check is drift waiting to happen; either name the verifying command in the same sentence or section, or delete the claim.
- Follow the license and third-party notices in the repository. Do not add dependencies, media, icons, fonts, or other assets unless their provenance and redistribution terms are clear.

## Build and checks

Use [Building Mikai](docs/building.md) for prerequisites and platform-specific configure/build/test commands. The release assembly entry points are `cmake --build <build> --target dist`, `packaging/windows/package.ps1` (with `-BuildDir` and `-OutputDir`), and `packaging/linux/package.sh` (build and output directories). Read [Releasing Mikai](docs/releasing.md) before changing packaging or release workflows. Do not commit generated output from these commands.

For every contribution, run relevant checks for the affected code and state what was or was not exercised. Release artifacts are prepared locally or by the configured workflows; contribution does not imply authorization to publish or sign a release.
