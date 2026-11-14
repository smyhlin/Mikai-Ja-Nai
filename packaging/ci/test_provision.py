"""Filesystem regression tests for required Qt Quick SDK readiness."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).with_name('provision.py')
SPEC = importlib.util.spec_from_file_location('mikai_ci_provision', SCRIPT)
provision = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(provision)


class QuickSdkReadinessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.prefix = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def add_required_files(self):
        for module in ('Qt6Qml', 'Qt6Quick', 'Qt6QuickControls2'):
            config = self.prefix / 'lib' / 'cmake' / module / f'{module}Config.cmake'
            config.parent.mkdir(parents=True, exist_ok=True)
            config.write_text('# installed module config\n', encoding='utf-8')
        controls = self.prefix / 'qml' / 'QtQuick' / 'Controls' / 'Basic' / 'qmldir'
        controls.parent.mkdir(parents=True, exist_ok=True)
        controls.write_text('module QtQuick.Controls.Basic\n', encoding='utf-8')

    def test_refuses_incomplete_quick_controls_sdk(self):
        self.add_required_files()
        config = self.prefix / 'lib' / 'cmake' / 'Qt6QuickControls2' / 'Qt6QuickControls2Config.cmake'
        config.unlink()

        with self.assertRaisesRegex(RuntimeError, 'Qt6QuickControls2Config.cmake'):
            provision.validate_quick_sdk(self.prefix)

    def test_refuses_sdk_without_controls_basic_qml_import(self):
        self.add_required_files()
        (self.prefix / 'qml' / 'QtQuick' / 'Controls' / 'Basic' / 'qmldir').unlink()

        with self.assertRaisesRegex(RuntimeError, 'qmldir'):
            provision.validate_quick_sdk(self.prefix)

    def test_accepts_complete_quick_sdk_layout(self):
        self.add_required_files()

        provision.validate_quick_sdk(self.prefix)

    def test_linux_install_uses_available_addons_and_validates_quick(self):
        root = self.prefix / 'sdk'
        evidence = self.prefix / 'evidence'
        self.prefix = root / '6.12.0' / 'gcc_64'
        self.add_required_files()
        config = self.prefix / 'lib' / 'cmake' / 'Qt6' / 'Qt6Config.cmake'
        config.parent.mkdir(parents=True, exist_ok=True)
        config.write_text('# installed base kit\n', encoding='utf-8')

        def aqt_repository(*command):
            install = command[command.index('install-qt'):]
            addons = install[install.index('-m') + 1:]
            if set(addons) != {'qtimageformats'}:
                raise AssertionError(f'Qt repository does not provide these addons: {addons}')
            return 'SDK installed'

        with patch.object(provision.sys, 'platform', 'linux'), \
             patch.object(provision.sys, 'argv', ['provision.py', '--root', str(root),
                                                 '--evidence', str(evidence)]), \
             patch.object(provision.shutil, 'which', return_value='/usr/bin/patchelf'), \
             patch.object(provision, 'run', side_effect=aqt_repository), \
             patch.dict(provision.os.environ, {'GITHUB_ENV': '', 'GITHUB_PATH': ''}):
            provision.main()

        self.assertTrue((evidence / 'toolchain.json').is_file())

    def test_windows_software_renderer_is_installed_in_qt_bin(self):
        root = self.prefix / 'sdk'
        evidence = self.prefix / 'evidence'
        downloads = evidence / 'downloads'
        downloads.mkdir(parents=True)
        archive = downloads / 'software-gl.7z'
        archive.write_bytes(b'pinned archive')
        entry = {
            'url': 'https://example.invalid/software-gl.7z',
            'bytes': archive.stat().st_size,
            'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
            'into': 'qt-bin',
        }

        def extract(command, **kwargs):
            destination = Path(next(arg[2:] for arg in command if arg.startswith('-o')))
            (destination / 'opengl32sw.dll').write_bytes(b'gl')

        with patch.object(provision, 'WINDOWS_SDK', (entry,)), \
             patch.object(provision, 'seven_zip', return_value='7z'), \
             patch.object(provision.subprocess, 'run', side_effect=extract):
            provision.install_windows_sdk(root, evidence)

        self.assertEqual((root / '6.12.0' / 'mingw_64' / 'bin' /
                          'opengl32sw.dll').read_bytes(), b'gl')


if __name__ == '__main__':
    unittest.main()
