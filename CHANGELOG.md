# Changelog

## 0.1.0 — Unreleased

### Release preparation

- Added an original Mikai icon for the application, Windows packages and Linux desktop integration.
- Added Windows x64 portable ZIP and per-user installer generation, payload inventories, checksums and provenance records.
- Added Linux x86_64 packaging with explicit system-mpv and private bundled-mpv profiles, optional Qt deployment and relative runtime search paths.
- Added source-snapshot packaging that hashes the bytes actually archived, excludes private/runtime data and records when a worktree is not equivalent to a commit.
- Added CI and protected release preparation with trusted-tag ancestry, artifact-integrity and explicit redistribution gates.

### Corrections

- Preserved processing-off startup even when enhancement presets were previously saved; processing requires an explicit request.
- Preserved pause intent and stopped-source state across OpenGL context recreation.
- Resolved explicitly selected runtime bundles through the validated resolver instead of constructing untrusted loader handoffs.
- Separated the localized runtime-availability summary from the retained technical refusal cause.
- Made installer Start Menu group selection effective and verified Unicode shortcut targets through the native Windows interface.
- Added installer payload-hash, upgrade/uninstall data-preservation and integration-preservation checks.
- Fixed Windows bundle-inspector symbol lookup recursion and Linux shared-library classification; added actual Standard-bundle verification and unsupported-Native refusal contracts.
- Corrected isolated Windows process ownership so console hosts are not mistaken for test descendants; exact child exit codes and bounded descendant cleanup are verified.
- Added real registry-sandbox coverage for foreign registration preservation and partial-failure rollback without opening Windows Settings.
- Added fullscreen aging diagnostics with explicit foreground, playback-owner, source-epoch and OpenGL-context validity checks; invalid desktop observations remain failures rather than being reported as visible playback proof.
- Removed accidental patch markers from source/notice files and excluded root probe scratch files from public source selection.

### Distribution status

These are unreleased source changes and local preparation tools, not publication approval. Windows installers are unsigned. Linux archives still require compatible host graphics, audio and libc facilities. The supplied third-party mpv/FFmpeg builds have unresolved corresponding-source and license-fulfillment requirements; their public redistribution remains blocked. See [release status](docs/release-status.md) and [redistribution](docs/redistribution.md) for evidence and limitations.
