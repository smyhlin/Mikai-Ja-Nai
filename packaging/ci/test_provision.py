"""Filesystem regression tests for required Qt Quick SDK readiness."""
import importlib.util
from pathlib import Path
import tempfile
import unittest


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


if __name__ == '__main__':
    unittest.main()
