"""Build, hermetic CTest, isolated renderer proof, then package (no publication)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
LINUX_MPV_ARCHIVE_NAME = 'mpv-linux-x64-2026-10-24-c493f1c-tls.tar.zst'


def ensure_linux_runtime():
    """Return the path of the bundled Linux runtime, building it when absent.

    The shipped runtime is the TLS rebuild, not the archive published upstream;
    configure refuses to bundle anything else, so CI cannot simply download it.
    """
    archive = ROOT / 'release-artifacts' / LINUX_MPV_ARCHIVE_NAME
    if archive.is_file():
        return archive
    if shutil.which('docker') is None:
        raise RuntimeError(
            f'{archive} is missing and docker is unavailable to build it; '
            'run packaging/mpv/build-tls-archive.sh first')
    subprocess.run(['bash', str(ROOT / 'packaging/mpv/build-tls-archive.sh'),
                    '--output', str(archive.parent)], check=True)
    if not archive.is_file():
        raise RuntimeError(f'The runtime build did not produce {archive}')
    return archive


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--qt-prefix', type=Path, default=os.getenv('MIKAI_QT_PREFIX'))
    args = parser.parse_args()
    if not args.qt_prefix:
        parser.error('--qt-prefix or MIKAI_QT_PREFIX is required')
    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    if build.exists() and any(build.iterdir()):
        raise RuntimeError(f'Refusing populated directory: {build}; supply a fresh CI-owned directory')
    build.mkdir(parents=True, exist_ok=True)
    if output.exists() and any(output.iterdir()):
        raise RuntimeError(f'Refusing populated directory: {output}; supply a fresh CI-owned directory')
    # The final package output is owned and created by package.ps1/Package.cmake.
    # Precreating it here made Linux packaging fail its fresh-output guard.
    output.parent.mkdir(parents=True, exist_ok=True)
    qt = args.qt_prefix.resolve()
    windows = sys.platform == 'win32'
    profile = 'windows-x64' if windows else 'linux-x86_64'
    suffix = '.exe' if windows else ''
    env = os.environ.copy()
    for key in ('MIKAI_API_KEY', 'MIKAI_PROBE_BUNDLE', 'MIKAI_MPV_MODULE', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH'):
        env.pop(key, None)
    env.update(MIKAI_LIVE='0', QT_QPA_PLATFORM='offscreen', QT_PLUGIN_PATH=str(qt / 'plugins'),
               QT_QPA_PLATFORM_PLUGIN_PATH=str(qt / 'plugins' / 'platforms'))
    env['PATH'] = str(qt / 'bin') + os.pathsep + env['PATH']
    scratch = build / '_ci-state'
    scratch.mkdir()
    for key, child in (('HOME', 'home'), ('USERPROFILE', 'home'), ('APPDATA', 'roaming'),
                       ('LOCALAPPDATA', 'local'), ('XDG_CONFIG_HOME', 'config'),
                       ('XDG_CACHE_HOME', 'cache'), ('XDG_DATA_HOME', 'data')):
        directory = scratch / child
        directory.mkdir(exist_ok=True)
        env[key] = str(directory)
    results = []

    def run(name, command, *, cwd=ROOT, timeout=1800, allowed=(0,), extra=None):
        command = [str(item) for item in command]
        print('+', subprocess.list2cmdline(command), flush=True)
        started = time.monotonic()
        process_env = dict(env, **(extra or {}))
        with (evidence / f'{name}.log').open('w', encoding='utf-8') as log:
            log.write(subprocess.list2cmdline(command) + '\n')
            log.flush()
            try:
                result = subprocess.run(command, cwd=cwd, env=process_env, stdout=log,
                                        stderr=subprocess.STDOUT, timeout=timeout, check=False)
            except subprocess.TimeoutExpired as error:
                results.append({'step': name, 'status': 'timeout', 'seconds': timeout})
                raise RuntimeError(f'{name} timed out after {timeout}s; see {evidence / (name + ".log")}') from error
        results.append({'step': name, 'exitCode': result.returncode,
                        'seconds': round(time.monotonic() - started, 2)})
        print((evidence / f'{name}.log').read_text(encoding='utf-8', errors='replace'), flush=True)
        if result.returncode not in allowed:
            raise RuntimeError(f'{name} failed with exit {result.returncode}; see {evidence / (name + ".log")}')
        return result.returncode

    verdict = {'schema': 'mikai.ci.verification/1', 'profile': profile, 'status': 'failed',
               'optionalNativeIncluded': False, 'steps': results}
    try:
        run('cmake-version', ['cmake', '--version'])
        run('compiler-version', [os.getenv('MIKAI_CXX', 'g++'), '--version'])
        run('qt-version', [qt / 'bin' / ('qmake.exe' if windows else 'qmake'), '-query'])
        run('python-dependencies', [sys.executable, '-m', 'pip', 'freeze'])
        run('release-contracts', [sys.executable, '-m', 'unittest', 'discover',
                                  '-s', ROOT / 'packaging/ci', '-p', 'test_release.py'])
        run('translation-contracts', [sys.executable, '-m', 'unittest', 'discover',
                                      '-s', ROOT / 'packaging/ci', '-p', 'test_translations.py'])
        if not windows:
            run('system-dependencies', ['dpkg-query', '-W', '-f=${binary:Package}\t${Version}\n'])
        configure = ['cmake', '-S', ROOT, '-B', build, '-G', 'Ninja',
                     '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_PREFIX_PATH={qt}',
                     f'-DCMAKE_CXX_COMPILER={os.getenv("MIKAI_CXX", "g++")}',
                     '-DMIKAI_BUILD_PROBES=ON', '-DMIKAI_WITH_NATIVE=OFF',
                     '-DMIKAI_WITH_MODEL_INSPECTION=OFF']
        if not windows:
            runtime = ensure_linux_runtime()
            configure.extend(['-DMIKAI_LINUX_BUNDLE_MPV=ON',
                              '-DMIKAI_LINUX_DEPLOY_QT=ON',
                              f'-DMIKAI_LINUX_MPV_ARCHIVE={runtime}'])
        if windows:
            compiler = Path(shutil.which(os.getenv('MIKAI_CXX', 'g++')) or os.getenv('MIKAI_CXX', 'g++'))
            pe = compiler.parent / 'libwinpthread-1.dll'
            if not pe.is_file():
                raise RuntimeError(f'MinGW PE fixture missing: {pe}')
            configure.append(f'-DMIKAI_DIST_CONTRACT_PE_SOURCE={pe}')
        run('configure', configure)
        shutil.copy2(build / 'CMakeCache.txt', evidence / 'CMakeCache.txt')
        run('build', ['cmake', '--build', build, '--parallel', '2', '--target', 'mikai',
                      'mikai_tests', 'mikai_launcher_child', 'mpv_surface_smoke',
                      'mikai_manifest_check', 'check_translations'], timeout=3600)
        # T-417: the label-excluded entry is the one-shot mikai_tests blob,
        # which a hard crash in an early suite would exit without ever running
        # the rest. The per-suite mikai_tests_<suite> entries carry the same
        # hermetic launcher and properties, so CI still exercises every suite -
        # now with one ctest result per suite instead of one for all of them.
        # Running both would merely execute every suite twice.
        run('ctest', ['ctest', '--test-dir', build, '--output-on-failure', '--no-tests=error',
                      '--timeout', '900', '--label-exclude', 'mikai_tests_blob',
                      '--output-junit', evidence / 'ctest.xml'], timeout=2400)
        # Exit 2 is missing externally-owned source roots; exit 3 remains an
        # actual redistribution block, not a successful native verification.
        code = run('optional-native-manifest', [build / ('mikai_manifest_check' + suffix),
                   '--sources', ROOT / 'native/manifest/sources.json',
                   '--components', ROOT / 'native/manifest/components.json'], allowed=(0, 2, 3))
        verdict['optionalNativeManifest'] = {'exitCode': code, 'status': {0: 'manifest-check-only',
                                             2: 'source-integrity-not-established',
                                             3: 'redistribution-blocked'}[code],
                                             'nativeRuntimeVerified': False}
        smoke = scratch / 'renderer'
        smoke.mkdir()
        executable = smoke / ('mpv_surface_smoke' + suffix)
        shutil.copy2(build / executable.name, executable)
        shutil.copytree(ROOT / 'shaders', smoke / 'shaders')
        runtime_receipt = build / 'mikai-runtime-Release.json'
        if not runtime_receipt.is_file():
            raise RuntimeError(f'CMake runtime receipt missing: {runtime_receipt}')
        runtime = json.loads(runtime_receipt.read_text(encoding='utf-8'))
        if (runtime.get('schema') != 'mikai.runtime.bundle/1'
                or runtime.get('platform') != profile
                or not isinstance(runtime.get('bundled'), bool)):
            raise RuntimeError(f'Invalid CMake runtime receipt: {runtime_receipt}')
        if runtime['bundled'] != (not windows):
            raise RuntimeError(f'Unexpected bundled mpv mode for {profile}: {runtime["bundled"]}')
        module_value = runtime.get('module')
        if not isinstance(module_value, str) or not Path(module_value).is_absolute():
            raise RuntimeError(f'Runtime receipt module must be an absolute path: {runtime_receipt}')
        module = Path(module_value)
        if not module.is_file():
            raise RuntimeError(f'Selected mpv runtime module missing: {module}')
        mpv_dir = smoke / 'mpv'
        mpv_dir.mkdir()
        shutil.copy2(module, mpv_dir / ('libmpv-2.dll' if windows else 'libmpv.so.2'))
        if runtime['bundled']:
            if windows:
                raise RuntimeError('Windows CI runtime receipt unexpectedly enables private bundling')
            dependency_root_value = runtime.get('dependencyRoot')
            if not isinstance(dependency_root_value, str) or not Path(dependency_root_value).is_absolute():
                raise RuntimeError('Bundled runtime receipt has no absolute dependencyRoot')
            dependency_root = Path(dependency_root_value)
            if not dependency_root.is_dir():
                raise RuntimeError(f'Private mpv dependency root missing: {dependency_root}')
            private_libraries = sorted(path for path in dependency_root.glob('*.so*')
                                       if path.is_file())
            if not private_libraries:
                raise RuntimeError(f'Private mpv dependency closure is empty: {dependency_root}')
            for library in private_libraries:
                if library.resolve() != module.resolve():
                    shutil.copy2(library, mpv_dir / library.name)
        command = [executable, 'av://lavfi:testsrc2=size=640x360:rate=25']
        software = {'QT_QPA_PLATFORM': 'windows' if windows else 'xcb',
                    'LIBGL_ALWAYS_SOFTWARE': '1', 'QT_OPENGL': 'software'}
        if not windows:
            command = ['xvfb-run', '-a', '-s', '-screen 0 1280x800x24 +extension GLX', *command]
        run('renderer-smoke', command, cwd=smoke, timeout=120, extra=software)
        picture = smoke / 'mpv_surface_smoke.png'
        if not picture.is_file():
            raise RuntimeError('Renderer exited without its required framebuffer evidence')
        shutil.copy2(picture, evidence / picture.name)
        if windows:
            run('package', ['pwsh', '-NoProfile', '-File', ROOT / 'packaging/windows/package.ps1',
                            '-BuildDir', build, '-OutputDir', output], timeout=1800)
            if os.getenv('GITHUB_ACTIONS', '').lower() == 'true':
                setup = output / 'mikai-0.1.0-windows-x64-setup.exe'
                if not setup.is_file():
                    raise RuntimeError(f'Installer smoke input missing: {setup}')
                smoke_output = evidence / 'installer-smoke'
                run('installer-smoke', ['pwsh', '-NoProfile', '-File',
                    ROOT / 'packaging/windows/smoke.ps1', '-Installer', setup,
                    '-OutputDir', smoke_output], timeout=900)
                smoke_evidence = smoke_output / 'installer-smoke.json'
                if not smoke_evidence.is_file():
                    raise RuntimeError('Installer smoke exited without JSON evidence')
                smoke_result = json.loads(smoke_evidence.read_text(encoding='utf-8'))
                if smoke_result.get('status') != 'passed':
                    raise RuntimeError('Installer smoke evidence did not report passed')
                verdict['installerSmoke'] = {'status': 'passed',
                    'evidence': 'installer-smoke/installer-smoke.json',
                    'installerSha256': smoke_result.get('installerSha256')}
            else:
                verdict['installerSmoke'] = {'status': 'skipped',
                    'reason': 'Requires a fresh hosted Windows account; local/user-account installer actions are not run.'}
        else:
            run('package', ['bash', ROOT / 'packaging/linux/package.sh', '--build-dir', build,
                            '--output-dir', output, '--config', 'Release'], timeout=1800)
        files = [{'file': path.name, 'sha256': digest(path), 'bytes': path.stat().st_size}
                 for path in sorted(output.iterdir()) if path.is_file()]
        if not files:
            raise RuntimeError('Packaging produced no files')
        verdict.update(status='passed', artifacts=files,
                       commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip())
    finally:
        if (build / 'Testing').is_dir():
            shutil.copytree(build / 'Testing', evidence / 'Testing', dirs_exist_ok=True)
        (evidence / 'verification.json').write_text(json.dumps(verdict, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
