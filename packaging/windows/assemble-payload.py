#!/usr/bin/env python3
"""Assembles the Windows payload and the Inno Setup file list from a cross build.

windeployqt.exe cannot run here (it needs Windows; it crashes under wine), so the
payload is the explicit set the application actually loads: the executable, Qt's
modules and the plugins it uses, the MinGW runtime DLLs, the pinned mpv DLL,
shaders, translations and licence material. Every file is copied by name — no
recursive source glob — and the installer list is generated from what was copied,
so the two cannot disagree.

Usage:
  assemble-payload.py --build-dir DIR --qt PREFIX --runtime-dlls DIR \
      --mpv-archive FILE --source DIR --out DIR --iss FILE --version VERSION
"""
import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys

SYSTEM = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll", "ole32.dll",
    "oleaut32.dll", "comdlg32.dll", "comctl32.dll", "ws2_32.dll", "wsock32.dll", "winmm.dll",
    "imm32.dll", "mpr.dll", "netapi32.dll", "userenv.dll", "version.dll", "uxtheme.dll",
    "shlwapi.dll", "crypt32.dll", "bcrypt.dll", "ncrypt.dll", "dnsapi.dll", "iphlpapi.dll",
    "secur32.dll", "wldap32.dll", "dbghelp.dll", "psapi.dll", "powrprof.dll", "setupapi.dll",
    "cfgmgr32.dll", "dwmapi.dll", "dwrite.dll", "d3d11.dll", "d3d9.dll", "dxgi.dll", "d2d1.dll",
    "d3dcompiler_47.dll", "opengl32.dll", "glu32.dll", "msvcrt.dll", "ucrtbase.dll", "msimg32.dll",
    "winspool.drv", "wtsapi32.dll", "authz.dll", "normaliz.dll", "mswsock.dll", "ntdll.dll",
    "kernelbase.dll", "wevtapi.dll", "wintrust.dll", "runtimeobject.dll", "propsys.dll",
    "dcomp.dll", "windowscodecs.dll", "pdh.dll", "hid.dll", "dxva2.dll", "evr.dll", "mf.dll",
    "mfplat.dll", "mfreadwrite.dll", "shcore.dll", "avrt.dll", "oleacc.dll", "gdiplus.dll",
    "dbgeng.dll", "winhttp.dll", "wininet.dll", "wlanapi.dll", "credui.dll", "netutils.dll",
    "srvcli.dll", "samcli.dll", "slc.dll", "tbs.dll", "mgmtapi.dll", "snmpapi.dll",
}

PLUGINS = [
    "platforms/qwindows.dll",
    "imageformats/qjpeg.dll",
    "imageformats/qwebp.dll",
    "imageformats/qgif.dll",
    "imageformats/qico.dll",
    "imageformats/qsvg.dll",
    "iconengines/qsvgicon.dll",
    "tls/qschannelbackend.dll",
    "tls/qcertonlybackend.dll",
    "styles/qmodernwindowsstyle.dll",
    "networkinformation/qnetworklistmanager.dll",
]


def imports(path):
    out = subprocess.run(["objdump", "-p", path], capture_output=True, text=True).stdout
    return [line.split(":", 1)[1].strip().lower() for line in out.splitlines()
            if line.strip().startswith("DLL Name:")]


def find(name, directories):
    for directory in directories:
        candidate = os.path.join(directory, name)
        if os.path.exists(candidate):
            return candidate
        try:
            for entry in os.listdir(directory):
                if entry.lower() == name.lower():
                    return os.path.join(directory, entry)
        except OSError:
            pass
    return None


def import_table_symbols(output):
    imports = {}
    in_import_table = False
    found_import_table = False
    current_dll = None
    for line in output.splitlines():
        if line.startswith("The Import Tables"):
            in_import_table = True
            found_import_table = True
            continue
        if in_import_table and line.startswith("The "):
            in_import_table = False
        if not in_import_table:
            continue
        match = re.search(r"DLL Name: (.+)$", line)
        if match:
            current_dll = match.group(1).casefold()
            imports.setdefault(current_dll, set())
            continue
        if current_dll:
            match = re.search(r"\s([^\s]+)$", line)
            if match:
                imports[current_dll].add(match.group(1))
    if not found_import_table:
        raise ValueError("objdump output has no PE import table")
    return imports


def export_table_symbols(output):
    exports = set()
    in_export_table = False
    found_export_table = False
    for line in output.splitlines():
        if line.startswith("The Export Tables"):
            in_export_table = True
            found_export_table = True
            continue
        if in_export_table and line.startswith("The "):
            in_export_table = False
        if in_export_table:
            match = re.search(r"\s([^\s]+)$", line)
            if match:
                exports.add(match.group(1))
    if not found_export_table:
        raise ValueError("objdump output has no PE export table")
    return exports


def validate_qt_mingw_runtime(payload):
    for name in ("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"):
        if not (pathlib.Path(payload) / name).is_file():
            sys.exit(f"deployed Qt compiler runtime is missing: {pathlib.Path(payload) / name}")

    qt_core = pathlib.Path(payload) / "Qt6Core.dll"
    if not qt_core.is_file():
        sys.exit(f"deployed Qt core library is missing: {qt_core}")
    core_output = subprocess.run(
        ["objdump", "-p", str(qt_core)], capture_output=True, text=True, check=True).stdout
    if "__cxa_call_terminate" not in import_table_symbols(core_output).get(
            "libstdc++-6.dll".casefold(), set()):
        return

    runtime = pathlib.Path(payload) / "libstdc++-6.dll"
    runtime_output = subprocess.run(
        ["objdump", "-p", str(runtime)], capture_output=True, text=True, check=True).stdout
    if "__cxa_call_terminate" not in export_table_symbols(runtime_output):
        sys.exit(
            "Qt6Core.dll imports __cxa_call_terminate from libstdc++-6.dll, "
            "but the deployed runtime does not export it")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--qt", type=pathlib.Path, required=True)
    parser.add_argument("--runtime-dlls", type=pathlib.Path, required=True)
    parser.add_argument("--mpv-archive", type=pathlib.Path, required=True)
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--iss", type=pathlib.Path, required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()

    executable = args.build_dir / f"mikai.exe-{args.version}"
    if not executable.is_file():
        sys.exit(f"cross build did not produce {executable}")

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)

    search = [str(args.qt / "bin"), str(args.qt), str(args.runtime_dlls)]
    queue, copied = [], {}

    def take(name):
        if name.lower() in copied or name.lower() in SYSTEM:
            return
        source = find(name, search)
        if not source:
            return
        target = args.out / os.path.basename(source)
        if not target.exists():
            shutil.copy2(source, target)
        copied[name.lower()] = str(target)
        queue.append(str(target))

    app = args.out / "mikai.exe"
    shutil.copy2(executable, app)
    queue.append(str(app))
    while queue:
        for dependency in imports(queue.pop()):
            take(dependency)

    missing = []
    for relative in PLUGINS:
        source = args.qt / "plugins" / relative
        if not source.is_file():
            missing.append(relative)
            continue
        target = args.out / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        for dependency in imports(target):
            take(dependency)
    if missing:
        sys.exit(f"Qt plugins missing from the SDK: {missing}")
    validate_qt_mingw_runtime(args.out)

    shutil.copytree(args.source / "shaders", args.out / "shaders")
    (args.out / "translations").mkdir(exist_ok=True)
    for name in ("qt_uk.qm", "qtbase_uk.qm"):
        source = args.qt / "translations" / name
        if source.is_file():
            shutil.copy2(source, args.out / "translations" / name)
    built_translation = args.build_dir / "mikai_uk.qm"
    if built_translation.is_file():
        shutil.copy2(built_translation, args.out / "translations" / "mikai_uk.qm")

    extract = args.out.parent / "_mpv-extract"
    if extract.exists():
        shutil.rmtree(extract)
    subprocess.run(["7z", "x", "-y", "-bso0", "-bsp0", str(args.mpv_archive),
                    f"-o{extract}"], check=True)
    dlls = list(extract.rglob("libmpv-2.dll"))
    if not dlls:
        sys.exit("libmpv-2.dll is missing from the pinned archive")
    (args.out / "mpv").mkdir(exist_ok=True)
    shutil.copy2(dlls[0], args.out / "mpv" / "libmpv-2.dll")
    shutil.rmtree(extract)

    shutil.copy2(args.source / "LICENSE", args.out / "LICENSE")
    shutil.copy2(args.source / "packaging/third-party.json", args.out / "third-party.json")
    shutil.copytree(args.source / "packaging/licenses", args.out / "licenses", dirs_exist_ok=True)
    (args.out / "qt.conf").write_text(
        "[Paths]\nPrefix=.\nPlugins=.\nTranslations=translations\n", encoding="utf-8")
    (args.out / "run.bat").write_bytes(b'@echo off\r\nstart "" "%~dp0mikai.exe" %*\r\n')
    revision = subprocess.run(["git", "-C", str(args.source), "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
    (args.out / "VERSION").write_text(f"{revision}\n", encoding="utf-8")

    payload = sorted(
        path.relative_to(args.out).as_posix()
        for path in args.out.rglob("*") if path.is_file())
    lines = []
    for relative in payload:
        destination = "{app}"
        if "/" in relative:
            directory = relative.rsplit("/", 1)[0].replace("/", "\\")
            destination = "{app}\\" + directory
        lines.append(f'Source: "{{#StageDir}}\\{relative.replace("/", chr(92))}"; '
                     f'DestDir: "{destination}"; Flags: ignoreversion')
    lines.sort()
    args.iss.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"files: {len(payload)}")
    print(f"installer list: {args.iss}")


if __name__ == "__main__":
    main()
