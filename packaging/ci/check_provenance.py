"""Provenance/pin lockstep check between FindOrFetchMpv.cmake and the runtime provenance JSON.

T-419: the Windows mpv archive pin in `cmake/FindOrFetchMpv.cmake`
(MIKAI_MPV_ARCHIVE_NAME / MIKAI_MPV_ARCHIVE_URL / MIKAI_MPV_ARCHIVE_SHA256)
and the hand-curated record in `packaging/provenance/windows-runtime.json`
(`build.mpv.asset` plus `build.mpv.sourceCommit` / `release`) are maintained
by hand and must move together. Nothing else in the tree cross-checks them; a
pin bump that misses the JSON (or the reverse) ships a provenance record that
describes a runtime the build never used - and, once provenance clears, a
runtime the build actually no longer ships.

Comparable fields, enumerated (both sources must be present for every one):
  1. asset URL      - cmake MIKAI_MPV_ARCHIVE_URL vs json build.mpv.asset.url
  2. asset SHA-256  - cmake MIKAI_MPV_ARCHIVE_SHA256 vs json build.mpv.asset.sha256
  3. archive name   - cmake MIKAI_MPV_ARCHIVE_NAME vs the URL's last path
                      segment (self-consistency of the cmake side, and the
                      name the JSON URL must also end with)
  4. source commit  - json build.mpv.sourceCommit must start with the short
                      revision embedded in the archive name ("...-git-<short>.7z")
  5. release tag    - the tag segment of json build.mpv.release must appear in
                      the pinned URL path (the download lives in that release)

Registered as the `mikai_provenance_check` ctest in cmake/MikaiTests.cmake,
which is how CI runs it (packaging/ci/build.py invokes plain ctest). No
third-party imports; Python 3 only. Any mismatch prints the exact field,
both values and the file it came from, then exits nonzero.
"""
import argparse
import json
import re
import sys
from pathlib import Path


def fail(message):
    print(f'mikai_provenance_check: FAIL {message}', flush=True)
    sys.exit(1)


def cmake_string(text, variable):
    """Reads one `set(<VARIABLE> "...")` from a CMake file, newline-tolerant.

    FindOrFetchMpv.cmake breaks MIKAI_MPV_ARCHIVE_URL across two lines; a
    single-line regex would silently read nothing, and an absent pin must be
    a presence failure, not a null comparison.
    """
    match = re.search(r'set\(\s*' + variable + r'\s*"([^"]+)"', text, re.S)
    if match is None:
        fail(f'cmake/FindOrFetchMpv.cmake does not set {variable}')
    return match.group(1)


def json_field(document, path):
    node = document
    for key in path:
        if not isinstance(node, dict) or key not in node:
            fail(f'packaging/provenance/windows-runtime.json is missing {".".join(path)}')
        node = node[key]
    if not isinstance(node, str) or not node.strip():
        fail(f'packaging/provenance/windows-runtime.json field {".".join(path)} is empty')
    return node.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root = args.source_root.resolve()

    cmake_path = root / 'cmake' / 'FindOrFetchMpv.cmake'
    json_path = root / 'packaging' / 'provenance' / 'windows-runtime.json'
    for path in (cmake_path, json_path):
        if not path.is_file():
            fail(f'required input is missing: {path}')

    cmake_text = cmake_path.read_text(encoding='utf-8')
    archive_name = cmake_string(cmake_text, 'MIKAI_MPV_ARCHIVE_NAME')
    archive_url = cmake_string(cmake_text, 'MIKAI_MPV_ARCHIVE_URL')
    archive_sha = cmake_string(cmake_text, 'MIKAI_MPV_ARCHIVE_SHA256')

    # The cmake side composes the URL from the archive name variable
    # (`.../${MIKAI_MPV_ARCHIVE_NAME}`). Expand exactly that one reference so
    # the comparison is against the URL the build actually downloads; any
    # OTHER unexpanded placeholder is a pin nobody can verify, and fails.
    archive_url = archive_url.replace('${MIKAI_MPV_ARCHIVE_NAME}', archive_name)
    unresolved = re.search(r'\$\{[A-Za-z0-9_]+\}', archive_url)
    if unresolved is not None:
        fail(f'the pinned URL still contains the unresolved placeholder '
             f'{unresolved.group(0)!r} after expanding the archive name')

    document = json.loads(json_path.read_text(encoding='utf-8'))
    asset_url = json_field(document, ['build', 'mpv', 'asset', 'url'])
    asset_sha = json_field(document, ['build', 'mpv', 'asset', 'sha256'])
    source_commit = json_field(document, ['build', 'mpv', 'sourceCommit'])
    release_url = json_field(document, ['build', 'mpv', 'release'])

    problems = []

    # 1. asset URL: the download the build verifies is the download the
    # provenance record describes.
    if archive_url != asset_url:
        problems.append(f'asset URL disagrees: FindOrFetchMpv.cmake pins {archive_url!r}, '
                        f'provenance records {asset_url!r}')

    # 2. asset SHA-256: the digest the build enforces is the digest the record
    # kept. Case is normalized; the pin files are lowercase hex either way.
    if archive_sha.lower() != asset_sha.lower():
        problems.append(f'asset SHA-256 disagrees: FindOrFetchMpv.cmake pins '
                        f'{archive_sha.lower()!r}, provenance records {asset_sha.lower()!r}')

    # 3. archive name: the cmake side must be self-consistent, and the JSON's
    # URL (already proven equal to the cmake URL) must carry the same member.
    if archive_name != archive_url.rsplit('/', 1)[-1]:
        problems.append(f'archive name disagrees with the URL: MIKAI_MPV_ARCHIVE_NAME is '
                        f'{archive_name!r}, the pinned URL ends with '
                        f'{archive_url.rsplit("/", 1)[-1]!r}')
    if asset_url.rsplit('/', 1)[-1] != archive_name:
        problems.append(f'provenance asset URL does not name the pinned archive '
                        f'{archive_name!r}')

    # 4. source commit: the short revision in the archive name must be a
    # prefix of the full 40-hex commit the provenance record kept.
    short_match = re.search(r'git-([0-9a-f]{7,40})\.', archive_name)
    if short_match is None:
        problems.append(f'archive name {archive_name!r} embeds no "git-<short-rev>" revision '
                        'to compare against the provenance source commit')
    elif not re.fullmatch(r'[0-9a-f]{40}', source_commit):
        problems.append(f'provenance sourceCommit {source_commit!r} is not a 40-hex revision')
    elif not source_commit.lower().startswith(short_match.group(1).lower()):
        problems.append(f'source commit disagrees: the archive name says '
                        f'{short_match.group(1)!r}, provenance records {source_commit!r}')

    # 5. release tag: the record's release tag must be the release the pinned
    # download URL actually lives under.
    release_tag = release_url.rstrip('/').rsplit('/', 1)[-1]
    if not release_tag or f'/{release_tag}/' not in archive_url:
        problems.append(f'release tag {release_tag!r} (from {release_url!r}) does not appear '
                        f'in the pinned archive URL {archive_url!r}')

    if not re.fullmatch(r'[0-9a-fA-F]{64}', archive_sha):
        problems.append(f'MIKAI_MPV_ARCHIVE_SHA256 {archive_sha!r} is not 64 hex characters')

    if problems:
        for problem in problems:
            print(f'mikai_provenance_check: FAIL {problem}', flush=True)
        print('The pin in cmake/FindOrFetchMpv.cmake and the record in '
              'packaging/provenance/windows-runtime.json must be updated together.',
              flush=True)
        sys.exit(1)

    print('mikai_provenance_check: PASS (archive, URL, SHA-256, source commit and release '
          'tag agree between FindOrFetchMpv.cmake and windows-runtime.json)', flush=True)
    print(f'  archive : {archive_name}', flush=True)
    print(f'  url     : {archive_url}', flush=True)
    print(f'  sha256  : {archive_sha.lower()}', flush=True)
    print(f'  commit  : {source_commit}', flush=True)
    print(f'  release : {release_url}', flush=True)
    sys.exit(0)


if __name__ == '__main__':
    main()
