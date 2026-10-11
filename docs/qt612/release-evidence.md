# Qt 6.12 migration — evidence record

Every line below is a command that was run and its observed output, or a file
that was read. Where a result is pending or not obtained, it says so.

## 1. Target identification

```
$ grep -E '^QT_VERSION|^QT_GCC' C:/temp/qt6/6.12.0/mingw_64/mkspecs/qconfig.pri
QT_VERSION = 6.12.0
QT_GCC_MAJOR_VERSION = 15
QT_GCC_MINOR_VERSION = 1
QT_GCC_PATCH_VERSION = 0

$ powershell -NoProfile -Command "(Get-Item 'C:/temp/qt6/6.12.0/mingw_64/bin/Qt6Core.dll').VersionInfo.FileVersion"
6.12.0.0

$ PATH=<qt612>/bin <qt612>/bin/windeployqt.exe --version
6.12.0
$ PATH=<qt612>/bin <qt612>/bin/lrelease.exe -version
lrelease version 6.12.0
```

Source of the prefix: the official Qt repository, not an installer:

```
$ curl -s https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6120/ | grep -oE 'href="qt6_6120[^"]*"'
href="qt6_6120_llvm_mingw/"
href="qt6_6120_mingw/"
href="qt6_6120_msvc2022_64/"
href="qt6_6120_msvc2022_arm64_cross_compiled/"
```

Packages unpacked into `C:/temp/qt6/6.12.0/mingw_64` with
`C:/Windows/System32/tar.exe -xf` (bsdtar reads 7z; no 7-Zip, no Python):
`qtbase`, `qttools`, `qtsvg`, `qttranslations`, `qtimageformats`,
`qtdeclarative`, `qtlanguageserver`, `opengl32sw`, `d3dcompiler_47` and the
`MinGW-w64-x86_64-…-runtime` archive.

Module configs present after unpacking:

```
$ for m in Qt6Core Qt6Gui Qt6Widgets Qt6Network Qt6OpenGL Qt6OpenGLWidgets Qt6Test Qt6Svg Qt6LinguistTools; do test -f "$Q/lib/cmake/$m/${m}Config.cmake" && echo "OK $m"; done
OK Qt6Core  OK Qt6Gui  OK Qt6Widgets  OK Qt6Network  OK Qt6OpenGL
OK Qt6OpenGLWidgets  OK Qt6Test  OK Qt6Svg  OK Qt6LinguistTools
```

Plugins after unpacking (before `qtimageformats`): `platforms/` qwindows,
qdirect2d, qminimal, qoffscreen; `imageformats/` qgif, qico, qjpeg, qsvg;
`tls/` qcertonlybackend, qopensslbackend, qschannelbackend. After
`qtimageformats`: qicns, qtga, qtiff, qwbmp, **qwebp**.

## 2. Two tool failures found by running the tools, not by reading a manifest

**`lupdate` exits 53 with no output** — its import table names `Qt6Qml.dll`:

```
$ objdump -p <qt612>/bin/lupdate.exe | grep 'DLL Name'
    DLL Name: ADVAPI32.dll
    DLL Name: KERNEL32.dll
    DLL Name: Qt6Core.dll
    DLL Name: Qt6Qml.dll
    DLL Name: libstdc++-6.dll
    DLL Name: msvcrt.dll
```

After unpacking `qtdeclarative` (`bin/Qt6Qml.dll`) and `qtlanguageserver`:

```
$ PATH=<qt612>/bin <qt612>/bin/lupdate.exe -no-obsolete src tests/cpp -ts translations/mikai_uk.ts
Scanning directory 'tests/cpp'...
Updating 'translations/mikai_uk.ts'...
    Found 1016 source text(s) (0 new and 1016 already existing)
$ <qt612>/bin/lrelease.exe translations/mikai_uk.ts -qm <scratch>/out.qm
Updating '<scratch>/out.qm'...
    Generated 1016 translation(s) (1016 finished and 0 unfinished)
```

`git diff --stat -- translations/mikai_uk.ts` after the in-place `lupdate` run is
**empty**: the catalogue round-trips unchanged, location hints included.

**`windeployqt` has no WebP plugin to deploy** without `qtimageformats` — the
same failure `readme.md` documents for 6.8.3, reproduced on 6.12 and fixed by
unpacking the addon.

## 3. Build

```
$ C:/temp/w64dev/w64devkit/bin/cmake.exe --preset release
-- Configuring done (12.1s)
-- Generating done (3.0s)
-- Build files have been written to: F:/ZKS/mikai_janai/build/release
```

The reconfigure was run in the existing `build/release` after deleting only
`CMakeCache.txt`/`CMakeFiles/`; `native/` (the real development native store),
`_dist-cache/` (the pinned mpv archive) and `_test-runs/` were preserved.

```
$ cmake --build build/release --target mikai mikai_tests --parallel
build-rc=0   errors=0   warnings=0

$ cmake --build build/release --target <33 probe targets> --parallel
probes-build-rc=0   errors=0   warnings=0
```

The single fix the migration needed, found by that warning count:

```
src/probes/player/EnginePopupProbe.cpp:490:61: warning:
  'QDragEnterEvent::QDragEnterEvent(const QPoint&, …)' is deprecated:
  Use the constructor taking QPointF [-Wdeprecated-declarations]
```

`<qt612>/include/QtGui/qevent.h:759` marks it `QT_DEPRECATED_VERSION_X_6_16` —
the unversioned `_X_` marker warns regardless of any cutoff. Changed to
`QPointF(10, 10)`, matching the `QDropEvent` construction two lines below.

## 4. Runtime linkage (the mixed-toolchain question)

`mikai.exe`'s import table is `KERNEL32, Qt6Core/Gui/Network/OpenGL/
OpenGLWidgets/Widgets, SHELL32, msvcrt` — no `libstdc++-6.dll`, no
`libgcc_s_seh-1.dll`, no `libwinpthread-1.dll`: w64devkit ships `libstdc++.a`
and no `.dll.a`, so the app's own C++ runtime is static. Qt's DLLs import the
shared ones, which the dist deploys from the Qt prefix.

Each Qt module loads standalone against the runtime the Qt package ships:

```
$ powershell … SetErrorMode(SEM_FAILCRITICALERRORS); LoadLibraryExW(<qt612>\bin\<dll>, 0, LOAD_WITH_ALTERED_SEARCH_PATH)
Qt6Core.dll ok   Qt6Gui.dll ok   Qt6Widgets.dll ok   Qt6Network.dll ok
Qt6OpenGL.dll ok   Qt6OpenGLWidgets.dll ok   Qt6Svg.dll ok
```

## 5. Test suite

```
$ ctest --test-dir build/release --output-on-failure
1/4 Test #1: NativeStageContracts .............   Passed    1.43 sec
2/4 Test #2: LauncherContracts ................   Passed    3.25 sec
3/4 Test #3: DistContracts ....................   Passed    0.82 sec
4/4 Test #4: mikai_tests ......................   Passed   30.94 sec
100% tests passed out of 4
```

Per-suite detail through the isolated launcher (`mikai_tests` prints one line
per suite): 35 suites `PASS`, `TestLiveOracle` `SKIP` (no API key), launcher
exit 0.

## 6. Application run

```
$ powershell Start-Process build/release/mikai.exe   # interactive session
window class Qt6120QWindowIcon, title "Mikai", visible
```

`build/release/mikai.log`:

```
12:55:00.227  ---- mikai started ----
12:55:00.229  folder   : F:/ZKS/mikai_janai/build/release
12:55:00.229  WARNING: a previous session left a crash report: mikai-crash-2020261011-100858.txt
12:55:01.647  [INFO] mikai: player runtime standard stock-20c217472c01
```

1.4 s from start to the runtime line (6.8.3 measured ~0.7 s in an earlier
session; the number is recorded, not compared). No new `mikai-crash-*.txt`.

**Launch-context caveat, recorded so it is not misread as a regression:**
launched from this session's non-interactive shell the same binary exits
immediately with code 53 and never reaches `main()`, and a process started that
way sits in a Windows hard-error dialog (`ntdll!ZwRaiseHardError`). The same
binary started through `Start-Process` from the interactive session runs
normally. This is a property of the launch context, not of Qt 6.12 — the
6.8.3-built binary is subject to the same rule (`readme.md`: pixel proofs need
the interactive session attached).

## 7. Protected state

`dist-mikai/` (installed 6.8.3 application and its per-machine native assets),
`.env`, and the three component install records were not touched by this
migration; `build/release/native/` was preserved across the reconfigure. The
6.8.3 SDK at `C:/temp/qt6/6.8.3/mingw_64` remains installed as the rollback
path.
