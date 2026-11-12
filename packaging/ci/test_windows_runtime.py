import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'packaging' / 'windows' / 'assemble-payload.py'


def load_assembler():
    import importlib.util

    spec = importlib.util.spec_from_file_location('assemble_payload', SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class WindowsRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get('MIKAI_TEST_CXX')
        if not compiler or not pathlib.Path(compiler).is_file():
            raise RuntimeError(
                'Windows runtime ABI tests require MIKAI_TEST_CXX to name the configured MinGW C++ compiler')
        cls.assembler = load_assembler()
        cls.temp = tempfile.TemporaryDirectory()
        cls.fixture_dir = pathlib.Path(cls.temp.name)
        cls.payload = cls.fixture_dir / 'payload'
        cls.payload.mkdir()

        cls.compatible_runtime_dir = cls.fixture_dir / 'compatible-runtime'
        cls.compatible_runtime_dir.mkdir()
        cls._compile_dll('compatible-runtime.cpp', r'''
extern "C" __declspec(dllexport) void __cxa_call_terminate() {}
''', cls.compatible_runtime_dir / 'LiBsTdC++-6.DlL',
                         output_import_library=cls.compatible_runtime_dir / 'libstdc++-6.dll.a')
        shutil.copy2(cls.compatible_runtime_dir / 'LiBsTdC++-6.DlL',
                     cls.payload / 'libstdc++-6.dll')
        cls._compile_dll('gcc-runtime.cpp', r'''
extern "C" __declspec(dllexport) void gcc_runtime_fixture() {}
''', cls.payload / 'libgcc_s_seh-1.dll')
        cls._compile_dll('pthread-runtime.cpp', r'''
extern "C" __declspec(dllexport) void pthread_runtime_fixture() {}
''', cls.payload / 'libwinpthread-1.dll')
        cls._compile_dll('qtcore-importer.cpp', r'''
extern "C" __declspec(dllimport) void __cxa_call_terminate();
extern "C" __declspec(dllexport) void qtcore_fixture() { __cxa_call_terminate(); }
''', cls.payload / 'Qt6Core.dll',
                         link_import_library=cls.compatible_runtime_dir / 'libstdc++-6.dll.a')
        cls.bad_runtime_dir = cls.fixture_dir / 'bad-runtime'
        cls.bad_runtime_dir.mkdir()
        cls._compile_dll('old-runtime.cpp', r'''
extern "C" __declspec(dllexport) void unrelated_runtime_fixture() {}
''', cls.bad_runtime_dir / 'libstdc++-6.dll')
        cls._compile_dll('qtcore-no-import.cpp', r'''
extern "C" __declspec(dllexport) void qtcore_fixture() {}
''', cls.fixture_dir / 'Qt6Core-no-import.dll')

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @classmethod
    def _compile_dll(cls, source_name, source, output, output_import_library=None,
                     link_import_library=None):
        source_path = cls.fixture_dir / source_name
        source_path.write_text(source, encoding='utf-8')
        command = [os.environ['MIKAI_TEST_CXX'], '-shared', '-o', str(output)]
        if output_import_library:
            command.append(f'-Wl,--out-implib,{output_import_library}')
        command.append(str(source_path))
        if link_import_library:
            command.append(str(link_import_library))
        subprocess.run(command, check=True, capture_output=True, text=True)

    def _payload_copy(self):
        payload = self.fixture_dir / ('case-' + self.id().rsplit('.', 1)[-1])
        payload.mkdir()
        for name in ('Qt6Core.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll'):
            shutil.copy2(self.payload / name, payload / name)
        return payload

    def test_accepts_runtime_export_used_by_qtcore(self):
        payload = self._payload_copy()
        output = subprocess.run(
            ['objdump', '-p', str(payload / 'Qt6Core.dll')],
            check=True, capture_output=True, text=True).stdout
        self.assertIn('DLL Name: LiBsTdC++-6.DlL', output)
        self.assembler.validate_qt_mingw_runtime(payload)

    def test_rejects_runtime_missing_qtcore_import(self):
        payload = self._payload_copy()
        shutil.copy2(self.bad_runtime_dir / 'libstdc++-6.dll', payload / 'libstdc++-6.dll')
        with self.assertRaisesRegex(SystemExit, 'does not export it'):
            self.assembler.validate_qt_mingw_runtime(payload)

    def test_does_not_require_unused_runtime_symbol(self):
        payload = self._payload_copy()
        shutil.copy2(self.fixture_dir / 'Qt6Core-no-import.dll', payload / 'Qt6Core.dll')
        shutil.copy2(self.bad_runtime_dir / 'libstdc++-6.dll', payload / 'libstdc++-6.dll')
        self.assembler.validate_qt_mingw_runtime(payload)


class BenchmarkPayloadTests(unittest.TestCase):
    """The generated benchmark clips are installed payload, not build leftovers."""

    @classmethod
    def setUpClass(cls):
        cls.assembler = load_assembler()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.source = self.root / 'source'
        self.benchmarks = self.source / 'resources' / 'benchmarks'
        self.benchmarks.mkdir(parents=True)
        self.out = self.root / 'payload'
        self.out.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def write_clips(self, names):
        for name in names:
            (self.benchmarks / name).write_bytes(name.encode('utf-8'))

    def test_copies_every_clip_to_the_installed_directory(self):
        self.write_clips(self.assembler.BENCHMARKS)
        self.assembler.copy_benchmarks(self.source, self.out)
        installed = self.out / 'benchmarks'
        self.assertEqual(sorted(path.name for path in installed.iterdir()),
                         sorted(self.assembler.BENCHMARKS))
        for name in self.assembler.BENCHMARKS:
            self.assertEqual((installed / name).read_bytes(), name.encode('utf-8'))

    def test_refuses_a_missing_clip(self):
        missing = self.assembler.BENCHMARKS[-1]
        self.write_clips(self.assembler.BENCHMARKS[:-1])
        with self.assertRaises(SystemExit) as raised:
            self.assembler.copy_benchmarks(self.source, self.out)
        self.assertIn(missing, str(raised.exception))

    def test_refuses_an_empty_clip(self):
        self.write_clips(self.assembler.BENCHMARKS)
        (self.benchmarks / self.assembler.BENCHMARKS[0]).write_bytes(b'')
        with self.assertRaises(SystemExit) as raised:
            self.assembler.copy_benchmarks(self.source, self.out)
        self.assertIn(self.assembler.BENCHMARKS[0], str(raised.exception))

    def test_refuses_a_missing_clip_directory(self):
        shutil.rmtree(self.benchmarks)
        with self.assertRaises(SystemExit) as raised:
            self.assembler.copy_benchmarks(self.source, self.out)
        self.assertIn('benchmark clip directory is missing', str(raised.exception))


class QmlPayloadTests(unittest.TestCase):
    """QML modules are named payload entries; a missing import is a packaging error."""

    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get('MIKAI_TEST_CXX')
        if not compiler or not pathlib.Path(compiler).is_file():
            raise RuntimeError(
                'Windows QML payload tests require MIKAI_TEST_CXX to name the configured MinGW C++ compiler')
        cls.assembler = load_assembler()
        cls.temp = tempfile.TemporaryDirectory()
        cls.fixture_dir = pathlib.Path(cls.temp.name)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def _case(self):
        case = self.fixture_dir / ('qml-case-' + self.id().rsplit('.', 1)[-1])
        case.mkdir()
        return case

    def _qt_root_with_qml(self, case, files):
        qt_root = case / 'qt'
        for relative in files:
            target = qt_root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b'fixture')
        return qt_root

    def _compile_dll(self, case, source_name, source, output, import_library=None,
                     link_library=None):
        source_path = case / source_name
        source_path.write_text(source, encoding='utf-8')
        command = [os.environ['MIKAI_TEST_CXX'], '-shared', '-o', str(output)]
        if import_library:
            command.append(f'-Wl,--out-implib,{import_library}')
        command.append(str(source_path))
        if link_library:
            command.append(str(link_library))
        subprocess.run(command, check=True, capture_output=True, text=True)

    def test_names_a_missing_qml_import(self):
        case = self._case()
        qt_root = self._qt_root_with_qml(case, ['qml/QtQuick/qmldir'])
        out = case / 'payload'
        out.mkdir()
        with self.assertRaises(SystemExit) as raised:
            self.assembler.copy_qml_imports(
                qt_root, out, ['qml/QtQuick/qmldir', 'qml/QtQml/qmldir'])
        self.assertIn('QML imports missing from the Qt SDK:', str(raised.exception))
        self.assertIn('qml/QtQml/qmldir', str(raised.exception))

    def test_reviewed_manifest_includes_player_qml_imports(self):
        imports = set(self.assembler.load_qml_imports())
        self.assertTrue({
            'qml/QtQuick/qmldir',
            'qml/QtQuick/qtquick2plugin.dll',
            'qml/QtQuick/Layouts/qmldir',
            'qml/QtQuick/Layouts/qquicklayoutsplugin.dll',
            'qml/QtQuick/Controls/qmldir',
            'qml/QtQuick/Controls/Basic/qmldir',
            'qml/QtQuick/Controls/Basic/qtquickcontrols2basicstyleplugin.dll',
        }.issubset(imports))

    def _write_manifest(self, case, files, schema='mikai.qt.qml-imports/1'):
        manifest = case / self.assembler.QML_MANIFEST_NAME
        manifest.write_text(json.dumps({
            'schema': schema,
            'qtVersion': '6.12.0',
            'files': files,
        }), encoding='utf-8')
        return manifest

    def test_refuses_a_manifest_entry_outside_the_qml_tree(self):
        manifest = self._write_manifest(self._case(), ['qml/../escape/qmldir'])
        with self.assertRaises(ValueError):
            self.assembler.load_qml_imports(manifest)

    def test_refuses_a_manifest_with_an_unknown_schema(self):
        manifest = self._write_manifest(
            self._case(), ['qml/QtQuick/qmldir'], schema='mikai.qt.qml-imports/2')
        with self.assertRaises(ValueError):
            self.assembler.load_qml_imports(manifest)

    def test_closes_the_dependency_chain_of_copied_qml_plugins(self):
        case = self._case()
        qt_bin = case / 'qt' / 'bin'
        qt_bin.mkdir(parents=True)
        payload = case / 'payload'
        payload.mkdir()
        quick_library = qt_bin / 'Qt6Quick.dll.a'
        self._compile_dll(
            case, 'quick.cpp',
            'extern "C" __declspec(dllexport) void quick_fixture() {}',
            qt_bin / 'Qt6Quick.dll', import_library=quick_library)
        qml_library = qt_bin / 'Qt6Qml.dll.a'
        self._compile_dll(
            case, 'qml.cpp',
            'extern "C" __declspec(dllimport) void quick_fixture();\n'
            'extern "C" __declspec(dllexport) void qml_fixture() { quick_fixture(); }',
            qt_bin / 'Qt6Qml.dll', import_library=qml_library, link_library=quick_library)
        plugin = payload / 'qml' / 'QtQuick' / 'qtquick2plugin.dll'
        plugin.parent.mkdir(parents=True)
        self._compile_dll(
            case, 'qml-plugin.cpp',
            'extern "C" __declspec(dllimport) void qml_fixture();\n'
            'extern "C" __declspec(dllexport) void plugin_fixture() { qml_fixture(); }',
            plugin, link_library=qml_library)
        self.assembler.close_imports(payload, [str(qt_bin)], [str(plugin)])
        self.assertTrue((payload / 'Qt6Qml.dll').is_file())
        self.assertTrue((payload / 'Qt6Quick.dll').is_file())


if __name__ == '__main__':
    unittest.main()
