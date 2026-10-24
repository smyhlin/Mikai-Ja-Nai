#!/usr/bin/env python3
"""Create a local source snapshot from the current Git worktree."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import sys
import tarfile

VERSION = '0.1.0'
ARCHIVE_NAME = f'mikai-{VERSION}-source.tar.gz'
PROVENANCE_NAME = 'source-provenance.json'
CHECKSUMS_NAME = 'SHA256SUMS'

# These generated/private components are excluded anywhere they may appear.
EXCLUDED_COMPONENTS = {
    '.git', '.omp', '.probe-out', 'build', 'builds', 'cmake-build-debug',
    'cmake-build-release', 'dist', 'dist-mikai', 'release-artifacts', 'logs',
    'log', 'crashes', 'crash-dumps', 'cache', 'caches', '__pycache__',
    'probe-out', 'launchercontracts', 'test-results', 'testing',
    'collected', 'ci-artifacts',
}
SENSITIVE_COMPONENTS = {
    'secrets', 'secret', 'credentials', 'credential', 'logs', 'log', 'crashes',
    'crash-dumps', '.mcp.json', '$env',
}
ARCHIVE_SUFFIXES = ('.zip', '.7z', '.rar', '.tar', '.tgz', '.gz', '.bz2', '.xz', '.zst', '.whl', '.jar', '.apk', '.iso', '.cab', '.exe', '.msi', '.msix', '.appx', '.appimage', '.deb', '.rpm', '.dll', '.so', '.dylib')


class SourcePackageError(Exception):
    pass


def git(root: Path, *args: str) -> bytes:
    try:
        return subprocess.check_output(['git', '-C', str(root), *args], stderr=subprocess.PIPE)
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = exc.stderr.decode('utf-8', 'replace').strip() if isinstance(exc, subprocess.CalledProcessError) else str(exc)
        raise SourcePackageError(f'git {args[0]} failed: {detail}') from exc


def git_paths(root: Path, *args: str) -> set[str]:
    return {os.fsdecode(item) for item in git(root, *args).split(b'\0') if item}


def sensitive(path: str) -> bool:
    parts = PurePosixPath(path).parts
    lower = [part.casefold() for part in parts]
    name = lower[-1] if lower else ''
    return (any(part in SENSITIVE_COMPONENTS for part in lower)
            or any(part == '.env' or part.startswith('.env.') or part.startswith('.env-') for part in lower)
            or name in {'id_rsa', 'id_ed25519', 'known_hosts'}
            or name.endswith(('.pem', '.key', '.p12', '.pfx'))
            or any(token in name for token in ('secret', 'credential')))


def excluded(path: str) -> bool:
    parts = PurePosixPath(path).parts
    lower = [part.casefold() for part in parts]
    name = lower[-1] if lower else ''
    root_user_stores = {
        ('models',), ('user-data',), ('user-store',), ('user-stores',), ('userdata',),
        ('native', 'models'), ('native', 'user-data'), ('native', 'user-store'),
        ('native', 'user-stores'), ('native', 'userdata'),
    }
    root_user_store = any(tuple(lower[:len(store)]) == store for store in root_user_stores)
    native_runtime_data = len(lower) > 1 and lower[0] == 'native' and lower[1] in {
        'components', 'assets', 'animejanai', 'user-data', 'logs', 'models',
        'cache', 'caches', 'crashes', 'crash-dumps',
    }
    return (root_user_store
            or sensitive(path)
            or native_runtime_data
            or any(part in EXCLUDED_COMPONENTS for part in lower)
            or name.endswith(ARCHIVE_SUFFIXES)
            or name in {'sha256sums', 'source-provenance.json'}
            or name.endswith(('.log', '.dmp', '.core', '.pyc', '.pyo', '.pyd')))


def checked_file(root: Path, rel: str) -> tuple[Path, os.stat_result]:
    pure = PurePosixPath(rel)
    if (pure.is_absolute() or not pure.parts
            or any(part in ('', '.', '..') or '\\' in part or ':' in part for part in pure.parts)):
        raise SourcePackageError(f'unsafe repository path: {rel!r}')
    path = root.joinpath(*pure.parts)
    cursor = root
    for part in pure.parts:
        cursor = cursor / part
        try:
            info = cursor.lstat()
        except FileNotFoundError:
            raise SourcePackageError(f'Git path is missing from worktree: {rel}') from None
        if stat.S_ISLNK(info.st_mode):
            raise SourcePackageError(f'refusing symbolic link in source snapshot: {rel}')
    info = path.stat()
    if not stat.S_ISREG(info.st_mode):
        raise SourcePackageError(f'refusing non-regular source path: {rel}')
    resolved = path.resolve(strict=True)
    if not resolved.is_relative_to(root):
        raise SourcePackageError(f'source path escapes repository: {rel}')
    return path, info


class _HashingFile:
    def __init__(self, file: object) -> None:
        self.file = file
        self.hasher = hashlib.sha256()
        self.bytes_read = 0

    def read(self, size: int = -1) -> bytes:
        data = self.file.read(size)
        self.hasher.update(data)
        self.bytes_read += len(data)
        return data


def make_archive(root: Path, output: Path, entries: list[dict[str, object]]) -> None:
    archive = output / ARCHIVE_NAME
    with archive.open('wb') as raw:
        with gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0, compresslevel=9) as compressed:
            with tarfile.open(fileobj=compressed, mode='w', format=tarfile.PAX_FORMAT) as tar:
                for entry in entries:
                    rel = str(entry['path'])
                    path, info = checked_file(root, rel)
                    member = tarfile.TarInfo(f'mikai-{VERSION}/{rel}')
                    member.size = info.st_size
                    member.mode = 0o755 if info.st_mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH) else 0o644
                    member.uid = member.gid = 0
                    member.uname = member.gname = ''
                    member.mtime = 0
                    with path.open('rb') as source:
                        reader = _HashingFile(source)
                        try:
                            tar.addfile(member, reader)
                        except (OSError, tarfile.TarError) as exc:
                            raise SourcePackageError(f'source file changed while creating archive: {rel}') from exc
                        if reader.bytes_read != member.size:
                            raise SourcePackageError(f'source file changed while creating archive: {rel}')
                        if source.read(1):
                            raise SourcePackageError(f'source file changed while creating archive: {rel}')
                        if path.stat().st_size != member.size:
                            raise SourcePackageError(f'source file changed while creating archive: {rel}')
                        actual_hash = reader.hasher.hexdigest()
                    entry['sha256'] = actual_hash


def package(source_dir: Path, output_dir: Path) -> Path:
    source = source_dir.resolve(strict=True)
    if not source.is_dir():
        raise SourcePackageError(f'--source-dir is not a directory: {source}')
    root = Path(os.fsdecode(git(source, 'rev-parse', '--show-toplevel').strip())).resolve(strict=True)
    if source != root:
        raise SourcePackageError(f'--source-dir must be the Git worktree root: {root}')
    output = output_dir.resolve(strict=False)
    if output == source or source in output.parents or output in source.parents:
        raise SourcePackageError('source and output directories must not overlap')
    if output.exists() or output.is_symlink():
        raise SourcePackageError(f'output directory already exists; choose a fresh path: {output}')

    tracked = git_paths(root, 'ls-files', '--cached', '-z')
    sensitive_tracked = sorted(path for path in tracked if sensitive(path))
    if sensitive_tracked:
        raise SourcePackageError('tracked sensitive-path hazard(s): ' + ', '.join(repr(path) for path in sensitive_tracked))
    untracked = git_paths(root, 'ls-files', '--others', '--exclude-standard', '-z')
    deleted = git_paths(root, 'ls-files', '--deleted', '-z')
    candidates = sorted((tracked | untracked) - deleted, key=lambda value: value.encode('utf-8', 'surrogateescape'))
    files: list[tuple[str, Path, os.stat_result]] = []
    for rel in candidates:
        if excluded(rel):
            continue
        path, info = checked_file(root, rel)
        files.append((rel, path, info))

    entries = [{'path': rel} for rel, _, _ in files]
    head = git(root, 'rev-parse', 'HEAD').decode('ascii').strip()
    dirty = bool(git(root, 'status', '--porcelain=v1', '--untracked-files=all'))

    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir()
    make_archive(root, output, entries)
    # Hashes are authoritative only after the archive stream has consumed the
    # bytes; recompute the tree digest from the verified per-file hashes.
    tree_material = json.dumps(entries, ensure_ascii=True, sort_keys=True, separators=(',', ':')).encode('ascii')
    provenance = {
        'format': 1,
        'artifact': ARCHIVE_NAME,
        'version': VERSION,
        'snapshot_kind': 'local-worktree-source-snapshot',
        'head': head,
        'dirty': dirty,
        'commit_equivalence': 'not-claimed',
        'tree_sha256': hashlib.sha256(tree_material).hexdigest(),
        'files': entries,
    }
    provenance_path = output / PROVENANCE_NAME
    provenance_path.write_text(json.dumps(provenance, ensure_ascii=True, sort_keys=True, indent=2) + '\n', encoding='utf-8', newline='\n')
    sums = []
    for name in (ARCHIVE_NAME, PROVENANCE_NAME):
        digest = hashlib.sha256((output / name).read_bytes()).hexdigest()
        sums.append(f'{digest}  {name}')
    (output / CHECKSUMS_NAME).write_text('\n'.join(sums) + '\n', encoding='ascii', newline='\n')
    return output


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description='Create a deterministic local source snapshot from Git-tracked and nonignored worktree files.')
    parser.add_argument('--source-dir', type=Path, required=True, help='Git worktree root')
    parser.add_argument('--output-dir', type=Path, required=True, help='fresh output directory (must not overlap source)')
    args = parser.parse_args(argv)
    try:
        package(args.source_dir, args.output_dir)
    except (SourcePackageError, OSError) as exc:
        print(f'source package failed: {exc}', file=sys.stderr)
        return 1
    print(f'Created {args.output_dir / ARCHIVE_NAME}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
