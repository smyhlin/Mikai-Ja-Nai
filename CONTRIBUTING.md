# Contributing

Mikai is a C++20 / Qt Widgets desktop client. Before making a change, read the relevant user/build documentation and existing code conventions. Keep changes focused, preserve platform-specific behavior, and do not commit generated builds, release packages, local runtime stores, credentials, or personal media.

## Naming

- **Repository:** <https://github.com/smyhlin/Mikai-Ja-Nai> — the GitHub name is `Mikai-Ja-Nai`
  (hyphenated, case-insensitive in URLs); the checkout directory name is local and carries no weight.
  Do not reintroduce the underscore spelling or use bare `Mikai` as the application's public name.
- **Display name:** *Mikai Ja Nai* in prose where a human reads it; `mikai-janai` in identifiers.
  *janai* (じゃない) means "is not", which is the point: an unofficial client that is not the service.
- **Service name:** *Mikai* refers to mikai.me and its public API; do not use it as the application's public name.
- **GitHub description** (one line, shown under the repo name):
  `Unofficial Mikai Ja Nai desktop client — Qt 6 + libmpv player with Anime4K and AnimeJaNai upscaling, HDR, and a Compare workstation for Linux and Windows.`
- **Qt storage identity stays `Mikai` / `Mikai Wrapper`.** `QCoreApplication::applicationName()` is
  `Mikai Wrapper`, so `QStandardPaths` resolves to `~/.local/share/Mikai Wrapper/…` — the native
  store path. Renaming it relocates every installed component, model and setting, and that path's
  space is what the AnimeJaNai fork's unquoted `LD_LIBRARY_PATH` cannot survive (see
  [`docs/animejanai.md`](docs/animejanai.md)). A repository rename must not touch either Qt identity.

## Changes and review

- Base changes on the current project branch and keep each change narrowly scoped.
- Explain the user-visible effect and any important design trade-offs in the pull request. Link related issues when applicable; do not include private user data or secrets.
- For bug reports, include reproducible steps, expected and actual behavior, platform, and relevant sanitized logs. See the issue templates.
- Add or update focused automated coverage for behavior changes where appropriate. Do not claim checks you did not run; list the exact checks and results in the pull request.
- Documentation claims: countable invariants about the tree — for example, "X is the only copy" or "N finished, 0 unfinished" — must name the ctest entry, script or probe that verifies them. For example, `mikai_provenance_check` fails when the mpv archive pin in `cmake/FindOrFetchMpv.cmake` no longer matches `packaging/provenance/windows-runtime.json`; a doc section restating that pin should name the check.
  A claim no command can check is drift waiting to happen. Name its verifying command in the same sentence or section, or delete the claim.
- Follow the license and third-party notices in the repository. Do not add dependencies, media, icons, fonts, or other assets unless their provenance and redistribution terms are clear.

## Documentation style

- Keep prose paragraphs under about 400 characters; split longer passages where that improves readability. Prefer bullets or tables for related facts and evidence.
- In prose, show SHA-256 values as the first 12 hexadecimal characters in backticks. State where each full digest lives, such as the corresponding source manifest, provenance JSON or release `SHA256SUMS`.
- Keep full 64-character SHA-256 digests in tables, JSON, or `SHA256SUMS` files only. If a full digest is not already in JSON or `SHA256SUMS`, retain it in a Markdown table and link the short reference to that table.
- On first mention, format sizes as `41.5 MB (41,533,948 B)`; later mentions may use `41.5 MB`.
- Artifact tables must point readers to the release `SHA256SUMS` or the relevant provenance record for full hashes.

## Build and checks

Use [Building Mikai Ja Nai](docs/building.md) for prerequisites and platform-specific configure/build/test commands.

The release assembly entry points are `cmake --build <build> --target dist`, `packaging/windows/package.ps1` (with `-BuildDir` and `-OutputDir`), and `packaging/linux/package.sh` (build and output directories). Read [Releasing Mikai Ja Nai](docs/releasing.md) before changing packaging or release workflows. Do not commit generated output from these commands.

For every contribution, run relevant checks for the affected code and state what was or was not exercised. Release artifacts are prepared locally or by the configured workflows; contribution does not imply authorization to publish or sign a release.
