"""Behavioral contract tests for the release publication boundary."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name('release.py')
SPEC = importlib.util.spec_from_file_location('mikai_release_contract_target', SCRIPT)
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class ReleaseContractTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.fixture_number = 0
        self.old_root = release.ROOT
        release.ROOT = self.root
        self.git('init', '-b', 'main')
        self.git('config', 'user.name', 'Release contract fixture')
        self.git('config', 'user.email', 'release-contract@example.invalid')
        (self.root / 'CMakeLists.txt').write_text('project(mikai VERSION 0.1.0 LANGUAGES CXX)\n', encoding='utf-8')
        (self.root / 'packaging').mkdir()
        (self.root / 'packaging/third-party.json').write_text('{}\n', encoding='utf-8')
        self.git('add', '.')
        self.git('commit', '-m', 'fixture')
        self.commit = self.git('rev-parse', 'HEAD')
        self.git('tag', 'v0.1.0')
        self.git('update-ref', 'refs/remotes/origin/main', self.commit)

    def tearDown(self):
        release.ROOT = self.old_root
        self.temp.cleanup()

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.root, text=True).strip()

    @staticmethod
    def checksum(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def make_publication(self, ready=True):
        self.fixture_number += 1
        directory = self.root / f'publication-{self.fixture_number}'
        directory.mkdir()
        (directory / 'third-party.json').write_text(json.dumps({
            'schema': 'mikai.packaging.third-party/1',
            'profiles': {name: {'publicReleaseReady': ready, 'blockers': ['not cleared']}
                         for name in ('windows-x64', 'linux-x86_64')}}), encoding='utf-8')
        (directory / 'provenance.json').write_text(json.dumps({
            'commit': self.commit, 'optionalNativeIncluded': False}), encoding='utf-8')
        (directory / 'payload.zip').write_bytes(b'package bytes')
        (directory / 'SHA256SUMS').write_text(
            f'{self.checksum(directory / "payload.zip")}  payload.zip\n'
            f'{self.checksum(directory / "third-party.json")}  third-party.json\n'
            f'{self.checksum(directory / "provenance.json")}  provenance.json\n', encoding='utf-8')
        return directory

    def make_artifact_input(self, profile='windows-x64', report_commit=None, report_profile=None):
        self.fixture_number += 1
        base = self.root / f'artifacts-{self.fixture_number}'
        directory = base / f'mikai-{profile}'
        evidence = directory / 'evidence'
        packages = directory / 'packages'
        evidence.mkdir(parents=True)
        packages.mkdir()
        names = (['mikai-0.1.0-windows-x64-portable.zip', 'mikai-0.1.0-windows-x64-setup.exe']
                 if profile == 'windows-x64' else ['mikai-0.1.0-linux-x86_64.tar.gz'])
        artifact_records = []
        for name in names:
            path = packages / name
            path.write_bytes(name.encode())
            artifact_records.append({'file': name, 'sha256': self.checksum(path)})
        (evidence / 'verification.json').write_text(json.dumps({
            'schema': 'mikai.ci.verification/1', 'profile': report_profile or profile,
            'status': 'passed', 'commit': report_commit or self.commit,
            'optionalNativeIncluded': False, 'artifacts': artifact_records}), encoding='utf-8')
        return base, packages, evidence

    def test_rights_gate_requires_literal_true_for_each_profile(self):
        for readiness in (False, None, 1, 'true'):
            with self.subTest(readiness=readiness):
                directory = self.make_publication()
                manifest = json.loads((directory / 'third-party.json').read_text(encoding='utf-8'))
                manifest['profiles']['windows-x64']['publicReleaseReady'] = readiness
                (directory / 'third-party.json').write_text(json.dumps(manifest), encoding='utf-8')
                sums = (directory / 'SHA256SUMS').read_text(encoding='utf-8')
                lines = [line for line in sums.splitlines() if not line.endswith('  third-party.json')]
                lines.append(f'{self.checksum(directory / "third-party.json")}  third-party.json')
                (directory / 'SHA256SUMS').write_text('\n'.join(lines) + '\n', encoding='utf-8')
                with self.assertRaises(RuntimeError):
                    release.gate(directory)

    def test_gate_rejects_missing_or_tampered_digest_targets(self):
        directory = self.make_publication(ready=True)
        (directory / 'payload.zip').write_bytes(b'changed package')
        with self.assertRaises(RuntimeError):
            release.gate(directory)
        directory = self.make_publication(ready=True)
        (directory / 'payload.zip').unlink()
        with self.assertRaises(RuntimeError):
            release.gate(directory)

    def test_gate_rejects_missing_digest_record(self):
        directory = self.make_publication(ready=True)
        sums = (directory / 'SHA256SUMS').read_text(encoding='utf-8')
        (directory / 'SHA256SUMS').write_text('\n'.join(line for line in sums.splitlines()
                                                            if not line.endswith('  payload.zip')) + '\n', encoding='utf-8')
        with self.assertRaises(RuntimeError):
            release.gate(directory)

    def test_assembly_rejects_missing_or_modified_package_digest(self):
        for mode in ('missing', 'modified'):
            with self.subTest(mode=mode):
                artifacts, packages, evidence = self.make_artifact_input()
                target = packages / 'mikai-0.1.0-windows-x64-portable.zip'
                if mode == 'missing':
                    target.unlink()
                else:
                    target.write_bytes(b'tampered')
                with self.assertRaises(RuntimeError):
                    release.assemble(artifacts, self.root / 'out', 'v0.1.0')

        for mode in ('missing', 'tampered'):
            with self.subTest(evidence_digest=mode):
                artifacts, _, evidence = self.make_artifact_input()
                report_path = evidence / 'verification.json'
                report = json.loads(report_path.read_text(encoding='utf-8'))
                if mode == 'missing':
                    del report['artifacts'][0]['sha256']
                else:
                    report['artifacts'][0]['sha256'] = '0' * 64
                report_path.write_text(json.dumps(report), encoding='utf-8')
                with self.assertRaises((KeyError, RuntimeError)):
                    release.assemble(artifacts, self.root / 'out', 'v0.1.0')

    def test_assembly_rejects_profile_or_source_commit_mismatch(self):
        artifacts = self.root / 'valid-artifacts'
        for profile in ('windows-x64', 'linux-x86_64'):
            fixture, _, _ = self.make_artifact_input(profile=profile)
            source = fixture / f'mikai-{profile}'
            target = artifacts / source.name
            target.parent.mkdir(parents=True, exist_ok=True)
            source.rename(target)
        release.assemble(artifacts, self.root / 'valid-out', 'v0.1.0')

        for changed_profile, changed_commit in (('linux-x86_64', self.commit),
                                                ('windows-x64', '0' * 40)):
            with self.subTest(profile=changed_profile, commit=changed_commit):
                artifacts = self.root / f'mismatch-artifacts-{self.fixture_number}'
                artifacts.mkdir()
                for profile in ('windows-x64', 'linux-x86_64'):
                    fixture, _, _ = self.make_artifact_input(profile=profile)
                    source = fixture / f'mikai-{profile}'
                    target = artifacts / source.name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    source.rename(target)
                report_path = artifacts / 'mikai-windows-x64/evidence/verification.json'
                report = json.loads(report_path.read_text(encoding='utf-8'))
                report['profile'] = changed_profile
                report['commit'] = changed_commit
                report_path.write_text(json.dumps(report), encoding='utf-8')
                with self.assertRaises(RuntimeError):
                    release.assemble(artifacts, self.root / f'out-{self.fixture_number}', 'v0.1.0')

    def test_tag_validation_rejects_bad_unreachable_and_wrong_event_tags(self):
        base_environment = {'DEFAULT_BRANCH': 'main', 'GITHUB_EVENT_NAME': 'push',
                            'GITHUB_REF': 'refs/tags/v0.1.0'}
        with mock.patch.dict(os.environ, base_environment, clear=False):
            release.validate('v0.1.0')
        for tag in ('v0.1.0; touch injected', 'v0.1', 'v0.1.0\n'):
            environment = {**base_environment, 'GITHUB_REF': f'refs/tags/{tag}'}
            with self.subTest(tag=tag), mock.patch.dict(os.environ, environment, clear=False):
                with self.assertRaises(RuntimeError):
                    release.validate(tag)
        with mock.patch.dict(os.environ, {**base_environment, 'GITHUB_REF': 'refs/tags/v0.1.1'}, clear=False):
            orphan = self.git('commit-tree', self.git('rev-parse', 'HEAD^{tree}'), '-m', 'unreachable')
            self.git('tag', 'v0.1.1', orphan)
            with self.assertRaisesRegex(RuntimeError, 'not reachable from default branch main'):
                release.validate('v0.1.1')
        with mock.patch.dict(os.environ, {**base_environment, 'GITHUB_REF': 'refs/tags/v0.1.1'}, clear=False):
            with self.assertRaises(RuntimeError):
                release.validate('v0.1.1')

        manual_environment = {'DEFAULT_BRANCH': 'main', 'GITHUB_EVENT_NAME': 'workflow_dispatch',
                              'GITHUB_REF': 'refs/heads/main'}
        with mock.patch.dict(os.environ, manual_environment, clear=False):
            release.validate('v0.1.0')


if __name__ == '__main__':
    unittest.main()
