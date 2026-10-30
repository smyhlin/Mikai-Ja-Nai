"""Provision the public Qt SDK; never acquire optional inference components."""
import argparse
import json
import os
from pathlib import Path
import hashlib
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request


def run(*args):
    print('+', *args, flush=True)
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)



# Qt publishes one Updates.xml hash per *package*, not per archive, so a direct
# acquisition cannot verify those bytes the way aqt verifies what it fetches.
# These are the exact archives the Windows kit needs, pinned by SHA-256 and
# extracted into the layout aqt's installer would produce. Re-derive a digest
# only by downloading the same URL and confirming the file still builds.
WINDOWS_QT_ROOT = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop'
WINDOWS_SDK = (
    {
        'url': WINDOWS_QT_ROOT + '/qt6_6120/qt6_6120_mingw/qt.qt6.6120.win64_mingw/'
               '6.12.0-0-202609280346qtbase-Windows-Windows_11_24H2-Mingw-'
               'Windows-Windows_11_24H2-X86_64.7z',
        'bytes': 23944038,
        'sha256': '3356346505113242a4f7c0b09ba46188424ee7938232166655f794c28a3477fc',
        'into': 'qt',
    },
    {
        'url': WINDOWS_QT_ROOT + '/qt6_6120/qt6_6120_mingw/qt.qt6.6120.win64_mingw/'
               '6.12.0-0-202609280346qtsvg-Windows-Windows_11_24H2-Mingw-'
               'Windows-Windows_11_24H2-X86_64.7z',
        'bytes': 345663,
        'sha256': '8dd193d39e21395f8fcb802f543f6e43e13a5caf5dd6f94cab731a53c598496a',
        'into': 'qt',
    },
    {
        'url': WINDOWS_QT_ROOT + '/qt6_6120/qt6_6120_mingw/qt.qt6.6120.win64_mingw/'
               '6.12.0-0-202609280346qttranslations-Windows-Windows_11_24H2-Mingw-'
               'Windows-Windows_11_24H2-X86_64.7z',
        'bytes': 2005152,
        'sha256': '4c8151e7d0cbcb7bcdd7e36d5a45b1d525b0873987a8a55f590a25eaab308c69',
        'into': 'qt',
    },
    {
        'url': WINDOWS_QT_ROOT + '/qt6_6120/qt6_6120_mingw/qt.qt6.6120.addons.'
               'qtimageformats.win64_mingw/6.12.0-0-202609280346qtimageformats-Windows-'
               'Windows_11_24H2-Mingw-Windows-Windows_11_24H2-X86_64.7z',
        'bytes': 641080,
        'sha256': '822d37a7f2f30a69eefad60f8dc6b17549b11b8b51e62fc9b47259ce3c3ea4de',
        'into': 'qt',
    },
    {
        'url': WINDOWS_QT_ROOT + '/tools_mingw1310/qt.tools.win64_mingw1310/'
               '13.1.0-202407240918mingw1310.7z',
        'bytes': 112089140,
        'sha256': 'a7b502294a903b64fccd4a41ba39f48d3f6c9a5ece167f0cfaeac920d15b344f',
        'into': 'tools',
    },
)


def seven_zip():
    for name in ('7z', '7za', '7zr'):
        found = shutil.which(name)
        if found:
            return found
    for candidate in (r'C:\Program Files\7-Zip\7z.exe', r'C:\Program Files (x86)\7-Zip\7z.exe'):
        if Path(candidate).is_file():
            return candidate
    raise RuntimeError('7-Zip is required to extract the pinned Qt SDK archives')


def install_windows_sdk(root, evidence):
    """Acquire the pinned Qt 6.12 MinGW kit without aqt.

    aqt 3.3 resolves the repository metadata one directory above where Qt now
    publishes it, so every `install-qt windows` call fails before downloading
    anything. The archives themselves are still served; this path fetches the
    pinned ones, verifies their digests and lays them out like the installer.
    """
    archives = []
    downloads = evidence / 'downloads'
    downloads.mkdir(parents=True, exist_ok=True)
    for entry in WINDOWS_SDK:
        name = Path(urllib.parse.urlparse(entry['url']).path).name
        target = downloads / name
        if not target.is_file() or target.stat().st_size != entry['bytes']:
            print(f'+ curl {entry["url"]}', flush=True)
            with urllib.request.urlopen(entry['url'], timeout=600) as response, \
                    target.open('wb') as stream:
                shutil.copyfileobj(response, stream, 1 << 20)
        measured = hashlib.file_digest(target.open('rb'), 'sha256').hexdigest()
        if measured != entry['sha256'] or target.stat().st_size != entry['bytes']:
            raise RuntimeError(
                f'{name} does not match its pin ({measured}, {target.stat().st_size} bytes)')
        destination = root / '6.12.0' / 'mingw_64' if entry['into'] == 'qt' else root
        destination.mkdir(parents=True, exist_ok=True)
        subprocess.run([seven_zip(), 'x', '-y', f'-o{destination}', str(target)],
                       check=True, stdout=subprocess.DEVNULL)
        archives.append({'file': name, 'bytes': entry['bytes'], 'sha256': measured,
                         'url': entry['url']})
        print(f'  extracted {name} into {destination}', flush=True)
    return archives


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

    archives = []
    if host == 'windows':
        archives = install_windows_sdk(args.root, args.evidence)
    else:
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
        # Qt's published 6.12.0 MinGW kit is 13.1.0; its toolchain archive is in
        # the pinned table, so the compiler is never an arbitrary PATH one.
        compilers = list((args.root / 'Tools').glob('*/bin/g++.exe'))
        if len(compilers) != 1:
            raise RuntimeError(f'Expected one provisioned MinGW compiler, found {compilers}')
        compiler = str(compilers[0])
        bins.insert(0, compilers[0].parent)
        version = run(compiler, '-dumpfullversion').strip()
        if not version.startswith('13.1.'):
            raise RuntimeError(f'Qt 6.12.0 repository publishes the MinGW 13.1.0 kit, got {version}')
    data = {'qtVersion': '6.12.0', 'qtPrefix': str(prefix), 'compiler': compiler,
            'path': [str(path) for path in bins], 'host': host,
            'acquisition': 'pinned-archives' if archives else 'aqt',
            'archives': archives}
    (args.evidence / 'toolchain.json').write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    if os.getenv('GITHUB_ENV'):
        with open(os.environ['GITHUB_ENV'], 'a', encoding='utf-8') as stream:
            stream.write(f'MIKAI_QT_PREFIX={prefix}\nMIKAI_CXX={compiler}\n')
        with open(os.environ['GITHUB_PATH'], 'a', encoding='utf-8') as stream:
            stream.write('\n'.join(str(path) for path in bins) + '\n')


if __name__ == '__main__':
    main()
