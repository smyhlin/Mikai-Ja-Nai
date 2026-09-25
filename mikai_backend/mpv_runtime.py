from __future__ import annotations

import ctypes
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path
from typing import Any

# Latest x64 mpv development archive from the Windows build project's
# GitHub Releases API. The release is checked at runtime, not hard-coded.
MPV_RELEASES_API = "https://api.github.com/repos/shinchiro/mpv-winbuild-cmake/releases/latest"


def cache_dir() -> Path:
    base = Path(os.environ.get("LOCALAPPDATA", Path.home()))
    return base / "MPVBackend" / "runtime"


def library_names() -> tuple[str, ...]:
    return ("mpv-2.dll", "libmpv-2.dll", "mpv.dll")


def find_library(folder: Path) -> Path | None:
    for name in library_names():
        found = next(folder.rglob(name), None)
        if found is not None and found.is_file():
            return found
    return None


def find_runtime(mpv_path: str | Path | None = None) -> Path | None:
    candidates: list[Path] = []

    if mpv_path:
        provided = Path(mpv_path).expanduser()
        candidates.append(provided if provided.is_dir() else provided.parent)

    # Common bundled locations, including PyInstaller's extraction folder.
    app_root = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
    candidates.extend((app_root / "mpv", app_root / "lib", app_root))

    # Previously downloaded runtime.
    candidates.append(cache_dir())

    for folder in candidates:
        if folder.is_dir() and find_library(folder):
            return folder.resolve()

    return None


def ensure_runtime(mpv_path: str | Path | None = None, *, auto_download: bool = True) -> Path:
    if sys.platform != "win32":
        raise OSError("This MPV runtime downloader is configured for Windows.")
    runtime = find_runtime(mpv_path)
    if runtime is not None:
        return runtime
    if not auto_download:
        raise FileNotFoundError("libmpv was not found. Pass mpv_path or enable auto_download.")
    return download_runtime()


def _request_json(url: str) -> dict:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "MPVBackend",
        },
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


SEVENZR_URL = "https://www.7-zip.org/a/7zr.exe"


def _find_seven_zip() -> Path | None:
    for name in ("7z.exe", "7z", "7za.exe", "7za"):
        found = shutil.which(name)
        if found:
            return Path(found)
    for candidate in (
        Path(r"C:\Program Files\7-Zip\7z.exe"),
        Path(r"C:\Program Files (x86)\7-Zip\7z.exe"),
    ):
        if candidate.is_file():
            return candidate
    return None


def _ensure_seven_zip() -> Path:
    system_7z = _find_seven_zip()
    if system_7z is not None:
        return system_7z
    tool = cache_dir().parent / "tools" / "7zr.exe"
    if tool.is_file():
        return tool
    tool.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(SEVENZR_URL, headers={"User-Agent": "MPVBackend"})
    with urllib.request.urlopen(request, timeout=60) as response, tool.open("wb") as out:
        shutil.copyfileobj(response, out)
    return tool


def _extract_archive(archive_path: Path, destination: Path) -> None:
    py7zr_error: Exception | None = None
    try:
        import py7zr

        with py7zr.SevenZipFile(archive_path, mode="r") as archive:
            archive.extractall(path=destination)
        return
    except Exception as exc:  # noqa: BLE001 - fall back, final failure below raises
        py7zr_error = exc

    # The mpv dev archive uses the BCJ2 filter, which py7zr does not implement;
    # fall back to the official 7-Zip console (system install or downloaded 7zr.exe).
    try:
        seven_zip = _ensure_seven_zip()
    except Exception as exc:
        raise RuntimeError(
            "py7zr cannot extract this archive (BCJ2) and 7-Zip is unavailable: "
            f"{py7zr_error}; 7zr download failed: {exc}"
        ) from exc
    result = subprocess.run(
        [str(seven_zip), "x", f"-o{destination}", "-y", str(archive_path)],
        capture_output=True,
        text=True,
        timeout=300,
        check=False,
    )
    if result.returncode != 0:
        tail = (result.stderr or result.stdout or "")[-500:]
        raise RuntimeError(
            f"7-Zip extraction failed (exit {result.returncode}); "
            f"py7zr error was: {py7zr_error}; {tail}"
        )


def download_runtime() -> Path:
    machine = platform.machine().lower()
    if machine not in {"amd64", "x86_64"}:
        raise OSError(f"This automatic downloader supports Windows x64; detected {machine}.")

    release = _request_json(MPV_RELEASES_API)
    assets = release.get("assets", [])

    # Development package includes libmpv and runtime DLLs.
    asset = next(
        (
            item
            for item in assets
            if item.get("name", "").startswith("mpv-dev-x86_64-")
            and item.get("name", "").endswith(".7z")
        ),
        None,
    )
    if asset is None:
        raise RuntimeError("Could not find an x64 mpv development archive in the latest release.")

    target = cache_dir()
    target.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="mpv-download-", ignore_cleanup_errors=True) as temp:
        temp_dir = Path(temp)
        archive_path = temp_dir / asset["name"]

        request = urllib.request.Request(
            asset["browser_download_url"],
            headers={"User-Agent": "MPVBackend"},
        )
        with (
            urllib.request.urlopen(request, timeout=120) as response,
            archive_path.open("wb") as out,
        ):
            shutil.copyfileobj(response, out)

        extracted = temp_dir / "extracted"
        extracted.mkdir()
        _extract_archive(archive_path, extracted)

        library = find_library(extracted)
        if library is None:
            raise RuntimeError("The downloaded mpv archive did not contain a supported libmpv DLL.")

        # Keep the full extracted tree because libmpv depends on adjacent DLLs.
        staging = target.with_name(target.name + ".new")
        if staging.exists():
            shutil.rmtree(staging)
        shutil.copytree(extracted, staging)

        if target.exists():
            shutil.rmtree(target)
        staging.replace(target)

    if find_library(target) is None:
        raise RuntimeError("Extracted mpv runtime is missing its libmpv DLL.")

    return target.resolve()


def configure_runtime(runtime_dir: Path) -> tuple[list[Any], ctypes.CDLL]:
    # Add every directory containing DLLs so Windows can resolve mpv/FFmpeg
    # runtime dependencies, even when archives put them in subfolders.
    dll_dirs = {runtime_dir}
    dll_dirs.update(file.parent for file in runtime_dir.rglob("*.dll") if file.is_file())

    handles: list[Any] = []
    for directory in dll_dirs:
        try:
            handles.append(os.add_dll_directory(str(directory)))
        except AttributeError, OSError:
            pass

    os.environ["PATH"] = str(runtime_dir) + os.pathsep + os.environ.get("PATH", "")

    library_path = find_library(runtime_dir)
    if library_path is None:
        raise FileNotFoundError(f"Could not locate libmpv under {runtime_dir}")

    # Preload the library so python-mpv can find it reliably on Windows.
    return handles, ctypes.CDLL(str(library_path))
