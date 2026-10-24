"""Validate trusted release refs, assemble source/provenance, and fail closed on rights."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT, text=True).strip()


def version():
    text = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
    match = re.search(r'project\(mikai\s+VERSION\s+(\d+\.\d+\.\d+)', text, re.IGNORECASE)
    if not match:
        raise RuntimeError('CMake project(mikai VERSION x.y.z ...) must declare the release version')
    return match.group(1)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate(tag):
    if not re.fullmatch(r'v\d+\.\d+\.\d+', tag):
        raise RuntimeError('Release tag must be an existing exact vMAJOR.MINOR.PATCH tag')
    default = os.environ['DEFAULT_BRANCH']
    event = os.environ['GITHUB_EVENT_NAME']
    if event == 'workflow_dispatch' and os.environ['GITHUB_REF'] != f'refs/heads/{default}':
        raise RuntimeError('Manual release workflow must run from the repository default branch')
    if event == 'push' and os.environ['GITHUB_REF'] != f'refs/tags/{tag}':
        raise RuntimeError('Tag event and requested release tag differ')
    if event not in ('push', 'workflow_dispatch'):
        raise RuntimeError('Only trusted tag pushes and explicit default-branch dispatches may prepare releases')
    commit = git('rev-parse', '--verify', f'refs/tags/{tag}^{{commit}}')
    ancestry = subprocess.run(
        ['git', 'merge-base', '--is-ancestor', commit, f'refs/remotes/origin/{default}'],
        cwd=ROOT, check=False, capture_output=True, text=True)
    if ancestry.returncode:
        raise RuntimeError(
            f'Release tag {tag} commit {commit} is not reachable from default branch {default}')
    cmake = git('show', f'{commit}:CMakeLists.txt')
    match = re.search(r'project\(mikai\s+VERSION\s+(\d+\.\d+\.\d+)', cmake, re.IGNORECASE)
    if not match or tag != 'v' + match.group(1):
        raise RuntimeError('Tag does not match the tagged CMake project version')
    if os.getenv('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as stream:
            stream.write(f'tag={tag}\ncommit={commit}\n')
    print(f'Validated existing tag {tag} at {commit}, reachable from default branch {default}')


def assemble(artifacts, output, tag):
    if output.exists() and any(output.iterdir()):
        raise RuntimeError(f'Refusing populated release output {output}')
    output.mkdir(parents=True, exist_ok=True)
    commit = git('rev-parse', 'HEAD')
    if git('rev-parse', f'refs/tags/{tag}^{{commit}}') != commit or tag != 'v' + version():
        raise RuntimeError('Checked-out source/tag/version identity does not agree')
    records = []
    for profile in ('windows-x64', 'linux-x86_64'):
        directory = artifacts / f'mikai-{profile}'
        evidence = directory / 'evidence'
        report = json.loads((evidence / 'verification.json').read_text(encoding='utf-8'))
        if report.get('schema') != 'mikai.ci.verification/1' or report.get('profile') != profile:
            raise RuntimeError(f'{profile}: invalid verification report')
        if report.get('status') != 'passed' or report.get('commit') != commit or report.get('optionalNativeIncluded') is not False:
            raise RuntimeError(f'{profile}: build/test/package evidence does not establish this release commit')
        expected = ([f'mikai-{version()}-windows-x64-portable.zip', f'mikai-{version()}-windows-x64-setup.exe']
                    if profile == 'windows-x64' else [f'mikai-{version()}-linux-x86_64.tar.gz'])
        indexed = {item['file']: item for item in report['artifacts']}
        for name in expected:
            path = directory / 'packages' / name
            if name not in indexed or not path.is_file() or digest(path) != indexed[name]['sha256']:
                raise RuntimeError(f'{profile}: missing or modified verified artifact {name}')
            shutil.copy2(path, output / name)
            records.append({'file': name, 'sha256': digest(path), 'profile': profile})
        with zipfile.ZipFile(output / f'mikai-{version()}-{profile}-verification.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(evidence.rglob('*')):
                if path.is_file():
                    archive.write(path, path.relative_to(evidence))
            for path in sorted((directory / 'packages').iterdir()):
                if path.is_file() and path.name not in expected:
                    archive.write(path, Path('package-evidence') / path.name)
    archive = output / f'mikai-{version()}-source.tar.gz'
    subprocess.run(['git', 'archive', '--format=tar.gz', f'--prefix=mikai-{version()}/',
                    '-o', str(archive), commit], cwd=ROOT, check=True)
    shutil.copy2(ROOT / 'packaging/third-party.json', output / 'third-party.json')
    provenance = {'schema': 'mikai.release.provenance/1', 'version': version(), 'tag': tag,
                  'commit': commit, 'tree': git('rev-parse', 'HEAD^{tree}'),
                  'repository': os.getenv('GITHUB_REPOSITORY'), 'runId': os.getenv('GITHUB_RUN_ID'),
                  'runAttempt': os.getenv('GITHUB_RUN_ATTEMPT'), 'artifacts': records,
                  'optionalNativeIncluded': False,
                  'claim': 'Workflow-recorded build inputs and verification logs; not a signed SLSA attestation.'}
    (output / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf-8')
    (output / 'RELEASE-NOTES.txt').write_text(
        f'Mikai {version()}\n\nWindows: unsigned portable ZIP and per-user setup.\n'
        'Linux: dependency-based x86_64 archive; not a standalone AppImage.\n'
        'Optional native inference components and acquired models are excluded.\n'
        'See bundled licenses, third-party.json, source archive and verification logs.\n'
        'Preparation is not publication or legal clearance.\n', encoding='utf-8')
    sums = ''.join(f'{digest(path)}  {path.name}\n' for path in sorted(output.iterdir()) if path.is_file())
    (output / 'SHA256SUMS').write_text(sums, encoding='utf-8')


def gate(directory):
    manifest = json.loads((directory / 'third-party.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 'mikai.packaging.third-party/1':
        raise RuntimeError('Unsupported packaging rights manifest')
    errors = []
    for profile in ('windows-x64', 'linux-x86_64'):
        decision = manifest.get('profiles', {}).get(profile, {})
        if decision.get('publicReleaseReady') is not True:
            errors.append(f'{profile}: ' + '; '.join(decision.get('blockers', ['No explicit redistribution clearance'])))
    provenance = json.loads((directory / 'provenance.json').read_text(encoding='utf-8'))
    if provenance.get('commit') != git('rev-parse', 'HEAD') or provenance.get('optionalNativeIncluded') is not False:
        errors.append('Release provenance commit/native exclusion mismatch')
    listed = set()
    for line in (directory / 'SHA256SUMS').read_text(encoding='utf-8').splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  ([^/\\]+)', line)
        if not match:
            raise RuntimeError('Malformed release SHA256SUMS record')
        path = directory / match.group(2)
        if not path.is_file() or digest(path) != match.group(1):
            raise RuntimeError(f'Release checksum mismatch: {path.name}')
        listed.add(path.name)
    if listed != {path.name for path in directory.iterdir() if path.is_file() and path.name != 'SHA256SUMS'}:
        errors.append('Release files do not exactly match SHA256SUMS')
    if errors:
        raise RuntimeError('PUBLICATION BLOCKED:\n' + '\n'.join(errors))
    print('Base profiles explicitly cleared; optional-native manifest blocks remain separate and excluded.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    check = sub.add_parser('validate')
    check.add_argument('--tag', required=True)
    prepare = sub.add_parser('assemble')
    prepare.add_argument('--tag', required=True)
    prepare.add_argument('--artifacts', type=Path, required=True)
    prepare.add_argument('--output', type=Path, required=True)
    publish = sub.add_parser('gate')
    publish.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'validate':
        validate(args.tag)
    elif args.command == 'assemble':
        assemble(args.artifacts.resolve(), args.output.resolve(), args.tag)
    else:
        gate(args.directory.resolve())


if __name__ == '__main__':
    main()
