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


if __name__ == '__main__':
    unittest.main()
