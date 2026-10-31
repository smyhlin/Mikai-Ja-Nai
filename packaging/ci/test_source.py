"""Real temporary-Git fixtures for the local source snapshot boundary."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


SCRIPT = Path(__file__).parents[1] / 'source' / 'package.py'
SPEC = importlib.util.spec_from_file_location('mikai_source_package_target', SCRIPT)
source_package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(source_package)


class SourcePackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / 'repo'
        self.root.mkdir()
        self.git('init', '-q')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.git('config', 'user.name', 'Fixture')
        (self.root / '.gitignore').write_text('.env\nprivate/\nignored.bin\n', encoding='utf-8')
        (self.root / 'tracked.txt').write_text('tracked source\n', encoding='utf-8')
        self.git('add', '.gitignore', 'tracked.txt')
        self.git('commit', '-qm', 'fixture')

    def tearDown(self):
        self.temp.cleanup()

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.root, text=True).strip()

    def package(self, name='out'):
        return source_package.package(self.root, self.root.parent / name)

    def test_excludes_ignored_private_and_release_output_but_includes_untracked_asset(self):
        (self.root / '.env').write_text('private-token=must-not-ship\n', encoding='utf-8')
        (self.root / 'private').mkdir()
        (self.root / 'private' / 'account.json').write_text('{"private":true}\n', encoding='utf-8')
        (self.root / 'ignored.bin').write_bytes(b'ignored')
        (self.root / 'resources').mkdir()
        (self.root / 'resources' / 'new-artwork.svg').write_text('<svg/>\n', encoding='utf-8')
        # The packaged benchmark clips are build inputs of every release route, so
        # a source snapshot has to carry them even though they are binary media.
        (self.root / 'resources' / 'benchmarks').mkdir()
        (self.root / 'resources' / 'benchmarks' / '480x360.mp4').write_bytes(b'clip')
        result = self.package()
        archive = result / source_package.ARCHIVE_NAME
        with tarfile.open(archive, 'r:gz') as tar:
            names = set(tar.getnames())
        self.assertIn('mikai-0.1.0/resources/new-artwork.svg', names)
        self.assertIn('mikai-0.1.0/resources/benchmarks/480x360.mp4', names)
        self.assertNotIn('mikai-0.1.0/.env', names)
        self.assertNotIn('mikai-0.1.0/private/account.json', names)
        self.assertNotIn('mikai-0.1.0/ignored.bin', names)
        provenance = json.loads((result / 'source-provenance.json').read_text(encoding='utf-8'))
        included = {entry['path'] for entry in provenance['files']}
        self.assertEqual(included, {'.gitignore', 'tracked.txt', 'resources/new-artwork.svg',
                                    'resources/benchmarks/480x360.mp4'})
        self.assertTrue(provenance['dirty'])
        self.assertEqual(provenance['commit_equivalence'], 'not-claimed')
        self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), (result / 'SHA256SUMS').read_text().split()[0])

    def test_preserves_production_model_code_and_excludes_runtime_model_stores(self):
        files = {
            'src/player/native/models/NativeCatalog.hpp': 'production model code\n',
            'native/models/runtime-model.onnx': 'runtime model data\n',
            'models/user-model.onnx': 'user model data\n',
            'user-data/models/user-model.onnx': 'user data\n',
            'user-store/models/user-model.onnx': 'user data\n',
        }
        for rel, content in files.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding='utf-8')
        result = self.package()
        archive = result / source_package.ARCHIVE_NAME
        with tarfile.open(archive, 'r:gz') as tar:
            names = set(tar.getnames())
        self.assertIn('mikai-0.1.0/src/player/native/models/NativeCatalog.hpp', names)
        for rel in files.keys() - {'src/player/native/models/NativeCatalog.hpp'}:
            self.assertNotIn(f'mikai-0.1.0/{rel}', names)

    def test_refuses_existing_output_without_overwriting(self):
        output = self.root.parent / 'out'
        output.mkdir()
        sentinel = output / 'keep.txt'
        sentinel.write_text('preserve', encoding='utf-8')
        with self.assertRaises(source_package.SourcePackageError):
            source_package.package(self.root, output)
        self.assertEqual(sentinel.read_text(encoding='utf-8'), 'preserve')

    def test_refuses_tracked_sensitive_path(self):
        (self.root / 'credentials.json').write_text('{"token":"private"}\n', encoding='utf-8')
        self.git('add', 'credentials.json')
        self.git('commit', '-qm', 'bad fixture')
        with self.assertRaisesRegex(source_package.SourcePackageError, 'tracked sensitive-path'):
            self.package()

    def test_refuses_symlink_escape(self):
        outside = self.root.parent / 'outside.txt'
        outside.write_text('not source', encoding='utf-8')
        link = self.root / 'escape.txt'
        try:
            link.symlink_to(outside)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f'symlinks unavailable: {exc}')
        with self.assertRaisesRegex(source_package.SourcePackageError, 'symbolic link'):
            self.package()


if __name__ == '__main__':
    unittest.main()
