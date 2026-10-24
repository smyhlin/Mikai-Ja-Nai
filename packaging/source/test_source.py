"""Focused regression checks for source snapshot selection and content integrity."""
import hashlib
import importlib.util
import json
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SCRIPT = Path(__file__).with_name('package.py')
SPEC = importlib.util.spec_from_file_location('mikai_source_package_integrity_target', SCRIPT)
source_package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(source_package)


class SourcePackageIntegrityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / 'repo'
        self.root.mkdir()
        self.git('init', '-q')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.git('config', 'user.name', 'Fixture')
        (self.root / '.gitignore').write_text('.env\nprivate/\nignored.bin\n', encoding='utf-8')
        (self.root / 'tracked.txt').write_text('tracked source\n', encoding='utf-8')
        (self.root / 'src').mkdir()
        (self.root / 'src' / 'production.cpp').write_text('production source\n', encoding='utf-8')
        (self.root / 'player').mkdir()
        (self.root / 'player' / 'player.cpp').write_text('player source\n', encoding='utf-8')
        native_models = self.root / 'src' / 'player' / 'native' / 'models'
        native_models.mkdir(parents=True)
        (native_models / 'catalog.cpp').write_text('native model source\n', encoding='utf-8')
        self.git('add', '.gitignore', 'tracked.txt', 'src', 'player')
        self.git('commit', '-qm', 'fixture')

    def tearDown(self):
        self.temp.cleanup()

    def git(self, *args):
        import subprocess
        return subprocess.check_output(['git', *args], cwd=self.root, text=True).strip()

    def package(self, name='out'):
        return source_package.package(self.root, self.root.parent / name)

    def archive_members(self, result):
        with tarfile.open(result / source_package.ARCHIVE_NAME, 'r:gz') as tar:
            return {
                member.name[len('mikai-0.1.0/'):]: tar.extractfile(member).read()
                for member in tar.getmembers()
                if member.isfile()
            }

    def test_provenance_hashes_describe_archived_bytes(self):
        result = self.package()
        provenance = json.loads((result / 'source-provenance.json').read_text(encoding='utf-8'))
        members = self.archive_members(result)
        by_path = {entry['path']: entry['sha256'] for entry in provenance['files']}

        self.assertEqual(set(members), set(by_path))
        for rel, data in members.items():
            self.assertEqual(hashlib.sha256(data).hexdigest(), by_path[rel])

        tree_material = json.dumps(
            provenance['files'], ensure_ascii=True, sort_keys=True, separators=(',', ':')
        ).encode('ascii')
        self.assertEqual(provenance['tree_sha256'], hashlib.sha256(tree_material).hexdigest())

    def test_same_size_change_before_archive_read_hashes_emitted_content(self):
        target = 'tracked.txt'
        original_check = source_package.checked_file
        checks = 0

        def mutate_at_archive_read(root, rel):
            nonlocal checks
            if rel == target:
                checks += 1
                if checks == 2:
                    (self.root / target).write_bytes(b'changed source\n')
            return original_check(root, rel)

        # The first check selects/stat's the candidate; the second is the archive
        # read boundary. Both payloads have equal length, so size checks alone miss it.
        with patch.object(source_package, 'checked_file', side_effect=mutate_at_archive_read):
            result = self.package()

        members = self.archive_members(result)
        provenance = json.loads((result / 'source-provenance.json').read_text(encoding='utf-8'))
        entry = next(item for item in provenance['files'] if item['path'] == target)
        self.assertEqual(members[target], b'changed source\n')
        self.assertEqual(entry['sha256'], hashlib.sha256(members[target]).hexdigest())
        self.assertEqual(checks, 2)

    def test_snapshot_excludes_private_and_ignored_files_but_keeps_production_sources(self):
        (self.root / '.env').write_text('PRIVATE=1\n', encoding='utf-8')
        (self.root / 'ignored.bin').write_bytes(b'ignored')
        (self.root / 'private').mkdir()
        (self.root / 'private' / 'notes.txt').write_text('private\n', encoding='utf-8')
        (self.root / 'untracked.txt').write_text('local addition\n', encoding='utf-8')
        (self.root / 'tracked.txt').unlink()
        result = self.package()
        names = set(self.archive_members(result))

        self.assertIn('src/production.cpp', names)
        self.assertIn('player/player.cpp', names)
        self.assertIn('src/player/native/models/catalog.cpp', names)
        self.assertIn('untracked.txt', names)
        self.assertNotIn('tracked.txt', names)
        self.assertNotIn('.env', names)
        self.assertNotIn('ignored.bin', names)
        self.assertNotIn('private/notes.txt', names)

    def test_tracked_sensitive_path_fails_closed(self):
        (self.root / '.env').write_text('PRIVATE=1\n', encoding='utf-8')
        self.git('add', '-f', '.env')
        with self.assertRaisesRegex(source_package.SourcePackageError, 'tracked sensitive-path hazard'):
            self.package()

    def test_output_must_be_fresh_and_nonoverlapping(self):
        existing = self.root.parent / 'existing'
        existing.mkdir()
        with self.assertRaisesRegex(source_package.SourcePackageError, 'already exists'):
            source_package.package(self.root, existing)
        with self.assertRaisesRegex(source_package.SourcePackageError, 'must not overlap'):
            source_package.package(self.root, self.root / 'inside')
