"""Provision the public Qt SDK; never acquire optional inference components."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def run(*args):
    print('+', *args, flush=True)
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--evidence', required=True, type=Path)
    args = parser.parse_args()
    args.root = args.root.resolve()
    args.evidence.mkdir(parents=True, exist_ok=True)
    host = 'windows' if sys.platform == 'win32' else 'linux'
    arch = 'win64_mingw' if host == 'windows' else 'linux_gcc_64'
    aqt = [sys.executable, '-m', 'aqt']
    log = args.evidence / 'provision.log'

    def install(*command):
        try:
            result = run(*aqt, *command)
        except subprocess.CalledProcessError as error:
            with log.open('a', encoding='utf-8') as stream:
                stream.write(error.output)
            raise RuntimeError(f'Qt provisioning failed; see {log}. Confirm Qt 6.12.0 public SDK availability; do not silently downgrade.') from error
        with log.open('a', encoding='utf-8') as stream:
            stream.write(result)
        print(result, flush=True)
        return result

    if host == 'linux' and shutil.which('patchelf') is None:
        apt = shutil.which('apt-get')
        if apt is None:
            raise RuntimeError('Linux CI provisioning requires apt-get to install build-only patchelf')
        privilege = [] if os.geteuid() == 0 else [shutil.which('sudo')]
        if privilege == [None]:
            raise RuntimeError('Installing build-only patchelf requires root or sudo')
        for command in ([*privilege, apt, 'update'],
                        [*privilege, apt, 'install', '--no-install-recommends', '-y', 'patchelf']):
            try:
                result = run(*command)
            except subprocess.CalledProcessError as error:
                with log.open('a', encoding='utf-8') as stream:
                    stream.write(error.output)
                raise RuntimeError(f'patchelf provisioning failed; see {log}') from error
            with log.open('a', encoding='utf-8') as stream:
                stream.write(result)
            print(result, flush=True)

    install('install-qt', host, 'desktop', '6.12.0', arch, '-O', str(args.root),
            '-m', 'qtimageformats')
    prefixes = list((args.root / '6.12.0').glob('*/lib/cmake/Qt6/Qt6Config.cmake'))
    if len(prefixes) != 1:
        raise RuntimeError(f'Expected exactly one Qt6 SDK under {args.root}, found {prefixes}')
    prefix = prefixes[0].parents[3]
    bins = [prefix / 'bin']
    compiler = 'g++'
    if host == 'linux':
        patchelf = shutil.which('patchelf')
        if patchelf is None:
            raise RuntimeError('Linux bundled mpv builds require patchelf as a build-only dependency')
        print(f'patchelf={patchelf}', flush=True)
    if host == 'windows':
        # Qt's published 6.12.0 MinGW kit metadata identifies MinGW 13.1.0.
        # Acquire the compiler only from its matching official tool group.
        variant = 'qt.tools.win64_mingw1310'
        group = 'tools_mingw1310'
        variants = install('list-tool', host, 'desktop', group).split()
        if variant not in variants:
            raise RuntimeError(f'Qt repository does not advertise {variant} in {group}. Inspect provision.log; update the Qt/toolchain pair together, never use an arbitrary PATH compiler.')
        install('install-tool', host, 'desktop', group, variant, '-O', str(args.root))
        compilers = list((args.root / 'Tools').glob('*/bin/g++.exe'))
        if len(compilers) != 1:
            raise RuntimeError(f'Expected one provisioned MinGW compiler, found {compilers}')
        compiler = str(compilers[0])
        bins.insert(0, compilers[0].parent)
        version = run(compiler, '-dumpfullversion').strip()
        if not version.startswith('13.1.'):
            raise RuntimeError(f'Qt 6.12.0 repository publishes the MinGW 13.1.0 kit, got {version}')
    data = {'qtVersion': '6.12.0', 'qtPrefix': str(prefix), 'compiler': compiler,
            'path': [str(path) for path in bins], 'host': host}
    (args.evidence / 'toolchain.json').write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    if os.getenv('GITHUB_ENV'):
        with open(os.environ['GITHUB_ENV'], 'a', encoding='utf-8') as stream:
            stream.write(f'MIKAI_QT_PREFIX={prefix}\nMIKAI_CXX={compiler}\n')
        with open(os.environ['GITHUB_PATH'], 'a', encoding='utf-8') as stream:
            stream.write('\n'.join(str(path) for path in bins) + '\n')


if __name__ == '__main__':
    main()
