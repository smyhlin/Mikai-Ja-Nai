import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest
import zipfile


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
''', cls.payload / 'libgcc_s_seh-1.dll',
                         output_import_library=cls.fixture_dir / 'libgcc_s_seh-1.dll.a')
        cls._compile_dll('pthread-runtime.cpp', r'''
extern "C" __declspec(dllexport) void pthread_runtime_fixture() {}
''', cls.payload / 'libwinpthread-1.dll',
                         output_import_library=cls.fixture_dir / 'libwinpthread-1.dll.a')
        cls._compile_dll('qtcore-importer.cpp', r'''
extern "C" __declspec(dllimport) void __cxa_call_terminate();
extern "C" __declspec(dllimport) void gcc_runtime_fixture();
extern "C" __declspec(dllimport) void pthread_runtime_fixture();
extern "C" __declspec(dllexport) void qtcore_fixture() {
    __cxa_call_terminate();
    gcc_runtime_fixture();
    pthread_runtime_fixture();
}
''', cls.payload / 'Qt6Core.dll',
                         output_import_library=cls.fixture_dir / 'libQt6Core.dll.a',
                         link_import_libraries=[
                             cls.compatible_runtime_dir / 'libstdc++-6.dll.a',
                             cls.fixture_dir / 'libgcc_s_seh-1.dll.a',
                             cls.fixture_dir / 'libwinpthread-1.dll.a'])
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
                     link_import_libraries=()):
        source_path = cls.fixture_dir / source_name
        source_path.write_text(source, encoding='utf-8')
        command = [os.environ['MIKAI_TEST_CXX'], '-shared', '-o', str(output)]
        if output_import_library:
            command.append(f'-Wl,--out-implib,{output_import_library}')
        command.append(str(source_path))
        command.extend(str(library) for library in link_import_libraries)
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

    def test_package_script_selects_first_duplicate_tool_and_creates_real_artifacts(self):
        cmake = shutil.which('cmake')
        iscc = shutil.which('ISCC.exe') or shutil.which('iscc.exe')
        powershell = shutil.which('pwsh') or shutil.which('powershell')
        ninja = shutil.which('ninja')
        missing = [name for name, path in (
            ('cmake', cmake), ('Inno Setup ISCC.exe', iscc),
            ('PowerShell', powershell), ('Ninja', ninja),
        ) if path is None]
        if missing:
            raise RuntimeError('Windows package regression requires ' + ', '.join(missing) + ' on PATH')
        compiler = pathlib.Path(os.environ['MIKAI_TEST_CXX']).resolve()
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            build = root / 'build'
            build.mkdir()
            output = root / 'artifacts'
            output.mkdir()
            poison = root / 'poison'
            poison.mkdir()
            poison_source = root / 'poison.cpp'
            poison_source.write_text('int main() { return 73; }\n', encoding='utf-8')
            compiled = subprocess.run(
                [str(compiler), str(poison_source), '-o', str(poison / 'cmake.exe')],
                check=False, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            shutil.copy2(poison / 'cmake.exe', poison / 'ISCC.exe')

            project = root / 'fixture'
            project.mkdir()
            (project / 'main.cpp').write_text('int main() { return 0; }\n', encoding='utf-8')
            (project / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.21)\n'
                'project(package_regression LANGUAGES CXX)\n'
                'add_executable(mikai main.cpp)\n',
                encoding='utf-8')
            configured = subprocess.run(
                [cmake, '-S', str(project), '-B', str(build), '-G', 'Ninja',
                 '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_COMPILER=' + str(compiler),
                 '-DCMAKE_MAKE_PROGRAM=' + ninja],
                check=False, capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)

            metadata = {
                'configuration': 'Release',
                'sourceDir': str(ROOT),
                'version': '0.0.0',
            }
            (build / 'mikai-package-Release.json').write_text(
                json.dumps(metadata), encoding='utf-8')
            stage = build / 'mikai-package-Release.cmake'
            stage.write_text(
                'file(MAKE_DIRECTORY "${DIST_DIR}")\n'
                f'file(COPY "{(build / "mikai.exe").as_posix()}" DESTINATION "${{DIST_DIR}}")\n'
                f'file(COPY "{(ROOT / "LICENSE").as_posix()}" DESTINATION "${{DIST_DIR}}")\n'
                'file(WRITE "${DIST_DIR}/VERSION" "mikai 0.0.0")\n',
                encoding='utf-8')

            env = os.environ.copy()
            env['PATH'] = os.pathsep.join((
                str(pathlib.Path(cmake).resolve().parent),
                str(pathlib.Path(iscc).resolve().parent),
                str(poison),
                str(compiler.parent),
                env.get('PATH', ''),
            ))
            packaged = subprocess.run(
                [powershell, '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                 '-File', str(ROOT / 'packaging' / 'windows' / 'package.ps1'),
                 '-BuildDir', str(build), '-OutputDir', str(output)],
                check=False, capture_output=True, text=True, env=env)
            self.assertEqual(packaged.returncode, 0, packaged.stdout + packaged.stderr)

            base = 'mikai-0.0.0-windows-x64'
            zip_path = output / (base + '-portable.zip')
            setup_path = output / (base + '-setup.exe')
            receipt_path = output / (base + '-release.json')
            self.assertEqual(setup_path.read_bytes()[:2], b'MZ')
            receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
            artifacts = {entry['name']: entry for entry in receipt['artifacts']}
            for artifact in (zip_path, setup_path):
                entry = artifacts[artifact.name]
                digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
                self.assertEqual(entry['sha256'], digest)
                self.assertEqual(entry['bytes'], artifact.stat().st_size)
            inventory = {entry['path']: entry for entry in receipt['payload']}
            exe_bytes = (build / 'mikai.exe').read_bytes()
            self.assertIn('mikai.exe', inventory)
            self.assertEqual(inventory['mikai.exe']['sha256'],
                             hashlib.sha256(exe_bytes).hexdigest())
            with zipfile.ZipFile(zip_path) as archive:
                packaged_names = {
                    name[len(base) + 1:] for name in archive.namelist()
                    if name.startswith(base + '/') and not name.endswith('/')
                }
                self.assertEqual(packaged_names, set(inventory))
                for relative, entry in inventory.items():
                    packaged = archive.read(base + '/' + relative)
                    self.assertEqual(entry['bytes'], len(packaged), relative)
                    self.assertEqual(entry['sha256'], hashlib.sha256(packaged).hexdigest(), relative)
                packaged_exe = archive.read(base + '/mikai.exe')
                self.assertEqual(packaged_exe, exe_bytes)

    def test_dist_closes_real_pe_dependencies_from_compiler_bin(self):
        required_tools = {
            'cmake': shutil.which('cmake'),
            'objdump': shutil.which('objdump'),
        }
        missing = [name for name, path in required_tools.items() if path is None]
        if missing:
            self.skipTest('Windows dist closure test requires ' + ', '.join(missing))
        cmake = pathlib.Path(required_tools['cmake']).resolve()
        objdump = pathlib.Path(required_tools['objdump']).resolve()
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            compiler_bin = root / 'compiler' / 'bin'
            compiler_bin.mkdir(parents=True)
            compiler_marker = compiler_bin / 'g++.exe'
            compiler_marker.write_bytes(b'fixture compiler metadata')
            for name in ('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll'):
                shutil.copy2(self.payload / name, compiler_bin / name)

            qt = root / 'qt'
            qt_bin = qt / 'bin'
            qt_bin.mkdir(parents=True)
            modules = (
                'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll',
                'Qt6OpenGL.dll', 'Qt6OpenGLWidgets.dll', 'Qt6Qml.dll', 'Qt6QmlCore.dll',
                'Qt6QmlModels.dll', 'Qt6QmlWorkerScript.dll', 'Qt6Quick.dll',
                'Qt6QuickControls2.dll', 'Qt6QuickControls2Basic.dll',
                'Qt6QuickControls2BasicStyleImpl.dll', 'Qt6QuickTemplates2.dll',
                'Qt6QuickLayouts.dll',
            )
            for name in modules:
                shutil.copy2(self.payload / 'Qt6Core.dll', qt_bin / name)
            deploy = qt_bin / 'windeployqt.cmd'
            deploy.write_text('@echo off\\r\\nexit /b 0\\r\\n', encoding='ascii')

            for relative in self.assembler.load_qml_imports():
                target = qt / pathlib.Path(relative)
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.suffix.lower() == '.dll':
                    shutil.copy2(self.fixture_dir / 'Qt6Core-no-import.dll', target)
                else:
                    target.write_text('fixture\\n', encoding='utf-8')
            manifest = root / self.assembler.QML_MANIFEST_NAME
            shutil.copy2(ROOT / 'packaging' / 'windows' / self.assembler.QML_MANIFEST_NAME,
                         manifest)

            for relative in ('platforms/qwindows.dll', 'imageformats/qjpeg.dll',
                             'imageformats/qwebp.dll', 'tls/qschannelbackend.dll'):
                target = qt / 'plugins' / pathlib.Path(relative)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(self.fixture_dir / 'Qt6Core-no-import.dll', target)

            source = root / 'source'
            benchmarks = source / 'resources' / 'benchmarks'
            benchmarks.mkdir(parents=True)
            for name in self.assembler.BENCHMARKS:
                (benchmarks / name).write_bytes(b'benchmark fixture')
            (source / 'LICENSE').write_text('Fixture license material.\\n', encoding='utf-8')
            (source / 'packaging' / 'licenses').mkdir(parents=True)
            (source / 'packaging' / 'third-party.json').write_text('{}\\n', encoding='utf-8')
            (source / 'packaging' / 'licenses' / 'fixture.txt').write_text(
                'Fixture license material.\\n', encoding='utf-8')

            shaders = root / 'shaders'
            shaders.mkdir()
            (shaders / 'fixture.glsl').write_text('// fixture\\n', encoding='utf-8')
            qml_scan = root / 'qml-source'
            qml_scan.mkdir()
            executable = root / 'mikai.exe'
            shutil.copy2(self.fixture_dir / 'Qt6Core-no-import.dll', executable)

            archive = root / 'fixture-mpv.zip'
            with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_STORED) as bundle:
                fixture_pe = (self.fixture_dir / 'Qt6Core-no-import.dll').read_bytes()
                bundle.writestr('libmpv-2.dll', fixture_pe)
                bundle.writestr('ajn-mvtools.dll', fixture_pe)
                bundle.writestr('licenses/mvtools/LICENSE', 'Fixture license material.\\n')
                bundle.writestr('licenses/mvtools/UPSTREAM.md', 'Fixture upstream metadata.\\n')
            archive_sha256 = hashlib.sha256(archive.read_bytes()).hexdigest()

            dist = root / 'dist'
            cache = root / 'cache'
            cache.mkdir()
            system_root = os.environ.get('SystemRoot')
            if not system_root:
                self.skipTest('Windows dist closure test requires SystemRoot')
            environment = os.environ.copy()
            system_paths = [str(pathlib.Path(system_root) / 'System32')]
            if environment.get('ComSpec'):
                system_paths.append(str(pathlib.Path(environment['ComSpec']).parent))
            environment['PATH'] = os.pathsep.join(system_paths)
            arguments = [
                str(cmake), '-DDIST_DIR=' + str(dist), '-DEXE=' + str(executable),
                '-DWINDEPLOYQT=' + str(deploy), '-DSHADERS=' + str(shaders),
                '-DSOURCE_DIR=' + str(source), '-DMIKAI_QML_SCAN_DIR=' + str(qml_scan),
                '-DMIKAI_QML_IMPORT_MANIFEST=' + str(manifest), '-DMIKAI_QT_VERSION=6.12.0',
                '-DMIKAI_MPV_ARCHIVE_NAME=fixture-mpv.zip',
                '-DMIKAI_MPV_ARCHIVE_URL=https://invalid.example/fixture-mpv.zip',
                '-DMIKAI_MPV_ARCHIVE_SHA256=' + archive_sha256,
                '-DMIKAI_DIST_CACHE_DIR=' + str(cache),
                '-DMIKAI_DIST_MPV_ARCHIVE=' + str(archive),
                '-DMIKAI_PUBLIC_RELEASE=ON',
                '-DMIKAI_RELEASE_COMPILER_BIN=' + str(compiler_marker),
                '-DMIKAI_RELEASE_OBJDUMP=' + str(objdump),
                '-DMIKAI_RELEASE_VERSION=0.0.0', '-DMIKAI_RELEASE_QT_VERSION=6.12.0',
                '-DMIKAI_RELEASE_COMPILER=fixture-mingw',
                '-P', str(ROOT / 'cmake' / 'dist.cmake'),
            ]
            result = subprocess.run(
                arguments, check=False, capture_output=True, text=True, env=environment,
                cwd=root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            inventory = (dist / 'PACKAGE-CONTENTS.sha256').read_text(encoding='utf-8')
            for name in ('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll'):
                staged = dist / name
                fixture_hash = hashlib.sha256((compiler_bin / name).read_bytes()).hexdigest()
                self.assertEqual(staged.read_bytes(), (compiler_bin / name).read_bytes())
                self.assertIn(f'{fixture_hash}  {name}', inventory.splitlines())


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
