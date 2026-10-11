# Qt 6.12 migration — factual build & Qt surface inventory

Repository: `F:/ZKS/mikai_janai`, branch `migrate-to-qt-6.12`, baseline commit `6bfa80f`.
This document is an inventory only: no recommendations, no migration plan.
Every claim below carries a `path:line`, a file listing, or a command with its observed output.

> **Reading state note.** While this inventory was being compiled, the migration itself landed in
> the working tree (`git diff` at the time of writing: `CMakePresets.json` now sets
> `CMAKE_PREFIX_PATH` to `C:/temp/qt6/6.12.0/mingw_64`, and `CMakeLists.txt:116` now reads
> `find_package(Qt6 6.12 REQUIRED …)` with `CMakeLists.txt:24` adding
> `add_compile_definitions(QT_DISABLE_DEPRECATED_UP_TO=0x060C00)`). All file:line citations below
> are against that live tree; the still-present Qt 6.8.3 installation at `C:/temp/qt6/6.8.3/mingw_64`
> is inventoried as the *other* installed prefix, since it remains the documented provenance
> reference. Section 5 records the baseline evidence at `6bfa80f`.

---

## 1. Build and toolchain inventory

### 1.1 Qt version, prefix, provenance

| Fact | Value | Evidence |
|---|---|---|
| **Declared Qt install prefix (live tree)** | `C:/temp/qt6/6.12.0/mingw_64` | `CMakePresets.json:12` (`"CMAKE_PREFIX_PATH": "C:/temp/qt6/6.12.0/mingw_64"`, working-tree diff); required version now `6.12` at `CMakeLists.txt:116` |
| **Previous prefix (baseline `6bfa80f`)** | `C:/temp/qt6/6.8.3/mingw_64` | `git show 6bfa80f:CMakePresets.json` (`"CMAKE_PREFIX_PATH": "C:/temp/qt6/6.8.3/mingw_64"`) |
| Configured prefix (build tree) | `C:/temp/qt6/6.12.0/mingw_64` | `build/release/CMakeCache.txt` (`CMAKE_PREFIX_PATH:UNINITIALIZED=C:/temp/qt6/6.12.0/mingw_64`, `Qt6Core_DIR:PATH=C:/temp/qt6/6.12.0/mingw_64/lib/cmake/Qt6Core`) |
| Qt version in the 6.12 install | **6.12.0 LTS** | `C:/temp/qt6/6.12.0/mingw_64/mkspecs/qconfig.pri:9` (`QT_VERSION = 6.12.0`); `C:/temp/qt6/6.12.0/mingw_64/bin/windeployqt.exe --version` → `6.12.0` |
| Qt version in the 6.8.3 install | **6.8.3** | `C:/temp/qt6/6.8.3/mingw_64/mkspecs/qconfig.pri:9` (`QT_VERSION = 6.8.3`); `C:/temp/qt6/6.8.3/mingw_64/bin/windeployqt.exe --version` → `6.8.3` |
| `Qt6Core.dll` FileVersion (6.12 install) | `6.12.0.0`, `ProductName: Qt6`, `CompanyName: The Qt Company Ltd.`, `IsDebug=False` | `powershell -NoProfile -Command "(Get-Item 'C:/temp/qt6/6.12.0/mingw_64/bin/Qt6Core.dll').VersionInfo"` → `FileVersion: 6.12.0.0`, `ProductVersion: 6.12.0.0` |
| `Qt6Core.dll` FileVersion (6.8.3 install) | `6.8.3.0` (BuildPart 3, `IsDebug=False`) | `powershell -NoProfile -Command "(Get-Item 'C:/temp/qt6/6.8.3/mingw_64/bin/Qt6Core.dll').VersionInfo"` → `FileVersion: 6.8.3.0`, `FileName: C:\temp\qt6\6.8.3\mingw_64\bin\Qt6Core.dll`, `ProductName: Qt6`, `CompanyName: The Qt Company Ltd.`, `ProductVersion: 6.8.3.0` |
| `mikai.exe` FileVersion | none set (`0.0.0.0`, empty description) | same command on `F:/ZKS/mikai_janai/build/release/mikai.exe` → `FileVersion: ` (empty), `FileName: F:\ZKS\mikai_janai\build\release\mikai.exe` |

Compiler Qt itself was built with, from `C:/temp/qt6/6.8.3/mingw_64/mkspecs/qconfig.pri`:

```
qconfig.pri:2:QT_BUILDABI = x86_64-little_endian-lp64
qconfig.pri:4:QT.global.enabled_features = version_tagging shared separate_debug_info signaling_nan
             thread future concurrent dbus opensslv30 … libstdcpp_assertions shared separate_debug_info
             reduce_exports openssl
qconfig.pri:5:QT.global.disabled_features = static cross_compile pkg-config debug_and_release
             appstore-compliant … c++20 c++2a c++2b …
qconfig.pri:7:QT_CONFIG += shared no-pkg-config separate_debug_info reduce_exports openssl release
qconfig.pri:8:CONFIG +=  shared plugin_manifest intelcet stack_protector stack_clash_protection
             libstdcpp_assertions
qconfig.pri:14:QT_GCC_MAJOR_VERSION = 13
qconfig.pri:15:QT_GCC_MINOR_VERSION = 1
qconfig.pri:16:QT_GCC_PATCH_VERSION = 0
```

So: Qt 6.8.3 was built **GCC 13.1.0**, `x86_64-little_endian-lp64`, **shared** (`shared` in
`QT_CONFIG`/`CONFIG`, `static` and `cross_compile` in `disabled_features`), **release only**
(`debug_and_release` and `build_all` disabled), with `c++20`/`c++2a`/`c++2b` **not** compiled as
Qt-side enabled features (the project compiles its own sources as C++20 — §1.5). The install is an
`aqtinstall`-style layout (no Maintenance Tool, no `components.xml`; `readme.md:110` states "A Qt
installed with `aqtinstall` has no Maintenance Tool") with the standard `bin/ include/ lib/
mkspecs/ plugins/ qml/ translations/ doc/ metatypes/ phrasebooks/ sbom/ modules/` layout
(`ls C:/temp/qt6/6.8.3/mingw_64/`).

The target prefix `C:/temp/qt6/6.12.0/mingw_64/mkspecs/qconfig.pri` shows the same shape with
different provenance:

```
qconfig.pri:2:QT_BUILDABI =                                              (empty in the 6.12 build)
qconfig.pri:4:QT.global.enabled_features = version_tagging shared separate_debug_info
             signaling_nan thread future concurrent dbus opensslv30 test_gui test_squish
             test_auto shared intelcet trivial_auto_var_init_pattern stack_protector
             stack_clash_protection libstdcpp_assertions shared separate_debug_info
             reduce_exports openssl
qconfig.pri:7:QT_CONFIG += shared no-pkg-config separate_debug_info reduce_exports openssl release
qconfig.pri:8:CONFIG +=  shared plugin_manifest intelcet trivial_auto_var_init_pattern
             stack_protector stack_clash_protection libstdcpp_assertions
qconfig.pri:9:QT_VERSION = 6.12.0
qconfig.pri:14:QT_GCC_MAJOR_VERSION = 15
qconfig.pri:15:QT_GCC_MINOR_VERSION = 1
qconfig.pri:16:QT_GCC_PATCH_VERSION = 0
```

So **Qt 6.12.0 was built GCC 15.1.0** (6.8.3 was GCC 13.1.0) and still **shared/release**; new
enabled features in this build are `test_gui`, `test_squish`, `test_auto` and
`trivial_auto_var_init_pattern`. Both installs therefore link `libgcc_s_seh-1.dll`,
`libstdc++-6.dll` and `libwinpthread-1.dll` dynamically, per §1.7, and the app is still compiled
with **GCC 16.2.0** (`C:/temp/w64dev/w64devkit/bin/g++.exe --version` → `g++.exe (GCC) 16.2.0`) —
i.e. the app's compiler is newer than **both** Qt builds' compilers.

### 1.2 Build system, generator, CMake version

| Fact | Value | Evidence |
|---|---|---|
| Build system | CMake, minimum 3.25 | `CMakeLists.txt:4` (`cmake_minimum_required(VERSION 3.25)`), `CMakeLists.txt:5` (`project(mikai LANGUAGES CXX)`); `CMakePresets.json:3` (`cmakeMinimumRequired` 3.25.0) |
| Preset schema | 6 | `CMakePresets.json:2`; the schema 6 → CMake 3.25 constraint is explained in the file's opening comment at `CMakeLists.txt:1-3` (`cmake_minimum_required` now sits at line 4) |
| Generator | `MinGW Makefiles` | `CMakePresets.json:7`; `build/release/Makefile:1` (`# Generated by "MinGW Makefiles" Generator, CMake Version 4.4`) |
| CMake version used for the build tree | 4.4 | `build/release/Makefile:1` |
| CMake binary | `C:/temp/w64dev/w64devkit/bin/cmake.exe` (from w64devkit) | `build/release/CMakeFiles/mikai.dir/link.txt:1` (`C:\temp\w64dev\w64devkit\bin\cmake.exe -E rm -f …`) |
| make program | `C:/temp/w64dev/w64devkit/bin/make.exe` | `CMakePresets.json:10`; `build/release/CMakeCache.txt` (`CMAKE_MAKE_PROGRAM:UNINITIALIZED=C:/temp/w64dev/w64devkit/bin/make.exe`) |
| Configure presets | `base` (hidden), `release` (default), `debug`, `native` | `CMakePresets.json:5-47` |
| Build presets | `app`, `tests`, `app-native`, `tests-native` | `CMakePresets.json:49-57` |
| Binary dir | `${sourceDir}/build/${presetName}` | `CMakePresets.json:8` |
| Build type | `Release` (default; `release` preset sets no explicit type, `CMakeLists.txt:25-27` defaults empty to `Release`) | `CMakePresets.json:20-24`; `build/release/CMakeCache.txt` → `CMAKE_BUILD_TYPE:STRING=` (empty ⇒ `Release`) |

Release flags observed in the actual link command (`build/release/CMakeFiles/mikai.dir/link.txt:5`):

```
… g++.exe -O3 -DNDEBUG -mwindows -Wl,--gc-sections -Wl,--whole-archive CMakeFiles\mikai.dir/objects.a
  -Wl,--no-whole-archive -o mikai.exe -Wl,--out-implib,libmikai.dll.a
  -Wl,--major-image-version,0,--minor-image-version,0 @CMakeFiles\mikai.dir\linkLibs.rsp
```

`-O3 -DNDEBUG` comes from `CMAKE_CXX_FLAGS_RELEASE` (`build/release/CMakeCache.txt`), and the
project's own release tuning is at `CMakeLists.txt:1009-1077` (`-march=x86-64`,
`-ffunction-sections`, `-fdata-sections`, `-Wl,--gc-sections`, and
`INTERPROCEDURAL_OPTIMIZATION_RELEASE` when `check_ipo_supported` passes). `mikai` is
`WIN32_EXECUTABLE TRUE` (`CMakeLists.txt:852-853`; `-mwindows` in the link line above).

### 1.3 Compiler identity

| Fact | Value | Evidence |
|---|---|---|
| Compiler | `C:/temp/w64dev/w64devkit/bin/g++.exe` (GCC) **16.2.0** | `C:/temp/w64dev/w64devkit/bin/g++.exe --version` → `g++.exe (GCC) 16.2.0`; `CMakePresets.json:12` (`"CMAKE_CXX_COMPILER": "C:/temp/w64dev/w64devkit/bin/g++.exe"`); `build/release/CMakeCache.txt` (`CMAKE_CXX_COMPILER:UNINITIALIZED=C:/temp/w64dev/w64devkit/bin/g++.exe`) |
| GCC same generation | `gcc.exe (GCC) 16.2.0` | `C:/temp/w64dev/w64devkit/bin/gcc.exe --version` |
| GCC lib dir | `C:/temp/w64dev/w64devkit/lib/gcc/x86_64-w64-mingw32/16.2.0/` | `find C:/temp/w64dev/w64devkit -name 'libstdc++*'` → the only x64 dir is `16.2.0` (a `32/` subdirectory also exists) |

Note the generation gap: the app is compiled with **GCC 16.2.0** while Qt 6.8.3 was built with
**GCC 13.1.0** (`C:/temp/qt6/6.8.3/mingw_64/mkspecs/qconfig.pri:14-16`) and **Qt 6.12.0 was built
GCC 15.1.0** (`C:/temp/qt6/6.12.0/mingw_64/mkspecs/qconfig.pri:14-16`) — i.e. the app's compiler
is one generation newer than either Qt build. This is the single largest ABI-relevant fact in the
toolchain inventory and is why §1.7's runtime rules exist. `CMakePresets.json` pins the compiler
explicitly (`CMakePresets.json:12`, `CMAKE_CXX_COMPILER`), so the gap is deliberate, not
inherited from `PATH`.

### 1.4 Architecture

| Fact | Value | Evidence |
|---|---|---|
| Target architecture | `x86_64-w64-mingw32`, 64-bit PE | `build/release/CMakeFiles/mikai.dir/includes_CXX.rsp` → `-isystem …/mkspecs/win32-g++`; `file` on the exe is `PE32+ … x86-64` (confirmed via `objdump -f` on the sibling plugin `C:/temp/qt6/6.8.3/mingw_64/plugins/imageformats/qwebp.dll` → `PE32+ executable for MS Windows … x86-64`) |
| Tuning floor | `-march=x86-64` (x64 + SSE2, the Microsoft x64 ABI floor; never `native`, never `x86-64-v2`) | `CMakeLists.txt:1012-1018` (comment: "x86-64 + SSE2 … is the wholesale floor … `-march=native` and `-march=x86-64-v2` are deliberately not requested") |
| Windows APIs linked directly | `opengl32`, `gdi32`, `d3d11`, `d3d12`, `dxgi`, `dxguid`, `mpr`, `userenv` | `build/release/CMakeFiles/mikai.dir/linkLibs.rsp` → `-lopengl32 -lgdi32 -ld3d11 -ld3d12 -ldxgi -ldxguid -lmpr -luserenv …` |

### 1.5 C++ standard and warnings

| Fact | Value | Evidence |
|---|---|---|
| C++ standard | **20**, required | `CMakeLists.txt:7-8` (`set(CMAKE_CXX_STANDARD 20)`, `set(CMAKE_CXX_STANDARD_REQUIRED ON)`) |
| Warning flags | `-Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type` (GNU/Clang only) | `CMakeLists.txt:15-17`; rationale in the comment at `CMakeLists.txt:10-16` |
| Deprecation policy (new) | `QT_DISABLE_DEPRECATED_UP_TO=0x060C00` (Qt 6.12.0), applies to every target in this directory and in `native/` | `CMakeLists.txt:18-24` (comment: "Deprecated Qt APIs are errors, not warnings. 0x060C00 is Qt 6.12.0 …") |

### 1.6 Static vs shared Qt

Qt is used **shared**. Evidence chain, no inference:

- Qt's own build config: `qconfig.pri:7` `QT_CONFIG += shared …`; `qconfig.pri:4` lists `shared` as
  an enabled feature and `qconfig.pri:5` lists `static` as **disabled**.
- The link step links the *import* libraries, not static archives, and the exe imports the
  runtime DLLs by name:
  `C:/temp/w64dev/w64devkit/bin/objdump.exe -p build/release/mikai.exe | grep "DLL Name"` →
  `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`, `Qt6OpenGL.dll`, `Qt6OpenGLWidgets.dll`,
  `Qt6Network.dll`, `KERNEL32.dll`, `SHELL32.dll`, `msvcrt.dll`.
- `build/release/CMakeFiles/mikai.dir/linkLibs.rsp` names, among others,
  `C:/temp/qt6/6.12.0/mingw_64/lib/libQt6Core.a` (the shared-build import library).
- Documentation agrees: `readme.md:139` ("`mikai.exe` links Qt dynamically, so the runtime has to
  sit beside it or be on `PATH`; a fresh exe with no deployed DLLs exits with `0xC0000135`").

### 1.7 Compiler-runtime configuration (static vs dynamic libstdc++)

`objdump -p build/release/mikai.exe | grep 'DLL Name'` shows **no** `libstdc++-6.dll`,
`libgcc_s_seh-1.dll` or `libwinpthread-1.dll`: the app's own C++ runtime is **statically
linked** (GCC's default for MinGW: `libstdc++.a` / `libgcc_eh.a`, no `-shared-libstdc++` flag
anywhere in `CMakeLists.txt` — verified: no `shared-libstdc++`/`static-libstdc++` occurrence
exists in the tree).

Only a **static** `libstdc++.a` is present in the w64devkit GCC directory — no import-library
counterpart, which is itself the proof that the toolchain is configured for static C++ runtime:

```
C:/temp/w64dev/w64devkit/lib/gcc/x86_64-w64-mingw32/16.2.0/libstdc++.a
C:/temp/w64dev/w64devkit/lib/gcc/x86_64-w64-mingw32/16.2.0/libstdc++.a-gdb.py
C:/temp/w64dev/w64devkit/lib/gcc/x86_64-w64-mingw32/16.2.0/libstdc++.exp.la  (libstdc++exp.a)
C:/temp/w64dev/w64devkit/lib/gcc/x86_64-w64-mingw32/16.2.0/libstdc++fs.a
C:/temp/w64dev/w64devkit/lib/gcc/x86_64-w64-mingw32/16.2.0/libwinpthread.a
C:/temp/w64dev/w64devkit/lib/gcc/x86_64-w64-mingw32/16.2.0/32/libstdc++.a   (32-bit twin)
```

A recursive search for any `libstdc++*.dll.a` under `C:/temp/w64dev/w64devkit` returns nothing
(`find C:/temp/w64dev/w64devkit -name 'libstdc++*'` → 32 hits, all `.a`/`.la`/`.py`/`.json`,
none `.dll.a`). `C:/temp/w64dev/w64devkit/lib/` (the sysroot, 958 entries) contains
`libwinpthread.a` but **no** `libstdc++*` — the C++ runtime archives live only under
`lib/gcc/…/16.2.0/`.

By contrast, **Qt's** runtime is dynamic, in **both** installs:

`objdump -p C:/temp/qt6/6.8.3/mingw_64/bin/Qt6Core.dll | grep 'DLL Name'`:

```
DLL Name: ADVAPI32.dll      DLL Name: MPR.dll          DLL Name: USERENV.dll
DLL Name: AUTHZ.dll         DLL Name: NETAPI32.dll     DLL Name: VERSION.dll
DLL Name: KERNEL32.dll      DLL Name: SHELL32.dll      DLL Name: WINMM.dll
DLL Name: api-ms-win-core-synch-l1-2-0.dll             DLL Name: WS2_32.dll
DLL Name: libgcc_s_seh-1.dll   DLL Name: libstdc++-6.dll   DLL Name: libwinpthread-1.dll
DLL Name: msvcrt.dll        DLL Name: ole32.dll        DLL Name: USER32.dll
```

`objdump -p C:/temp/qt6/6.12.0/mingw_64/bin/Qt6Core.dll | grep 'DLL Name'` (filtered to the
compiler runtime) → `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`, `msvcrt.dll`.

Both import `libgcc_s_seh-1.dll`, `libstdc++-6.dll` and `libwinpthread-1.dll` **by name**, and
those three DLLs ship in each Qt's own `bin/`
(`ls C:/temp/qt6/6.8.3/mingw_64/bin/` → `libgcc_s_seh-1.dll` (109 056 B), `libstdc++-6.dll`
(2 243 072 B), `libwinpthread-1.dll` (53 248 B), all dated 2023-05-25 — i.e. Qt 6.8.3 ships the
GCC 13-era runtime beside itself; `ls C:/temp/qt6/6.12.0/mingw_64/bin/` shows the same three
present).

The project's policy for this asymmetry is enforced, not assumed (`CMakeLists.txt:29-37` and
`CMakeLists.txt:66-113`, function `mikai_deploy_mingw_runtime_targets`):

- `MIKAI_MINGW_RUNTIME_DIR` (default **empty**, so nothing is deployed by default —
  `build/release/CMakeCache.txt` → `MIKAI_MINGW_RUNTIME_DIR:PATH=`) names the directory holding
  exactly `libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll`, each of which must exist or
  configure dies (`CMakeLists.txt:31-39`).
- The selected runtime must be **byte-identical** (SHA-256) to the copies in Qt's `bin/`
  (`CMakeLists.txt:83-90`); a different generation is a `FATAL_ERROR` at `CMakeLists.txt:90-95`
  with the message "…is a different MinGW generation… Deploying it beside a Qt executable
  would replace the thread runtime Qt was built against."
- `readme.md:129-134` records the deployment consequence: deploy with
  `windeployqt --no-compiler-runtime …` "so Qt does not overwrite the selected CRT".
- Qt's own runtime files land in the shipped folder via windeployqt, not via this option
  (see §3).

### 1.8 Observed migration delta (working tree vs baseline `6bfa80f`)

Recorded because a concurrent migration step landed in the working tree while this inventory was
being compiled. Both changes are visible in `git diff` at the time of writing:

| Change | Location | Effect on the surface |
|---|---|---|
| `CMAKE_PREFIX_PATH` `C:/temp/qt6/6.8.3/mingw_64` → `C:/temp/qt6/6.12.0/mingw_64` | `CMakePresets.json:12` | the configured Qt prefix; the 6.8.3 install remains on disk and is inventoried above |
| `find_package(Qt6 REQUIRED …)` → `find_package(Qt6 6.12 REQUIRED …)` | `CMakeLists.txt:116` | a **version floor** — the component list itself is unchanged |
| `add_compile_definitions(QT_DISABLE_DEPRECATED_UP_TO=0x060C00)` | `CMakeLists.txt:18-24` | deprecated Qt APIs become compile errors; adds no module |

The build tree `build/release/` was already configured against `C:/temp/qt6/6.12.0/mingw_64`
before this diff (`build/release/CMakeCache.txt` → `CMAKE_PREFIX_PATH:UNINITIALIZED=C:/temp/qt6/6.12.0/mingw_64`;
`build/release/CMakeFiles/mikai.dir/includes_CXX.rsp` → `-isystem C:/temp/qt6/6.12.0/mingw_64/include/QtCore`
and friends; `build/release/CMakeFiles/mikai.dir/linkLibs.rsp` →
`C:/temp/qt6/6.12.0/mingw_64/lib/libQt6Core.a` etc.), so the diff aligned the *declared*
configuration with the *already-configured* build.

---

## 2. Qt module and plugin inventory

### 2.1 Exact `find_package(Qt6 …)` component list

Two `find_package(Qt6 …)` calls exist in the whole repository (all others under `./build/…` or
`./.probe-out/…` belong to vendored meson trees and are not part of this project):

```
CMakeLists.txt:116:find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network LinguistTools)
CMakeLists.txt:151:  find_package(Qt6 REQUIRED COMPONENTS Test)
native/CMakeLists.txt:109:  find_package(CUDAToolkit REQUIRED)      # the optional MSVC/nvcc shim, OFF by default
```

(the `Test` line above is the live line number; at baseline `6bfa80f` these two calls were at
`CMakeLists.txt:109` and `CMakeLists.txt:144`, unversioned.)

**App components (always):** `Core`, `Gui`, `Widgets`, `OpenGL`, `OpenGLWidgets`, `Network`,
`LinguistTools` — and the version floor is now `6.12` (`CMakeLists.txt:116`).
**Test-only component:** `Test` (found at `CMakeLists.txt:151`, inside the
`MIKAI_BUILD_PROBES` guard; the comment at `CMakeLists.txt:114-116` states "Test is deliberately
not in this list: it is only needed by the QA probes and mikai_tests, so the app itself
configures on a Qt install without it").

`CMakeLists.txt:119-121` also derives two prefixes used later:
`MIKAI_TEST_QT_PREFIX` = `${Qt6_DIR}/../../..` and, at `CMakeLists.txt:876-985`,
`MIKAI_QT_PREFIX` + `MIKAI_QMAKE_EXECUTABLE` (from the `Qt6::qmake` target), whose directory
yields `MIKAI_WINDEPLOYQT`.

### 2.2 Every `Qt6::X` link (file:line)

Complete list, `grep -rn "Qt6::" --include=CMakeLists.txt .` minus `build/`:

| Target | Qt targets linked | Location |
|---|---|---|
| `mikai_manifest_check` | `Qt6::Core` | `CMakeLists.txt:144` |
| `mikai_backend` (static) | `Qt6::Core`, `Qt6::Network` | `CMakeLists.txt:323` |
| `mikai_native_support` (static) | `Qt6::Core` | `CMakeLists.txt:400` |
| `mikai_player` (static) | `Qt6::Core`, `Qt6::Gui`, `Qt6::Widgets`, `Qt6::OpenGL`, `Qt6::OpenGLWidgets` | `CMakeLists.txt:506-511` |
| `mikai_app` (static) | `Qt6::Core`, `Qt6::Gui`, `Qt6::Widgets`, `Qt6::Network` | `CMakeLists.txt:833-839` |
| `mikai` (the shipped app) | via `mikai_app` only | `CMakeLists.txt:852-854` |
| `mikai_tests` | `Qt6::Test`, `Qt6::Widgets` | `CMakeLists.txt:730` |
| `mikai_test_launcher` | `Qt6::Core` | `CMakeLists.txt:736` |
| `mikai_http_fixture` | `Qt6::Core`, `Qt6::Network` | `tests/fixtures/CMakeLists.txt:20` |
| `mikai_launcher_child` | `Qt6::Core` | `tests/fixtures/CMakeLists.txt:24` |
| probes (all `EXCLUDE_FROM_ALL`, gated on `MIKAI_BUILD_PROBES`) | `Qt6::Core`/`Qt6::Widgets`/`Qt6::Test`/`Qt6::Gui`/`Qt6::OpenGL`/`Qt6::OpenGLWidgets` | `CMakeLists.txt:539,550,553,558,562,565,568,571,573,576,581,585,589,593,599,607,611,615,619,622,847,892,899,914,923,927,935,944` |

`Qt6::OpenGLWidgets` is explicitly called out at `CMakeLists.txt:113-114` ("Qt6Widgets pulls in
OpenGL; QOpenGLWidget itself lives in Qt6OpenGLWidgets"). `Qt6::LinguistTools` contributes no
link line: it is consumed by `qt_add_translations()` at `CMakeLists.txt:860-868`
(`LUPDATE_TARGET update_translations`, `LRELEASE_TARGET release_translations`,
`LRELEASE_OPTIONS -nounfinished`).

Qt headers actually included by first-party sources (`grep -rn "#include <Qt" src/ tests/`), by
module — this is the real surface:

- `QtCore` (**65 distinct headers**; `QSettings`, `QJsonDocument`/`QJsonObject`/`QJsonArray`,
  `QSaveFile`, `QStandardPaths`, `QLibraryInfo`, `QProcess`, `QThread`, `QThreadPool`,
  `QTemporaryDir`, `QUuid`, `QCryptographicHash`, `QMimeData`, `qt_windows.h`, …)
- `QtGui` (**11 distinct headers**; `QImage`, `QImageReader`, `QPalette`, `QClipboard`,
  `QGuiApplication`, …)
- `QtWidgets` (**25 distinct headers**; `QApplication`, `QWidget`, layouts, `QFileDialog`,
  `QMessageBox`, …)
- `QtNetwork` (4: `QTcpServer`, `QTcpSocket`, `QAbstractSocket`, `QHostAddress`)
- `QtTest` (2, tests/probes only: `QTest`, `QSignalSpy`)
- unqualified Qt OpenGL headers used through `QtGui`'s OpenGL integration:
  `QOpenGLWidget` (`src/player/VideoSurface.hpp:20`), `QOpenGLContext`/`QOpenGLFunctions`/
  `QOpenGLFramebufferObject` (`src/player/CompareRenderer.cpp:11-13`,
  `src/player/VideoSurface.cpp:12-13`), `QOpenGLFunctions_3_3_Core`/`QSurfaceFormat`
  (`src/probes/native/NativeDmlInteropProbe.cpp:17-19`)
- `#include <Qt>` → `src/player/PresetShortcuts.hpp:10`, `src/probes/shared/ProbeManifest.hpp:18`;
  `#include <QtGlobal>` → `src/app/state/NativePlaybackAssembly.hpp:43`,
  `src/probes/shared/ProbeRuntime.hpp:7`

**Third-party, non-Qt runtime:** libmpv, consumed purely as headers + one DLL
(`src/player/MpvApi.hpp:13-14` → `#include <mpv/client.h>`, `#include <mpv/render_gl.h>`;
symbols resolved through `decltype(&::mpv_create)` etc. at `src/player/MpvApi.hpp:39-41`).

### 2.3 Deployed plugin/DLL set (`dist-mikai/`)

Complete recursive listing of the assembled folder (`find dist-mikai -type f`), Qt-provided
entries first, then non-Qt entries:

**Qt runtime DLLs (8)** — deployed by `windeployqt --release --translations uk`
(`cmake/dist.cmake:169`):

```
dist-mikai/Qt6Core.dll          6 986 928 B
dist-mikai/Qt6Gui.dll          11 041 960 B
dist-mikai/Qt6Widgets.dll        7 025 320 B
dist-mikai/Qt6Network.dll        2 012 840 B
dist-mikai/Qt6OpenGL.dll           593 576 B
dist-mikai/Qt6OpenGLWidgets.dll     63 636 B
dist-mikai/Qt6Svg.dll              510 120 B      ← deployed although the app never links Qt6Svg
dist-mikai/opengl32sw.dll       20 639 888 B      ← software GL fallback, windeployqt's choice
```

**Qt plugins (11)**:

```
dist-mikai/platforms/qwindows.dll
dist-mikai/styles/qmodernwindowsstyle.dll
dist-mikai/tls/qschannelbackend.dll
dist-mikai/tls/qcertonlybackend.dll
dist-mikai/imageformats/qgif.dll
dist-mikai/imageformats/qico.dll
dist-mikai/imageformats/qjpeg.dll
dist-mikai/imageformats/qsvg.dll
dist-mikai/imageformats/qwebp.dll          ← hard runtime dependency (readme.md:90-97)
dist-mikai/iconengines/qsvgicon.dll
dist-mikai/networkinformation/qnetworklistmanager.dll
dist-mikai/generic/qtuiotouchplugin.dll
```

**Qt translation catalogs (3)** — `qt_uk.qm` + `qtbase_uk.qm` by `cmake/dist.cmake:180-185`
(explicit loop) and `mikai_uk.qm` renamed into place by `cmake/dist.cmake:186-188`:

```
dist-mikai/translations/qt_uk.qm
dist-mikai/translations/qtbase_uk.qm
dist-mikai/translations/mikai_uk.qm
```

**Non-Qt entries**:

```
dist-mikai/mikai.exe                      7 166 019 B   (cmake/dist.cmake:164)
dist-mikai/libgcc_s_seh-1.dll               109 056 B   (MinGW runtime, windeployqt)
dist-mikai/libstdc++-6.dll                2 243 072 B   (MinGW runtime, windeployqt)
dist-mikai/libwinpthread-1.dll               53 248 B   (MinGW runtime, windeployqt)
dist-mikai/D3Dcompiler_47.dll             4 173 928 B   (windeployqt; ANGLE/software-GL dependency)
dist-mikai/mpv/libmpv-2.dll                             (SHA-pinned API DLL, dist.cmake:190-196)
dist-mikai/run.bat                                      (dist.cmake:185)
dist-mikai/VERSION                                       (dist.cmake:199-202)
dist-mikai/mikai.log                     1 205 090 B     (runtime artifact, not part of the package)
dist-mikai/packaged-app-startup.png       867 190 B     (runtime screenshot artifact)
dist-mikai/shaders/Anime4K_*.glsl   (49 files, dist.cmake:173)
dist-mikai/native/**            (AnimeJaNai install tree — see §4)
```

The three MinGW runtime DLLs and `D3Dcompiler_47.dll` are copied by windeployqt (they are absent
from `dist-mikai`'s own provenance in `cmake/dist.cmake`, whose only writes are the exe,
windeployqt, shaders, translations, run.bat, VERSION, mpv, native). `dist-mikai/` is
git-ignored (`.gitignore:29` → `dist-mikai/`; `git ls-files dist-mikai` returns nothing).

`Qt6Svg.dll` and its `qsvg`/`qsvgicon` plugins are a notable overshoot: no source links
`Qt6::Svg` (§2.1) and no `QtSvg` header is included (§2.2) — windeployqt pulls them in through
its own dependency scan. `qwebp.dll` must exist in the Qt install for this to work
(`readme.md:96-100`; `tests/cpp/TestModels.cpp:447-457` `webpIsDecodable()` fails the suite if it
is missing). `objdump -p C:/temp/qt6/…/plugins/imageformats/qwebp.dll | grep "DLL Name"` shows
`libstdc++-6.dll` (not `VCRUNTIME140.dll`), i.e. it is a MinGW build, matching the check
described at `readme.md:104`.

### 2.4 Facilities this app does NOT use (proven by the searches run)

The searches below were run over `src/ tests/ native/tools/` plus `CMakeLists.txt` and
`tests/cmake/ tests/fixtures/`, i.e. the first-party tree. All counts are **0** unless stated:

| Facility | Search pattern(s) | Result |
|---|---|---|
| **QML / Quick** | `QQuick`, `QQml`, `QML_`, `QmlEngine`, `qml/` | **0 matches**, e.g. `grep -rniE "QQuick\|QQml\|QML_\|qml/" src/ tests/ native/ CMakeLists.txt` → no output |
| **Charts / DataVisualization / Graphs** | `QtCharts`, `QChart`, `DataVisualization`, `QtGraphs`, `QAbstractSeries` | **0 matches** |
| **WebEngine / PDF / WebView** | `QtWebEngine`, `QWebEngine`, `QtPdf`, `QPdf`, `WebView` | **0 matches** |
| **Multimedia** | `QtMultimedia`, `QMedia`, `QAudio`, `QVideo` | **0 matches** |
| **Sql** | `QtSql`, `QSql`, `QSqlDatabase` | **0 matches** (Qt's `plugins/sqldrivers/` exists in the install but is never deployed) |
| **gRPC / Protobuf (as a library)** | `grpc`, `GRPC`, `Protobuf` (case-insensitive, whole tree) | **No dependency.** `grpc`/`GRPC` → **0 matches**. `protobuf`/`Protobuf` → **5 matches, all ONNX text and none a link, include or target**: `src/player/native/models/NativeModelInspect.hpp:7` ("Nothing here parses the protobuf by hand…"), `src/probes/native/NativeAssetsProbe.cpp:91` and `tests/cpp/TestNativeCatalog.cpp:41` (both "A minimal protobuf writer"), `tests/cpp/TestNativeCatalog.cpp:915` and `tests/cpp/TestNativeCatalog.hpp:70` (a suite name). No `find_package(Protobuf)`, no `protobuf::` target, no `grpc++`/`_pb2` anywhere |
| **OpenAPI** | `OpenAPI`, `openapi` | **0 matches** (the API surface is hand-written: `src/backend/MikaiClient.cpp`) |
| **Quick3D / Qt3D** | `Quick3D`, `Qt3D`, `QMaterial` | **0 matches** |
| **Qt5Compat** | `Qt5Compat`, `QtCore5Compat` | **0 matches** |
| **`Qt.labs.*`** | `Qt.labs`, `labs.qml` | **0 matches** |
| **Qt private headers** | `#include <QtCore/private`, `#include <QtGui/private`, `#include <QtWidgets/private`; `#include <…_p.h"`; `#include <Qt$` (bare umbrella) | **0 matches** |
| Not-in-use-but-adjacent | `QtSerialPort`, `SerialBus`, `QtSerialBus` | **0 matches** |
| Not-in-use-but-adjacent | `QtStateMachine`, `QStateMachine` | **0 matches** |
| Not-in-use-but-adjacent | `QtDBus`, `QDBus` | **0 matches** |
| **`qt_add_executable` / `qt_add_qml_module` / `qt_add_resources` / `qt_add_big_resources`** | whole repo | **0 matches** — the only `qt_add_*` used is `qt_add_translations` (`CMakeLists.txt:860`) |

Used, for contrast (so the negatives above are not vacuous): `QtConcurrent|QThreadPool` = **11
matches** (four thread pools in `src/app/workers/Runner.hpp`, recorded at `plan.md:118`:
"shared 4, bulk 3, stream 2, image 12"). `AUTOMOC` is on for `mikai_player`
(`CMakeLists.txt:499`), `mikai_app` (`CMakeLists.txt:831`) and `mikai_tests`
(`CMakeLists.txt:723`), and deliberately off for `mikai_backend` (`CMakeLists.txt:318-321`,
"src/backend declares no Q_OBJECT/Q_GADGET"). The `.ts` catalogue holds exactly
1016 `<message>` entries and **0** `type="unfinished"` or `type="vanished"`
(`grep -c "<message" translations/mikai_uk.ts` → 1016; `grep -c 'type="unfinished"'` → 0).

One deprecation-adjacent setting now exists that the negatives above do not cover:
`QT_DISABLE_DEPRECATED_UP_TO=0x060C00` (`CMakeLists.txt:18-24`) — a *header-visibility* switch,
not a module dependency; it removes no module and adds no component.

Qt's install directory does contain `qml/` and `plugins/sqldrivers/`
(`ls C:/temp/qt6/6.8.3/mingw_64/qml` → QtQml, QtQuick, QtCore, QtNetwork, QtTest, Assets, QML,
QmlTime, builtins.qmltypes, jsroot.qmltypes) — installed Qt capability that this app simply does
not consume, and that windeployqt therefore does not deploy (`dist-mikai/` has no `qml/`).

---

## 3. Deployment and packaging

### 3.1 How `cmake/dist.cmake` deploys

The `dist` custom target is defined at `CMakeLists.txt:987-1005` and invokes the script:

```
cmake -DDIST_DIR=… -DEXE=$<TARGET_FILE:mikai> -DWINDEPLOYQT=${MIKAI_WINDEPLOYQT}
      -DSHADERS=${CMAKE_CURRENT_SOURCE_DIR}/shaders -DSOURCE_DIR=…
      -DMIKAI_DIST_NATIVE=… "-DMIKAI_DIST_NATIVE_ROOT=…"
      "-DMIKAI_DIST_MPV_ARCHIVE=${MIKAI_PINNED_MPV_ARCHIVE}" … -P cmake/dist.cmake
```

`MIKAI_WINDEPLOYQT` is derived from the `Qt6::qmake` imported target
(`CMakeLists.txt:982-985`: `get_target_property(MIKAI_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)`
→ its directory → `windeployqt.exe`). So **the Qt tool used is `windeployqt`, from the configured
Qt prefix's `bin/`**, invoked by the script at `cmake/dist.cmake:169`:

```cmake
execute_process(
  COMMAND "${WINDEPLOYQT}" --release --translations uk "${dist_exe}"
  RESULT_VARIABLE deploy_rc OUTPUT_VARIABLE deploy_out ERROR_VARIABLE deploy_err)
if(NOT deploy_rc EQUAL 0)
  message(FATAL_ERROR "dist: windeployqt failed (${deploy_rc}): ${deploy_err}")
endif()
```

`--translations uk` is what puts `qt_uk.qm`/`qtbase_uk.qm` into `translations/` (the script then
also copies them explicitly, `cmake/dist.cmake:180-185`), and `mikai_uk.qm` — written by
windeployqt at the exe root — is renamed into `translations/` at `cmake/dist.cmake:186-188`.

Sequence of writes (`cmake/dist.cmake:162-209`): make `DIST_DIR` (`:162`) → copy `EXE` (`:164`) →
windeployqt (`:168-173`) → copy `shaders/` (`:177`) → make `translations/` + copy the two Qt
catalogs (`:179-185`) → rename `mikai_uk.qm` (`:186-188`) → write `run.bat` (an `@echo off` +
`start "" "%~dp0mikai.exe"` stub, `:191`) → write `VERSION` containing `git rev-parse HEAD` + a
UTC timestamp (`:194-204`) → make `mpv/` + copy the pinned `libmpv-2.dll` (`:206-209`).

The mpv DLL is not arbitrary: it comes from a **SHA-256-pinned archive**
(`MIKAI_MPV_ARCHIVE_SHA256 = 077cb75f…2327c`, `CMakeLists.txt:184-186`), verified on both read
paths (`CMakeLists.txt:191-213` and `cmake/dist.cmake:61-67`), extracted, and validated as a
genuine `MZ`/`PE…` x86-64 image (`cmake/dist.cmake:83-99`: DOS signature `4d5a`, PE signature
`50450000`, machine `6486`, size ≥ 64). Exactly one `libmpv-2.dll` may be present in the
archive or the run fails (`cmake/dist.cmake:73-79`; the same rule at `CMakeLists.txt:221-239`).

### 3.2 The refusals `dist.cmake` enforces

| Refusal | Line | Message (abridged) |
|---|---|---|
| `DIST_DIR` unset | `cmake/dist.cmake:16` | `dist: DIST_DIR is not set` |
| `EXE` missing | `cmake/dist.cmake:19` | `dist: the executable was not found at …` |
| `SHADERS` not a directory | `cmake/dist.cmake:22` | `dist: the shaders directory was not found at …` |
| `windeployqt` missing | `cmake/dist.cmake:25-27` | `dist: windeployqt was not found at …` |
| mpv release metadata not passed | `cmake/dist.cmake:30-32` | `dist: pinned mpv release metadata was not provided by the configured build` |
| no mpv cache dir | `cmake/dist.cmake:38` | `dist: MIKAI_DIST_CACHE_DIR was not provided by the configured build` |
| archive hash mismatch | `cmake/dist.cmake:61-67` | `dist: mpv archive SHA-256 mismatch for …` |
| bad PE image | `cmake/dist.cmake:82/87/99` | `… too small / has no valid DOS/PE signature / is not a valid x64 PE image` |
| **populated output directory** | `cmake/dist.cmake:120-134` | `dist: refusing to recreate ${DIST_DIR} - it already contains: … The installer updates the app in place, so this script must never wipe an existing installation.` |
| `MIKAI_DIST_NATIVE=ON` with no `runtime/` | `cmake/dist.cmake:141-150` | `dist: the native runtime was not found at …/runtime. Nothing was assembled.` |
| **DIST_DIR inside NATIVE_ROOT** | `cmake/dist.cmake:152-158` | `MIKAI_DIST_DIR … must not be inside MIKAI_DIST_NATIVE_ROOT …; the script rewrites its output` |
| **NATIVE_ROOT inside DIST_DIR** | `cmake/dist.cmake:159-164` | the mirror check |
| **forbidden payload survived the sweep** | `cmake/dist.cmake:253-262` | `dist: the assembled folder still contains forbidden payload: … These are per-machine downloads, never shipped bytes.` |
| windeployqt nonzero exit | `cmake/dist.cmake:174` | `dist: windeployqt failed (rc): stderr` |

The forbidden-payload sweep (`cmake/dist.cmake:230-250`) is name-based over the **whole copied
tree** at any nesting depth, removing directories whose own name is one of
`components assets models records` (`cmake/dist.cmake:229`), with a documented rationale for not
using a glob (`cmake/dist.cmake:219-228`: a pattern ending in the forbidden name would also match
its parents and siblings). It runs a second collection pass afterwards and **fails loudly** if
anything survived (`cmake/dist.cmake:252-262`) — the assembly is verified, not assumed.

`MIKAI_DIST_DIR` defaults to `${CMAKE_SOURCE_DIR}/dist-mikai` and is documented "must not already
exist" (`CMakeLists.txt:973-976`); `MIKAI_DIST_NATIVE` defaults **OFF**
(`CMakeLists.txt:968`), and the native root copied is the **executable-local**
`<EXE dir>/native`, matching production `nativeRoot()` (`cmake/dist.cmake:106-118`), overridable
by `MIKAI_DIST_NATIVE_ROOT` (`CMakeLists.txt:978-981`).

The `mikai` target itself performs three more POST_BUILD steps outside dist: copy
`qtbase_uk.qm` into `<exe dir>/translations/` when the Qt install has it (`CMakeLists.txt:876-889`,
warning otherwise), copy the pinned `libmpv-2.dll` into `<exe dir>/mpv/`
(`CMakeLists.txt:948-956`), and copy the 49 `.glsl` shaders beside the exe
(`CMakeLists.txt:957-965`).

### 3.3 Which Qt-provided runtime files end up in the shipped folder

Every Qt file in `dist-mikai/` is listed with its size in §2.3. Summarised by kind: 6 Qt6 module
DLLs (`Core`, `Gui`, `Widgets`, `Network`, `OpenGL`, `OpenGLWidgets`, `Svg`), the software-GL
fallback `opengl32sw.dll`, the `D3Dcompiler_47.dll` that it needs, 11 plugins in
`platforms/ styles/ tls/ imageformats/ iconengines/ networkinformation/ generic/`, and 3 `uk`
translation catalogs. The MinGW runtime (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`,
`libwinpthread-1.dll`) also lands there — those come with Qt's deployment, not from
`MIKAI_MINGW_RUNTIME_DIR`, which is empty by default and is a configure-time SHA-256 match
against Qt's own copies, not a packager (§1.7).

---

## 4. Persisted and exchanged data

### 4.1 QSettings (native format, `HKCU\Software\Mikai\Mikai Wrapper`)

Identity is set once, before any preference or catalog path is resolved:
`src/app/main.cpp:87` `app.setApplicationName(QStringLiteral("Mikai Wrapper"))`,
`src/app/main.cpp:89` `app.setOrganizationName(QStringLiteral("Mikai"))` (version
`0.1.0` at `src/app/main.cpp:88`). No `QSettings::setDefaultFormat`, `setIniCodec` or explicit
`NativeFormat/IniFormat` call exists in application code — `QSettings::IniFormat` appears only in
probes that redirect the store to scratch (`src/probes/media/ExternalMediaProbe.cpp:676-678,
793-795, 944-946`, `src/probes/media/HlsRecoveryProbe.cpp:483`), which confirms the default is
the **Windows native registry format** and that probes must override it to stay hermetic.

Every key the app reads or writes:

| Key | Value shape | Read | Write |
|---|---|---|---|
| `ui/language` | `"system"` \| `"en"` \| `"uk"` | `src/app/I18n.hpp:8`, `src/app/I18n.cpp:200-207` | `src/app/dialogs/SettingsDialog.cpp:1518`, `:1591`, removed at `:1594`; `src/probes/native/RifeSettingsProbe.cpp:186` (probe fixture) |
| `home/feeds` | comma-joined feed ids (see `feeds::serialize`/`enabled`) | `src/app/state/Feeds.cpp:59` (`settingsKey()`), `src/app/dialogs/SettingsDialog.cpp:487`, `src/app/pages/SearchPage.cpp:697` | `src/app/dialogs/SettingsDialog.cpp:1521` |
| `home/recentTitles` | **JSON array** of title records | `src/app/pages/SearchPage.cpp:48`, parsed at `:449-455` | `src/app/pages/SearchPage.cpp:486-500` |
| `quality/urls` | **JSON object** `{"entries":{…}}` | `src/app/state/QualityCache.cpp:19` (key), `:49-53` (parse) | `src/app/state/QualityCache.cpp:111-113` |
| `player/anime4k` | Anime4K chain id | `src/player/PlayerDialog.cpp:103`, `:1032` | `src/player/PlayerDialog.cpp:1266` |
| `player/animejanai/preset` | preset id | `src/player/PlayerDialog.cpp:104`, `:1022`, `:1416` | `src/player/PlayerDialog.cpp:1555` |
| `player/animejanai/backend` | backend name | `src/player/PlayerDialog.cpp:105`, `:1024`, `:1417` | `src/player/PlayerDialog.cpp:1556`, `:1621` |
| `player/presetShortcuts/<preset id>` | key-sequence group, per preset id | `src/player/PresetShortcuts.hpp:18`, iterated at `src/app/dialogs/SettingsDialog.cpp:566-…` | `src/app/dialogs/SettingsDialog.cpp:1519-1525` |
| `player/native/schema` | `1` | `src/player/native/bundle/NativePrefs.hpp:36`, read `src/player/native/bundle/NativePrefs.cpp:127-137` | `src/player/native/bundle/NativePrefs.cpp:252` |
| `player/native/enabled` | bool | `src/player/native/bundle/NativePrefs.hpp:37`, read `:148-158` | `:254` |
| `player/native/backend` | stable backend name | `src/player/native/bundle/NativePrefs.hpp:38`, read `:162-175` | `:257` |
| `player/native/recipe` | recipe id | `src/player/native/bundle/NativePrefs.hpp:39`, read `:179-182` | `:260-265` (removal when empty) |
| `player/native/customModelsDir` | path | `src/player/native/bundle/NativePrefs.hpp:40`, read `:185-188` | `:268` |
| `player/native/componentsPromptDismissed` | bool (truthy) | `src/player/native/bundle/NativePrefs.hpp:44`, read `:293-299` | `:305-308` (`settings.sync()` after) |
| `player/native/runtimeChoice` | `"native"` \| `"standard"` | `src/player/native/bundle/NativeRuntimeSelection.cpp:265-267`, `:279-285` | `:287-306` (checks `settings.status()`) |

Group is `player/native` (`src/player/native/bundle/NativePrefs.cpp:113`,
`src/player/native/bundle/NativeRuntimeSelection.cpp:265`). `clearNativePreferences()`
(`:312-323`) removes the five preference keys (not the components-prompt dismissal — see the
comment at `:299-303`). Serialisation: **QSettings native format** (Windows registry), not
`QDataStream`, not `QSettings::IniFormat`.

**No `QDataStream` is used anywhere in the project** —
`grep -rn "QDataStream" src/ tests/` → 0 matches.

### 4.2 `.env` API-key file (raw text, QSaveFile-atomic)

- Location: walk **up** from `QCoreApplication::applicationDirPath()` at most 4 levels looking for
  a `.env`, else `QDir::current()/.env` — `src/backend/MikaiClient.cpp:272-286`, rationale in
  `src/backend/MikaiClient.hpp:30-31`; tests/probes override it via
  `setEnvFilePathOverride()` (`src/backend/MikaiClient.cpp:291`).
- Read: line-based, `#` comments ignored, accepts `=` **or** `:` separators, accepts the names
  `x-api-key`, `api_key`, `apikey` (lower-cased) — `src/backend/MikaiClient.cpp:293-318`.
  Environment variable `MIKAI_API_KEY` wins over the file
  (`src/backend/MikaiClient.cpp:536-537`).
- Written: `QSaveFile` with `setDirectWriteFallback(false)` — a rename-onto-target, so a failed
  replace leaves the previous key intact; payload is exactly `X-API-Key: <key>\n`, or an empty
  file when clearing — `src/backend/MikaiClient.cpp:318-347`. `.env` is git-ignored
  (`.gitignore:2`).
- Serialisation: **raw text** (`QFile`/`QSaveFile` byte writes), not Qt serialisation.

### 4.3 `QualityCache` — TTL, cap, debounce, storage

Defined at `src/app/state/QualityCache.cpp:13-15` and documented at
`src/app/state/QualityCache.hpp:11-19`:

| Constant | Value | Purpose (from the header comment) |
|---|---|---|
| `kQualityCacheTtlMs` | **1 800 000 ms (30 min)** | "Stream URLs are signed and expire, so a cached entry lives for this long only" |
| `kQualityCacheMaxEntries` | **240** | "Cap, so a long browsing session cannot grow the settings blob without end" |
| `kQualityCacheFlushIntervalMs` | **5 000 ms** | "Shortest gap between two writes of the settings blob" |

Storage is the **QSettings key `home/…`-style blob under `quality/urls`**
(`src/app/state/QualityCache.cpp:19`), holding a **QJsonDocument (Compact) string**:
`{"entries": {<embed url>: {"at": <epoch seconds>, "heights": {"<height>": "<url>"}}}}`
(read `:55-85`, write `:94-113`). Expiry is applied on load, on flush and on every lookup
(`:34-39`, `:36`, `:131-133`). Serialisation: **`QJsonDocument` payload inside a QSettings
value** — not `QDataStream`.

### 4.4 `verified.json` native cache (JSON, `mikai.native.cache/2`)

`src/player/native/bundle/NativeBundle.cpp:1108-1112`:

```cpp
constexpr const char* kCacheFileName = "verified.json";
// Bumped when the entry shape changes, so a cache written by an older build is
// ignored rather than half-read: version 2 records the root on a stock entry ...
constexpr const char* kCacheSchema = "mikai.native.cache/2";
```

Path: `nativeStorageRoot()/verified.json` (`:1117-1122`), where `nativeStorageRoot()` is the
executable-local `nativeRoot()` (`:465-467` → `<exe dir>/native`,
`src/player/native/bundle/NativePaths.cpp:302-307`); `MIKAI_NATIVE_CACHE` in the environment
overrides it for tests (`:1119-1120`). Read ignores any document whose `schema` member differs
(`:1124-1132`); write is a `QSaveFile` + `QJsonDocument::toJson(Compact)` (`:1134-1141`).
Real content in the assembled folder (`dist-mikai/native/verified.json`) shows the shape:

```json
{"schema":"mikai.native.cache/2","stock":[{"descriptor":{"deps":[],"descriptorId":"20c2174…",
 "files":[{"bytes":122146816,"path":"libmpv-2.dll","sha256":"90de8f89…"}],"modulePath":"…/dist-mikai/mpv/libmpv-2.dll",
 "root":"…/dist-mikai/mpv","version":"stock-20c217472c01"},"fingerprint":"29e71a9a…","modulePath":"…",
 "root":"…"}],"validations":[{"checkedAt":"2026-10-09T03:54:09Z","fingerprint":"29e71a9a…","root":"F:/ZKS/mikai_janai/dist-mikai/mpv", …
```

i.e. per-module descriptor + fingerprint + validation timestamp records — **QJson
serialisation**.

### 4.5 Install records (JSON per component/model/asset)

| Record | File | Schema constant | Writer (file:line) | Format |
|---|---|---|---|---|
| TensorRT component | `component.json` | `mikai.native.components/1` (`:24`) | `src/player/native/components/NativeComponentStore.cpp:180-189` | `QJsonDocument(Compact)` via `QSaveFile` (`:15`, `:180`) |
| RIFE archive | `install.json` | `mikai.native.rife/1` (`:27`) | `:461-471` | idem |
| AnimeJaNai install | `install.json` (under the component dir) | — | `src/player/native/components/AnimeJanaiInstallation.cpp:226` | `QSaveFile` |
| stored model record | `record.json` | `mikai.native.model/2` (`:38`) | `src/player/native/models/NativeModelInventory.cpp:389`, manifest key at `:407` | `QJsonDocument(Compact)` via `QSaveFile` (`:25`) |
| spatial asset manifest | `manifest.json` | `mikai.native.spatial-asset/1` (`:34`) | `src/player/native/models/NativeSpatialAssets.cpp:34`, `:37-39` (`graph.onnx`, `NOTICE.txt`) | JSON |
| artifact manifest | `manifest.json` | — | `src/player/native/prepare/NativeArtifactStore.cpp:22` | JSON |
| frame-fixture reference | `reference.json` | `mikai.frameReference` v1 (`:24`, `:438-439`) | `src/player/native/prepare/NativeFrameReference.cpp:22`, tensors as `input-<i>-<dtype>.bin` / `output-<i>-<dtype>.bin` (`:407`, `:411`) | JSON manifest + **raw binary** tensor dumps |
| user recipe | `<sha256>-<backend>.json` under `native/recipes/` | suffix `.json` (`:25`), id `recipe:sha256:<hex>:<backend>` (`:87-89`) | `src/player/native/models/NativeUserRecipes.cpp:96-98`, `:462-476`; root `:313`, `:337` | JSON |
| graph provenance | `<sha256>.onnx.source.json` | — | `src/player/native/components/AnimeJanaiRuntime.cpp:160-173` (source URL, commit, upstream file, sha256, licence fields) | JSON, written through the `writeIni()` helper |
| AnimeJaNai config | `animejanai/config/<safe preset>.ini` | `config_version=2` | `src/player/native/components/AnimeJanaiRuntime.cpp:131`, `:247-249`, `:340-342` | **raw INI text** through `writeIni()` + `QSaveFile` (`:145-158`) |
| AnimeJaNai stats | `animejanai/stats/<safe preset>.txt` | — | `AnimeJanaiRuntime.cpp:252`, `:343-348` | raw text |

Samples of the shipped install records (read-only evidence):
`dist-mikai/native/components/rife/3.7.0/install.json` begins
`{"archive":{"asset":"component-rife.7z","assetBytes":462444035,"assetSha256":"d41de2b5…","name":"rife-3.7.0","url":"…"},"files":[{"bytes":10699117,"path":"rife_v4.7.onnx","sha256":"8ec8fc75…"}, …]}`;
`dist-mikai/native/components/aji/v0.9.0/trt/install.json` begins
`{"abi":8,"acquisitions":[],"archiveBytes":0,"archiveSha256":"","backend":"trt","files":[{"bytes":313344,"path":"aji.dll","sha256":"59a06a64…"}, …]}`.

All of the above are JSON (`QJson*`) or raw text/INI; **none use `QDataStream`**.

### 4.6 Logs

- Session log: `mikai.log` beside the executable, rotated to `mikai.log.1` past **2 MiB**
  (`src/app/CrashLog.cpp:41-43` names; `src/app/CrashLog.cpp:22` `kMaxLogBytes = 2*1024*1024`;
  rotation `:616-631`). The directory is the exe's own folder read from the loader, not
  `QDir::currentPath()` and not `applicationDirPath()` (`:722-738`).
- Crash reports: `mikai-crash-<timestamp>.txt`, written by the unhandled-exception filter through
  fixed-buffer Win32 `CreateFileW`/`WriteFile` (no Qt allocation on a possibly-corrupt heap);
  previous crash reports are announced in the new session's log (`src/app/CrashLog.cpp:43`,
  `:632-641`, `readme.md:148-160`).
- Redaction at the logging boundary: `mikai::backend::redactDiagnostic()` replaces each sensitive
  run with one fixed `<redacted>` marker (`readme.md:150-160`, `src/backend/Diagnostics.hpp:10`).
- `dist-mikai/mikai.log` (120 509 B) is the observed runtime instance of this file.

### 4.7 Exchanged (network) data — formats, for completeness

- API client: JSON request/response over HTTPS, parsed with `QJsonDocument::fromJson`
  (`src/backend/MikaiClient.cpp:229`, `searchFromPayload` `:252-270`;
  `src/backend/Errors.cpp:19`). Authentication is the `X-API-Key` request header (`:333`).
- Recent-titles and quality blobs are the only JSON documents stored inside QSettings; every
  other JSON document crossing the network is parsed in memory, never persisted.

---

## 5. Baseline evidence at commit `6bfa80f`

**This section is inherited evidence from the pre-migration baseline, recorded here for the
migration's reference. None of it was re-run or re-verified while producing this inventory.**

| Item | Record |
|---|---|
| Commit | `6bfa80ffc0af04e7606dc19f87a48284ae3f3d9d` — `i18n: keep the lupdate location hints in the catalogue` (`git log -1 --format="%H %s"`) |
| Branch | `migrate-to-qt-6.12` (`git rev-parse --abbrev-ref HEAD`) |
| Working tree at the baseline | **clean** — `git status --porcelain` produced no output (`wc -l` → 0) *before* the inventory began. (By the time this document was written the working tree carried concurrent migration edits — `git diff --stat` showed `CMakeLists.txt`, `CMakePresets.json` and `src/probes/player/EnginePopupProbe.cpp` modified — which are recorded in §1.8 and are the live state, not baseline `6bfa80f`.) |
| Test suite | `ctest --test-dir build/release` = **4/4 PASS** |
| `mikai_tests` | all suites PASS, `TestLiveOracle` **SKIP** without a key (gated on `MIKAI_LIVE=1` + `MIKAI_API_KEY`; the suite "is a deliberate skip, not a pass, when the key is absent" — `AGENTS.md:520-524`, `:793`) |
| `NativeStageContracts` | **86/86** (`AGENTS.md:794`); the entry is registered at `CMakeLists.txt:161-167`, timeout 300 s (`:169-170`) |
| `LauncherContracts` | real hung child: exit 3, the child and its descendant gone by PID+creation-time identity, owned scratch removed, neighbouring files byte-identical (`AGENTS.md:794-796`); registered at `CMakeLists.txt:249-264`, timeout 180 s (`:266-269`) |
| `DistContracts` | **83 checks**, including byte-preservation of a populated output (`AGENTS.md:796`); registered at `CMakeLists.txt:289-295`, timeout 300 s (`:296`) |
| Added remediation suites | `TestDownloads`, `TestDiagnostics`, `TestApiClient`, `TestRunner` (21/21) and `TestRuntimeSelection` (`AGENTS.md:796-798`) |
| Compiler warnings | **zero** under `-Wall -Wextra`, for `mikai`, `mikai_tests` and every affected probe (`AGENTS.md:790-793`); the flags are set at `CMakeLists.txt:14-16` |
| Translations | `lrelease` **1016 finished, 0 unfinished** (`AGENTS.md:805`) |
| Configure | `cmake --preset release` configures (`AGENTS.md:790`); presets in `CMakePresets.json:5-47` |

Prose source for the above, quoted exactly as recorded:

> "`ctest --test-dir build/release` is **4/4 PASS**: `mikai_tests` (all suites PASS,
> `TestLiveOracle` SKIP without a key), `NativeStageContracts` (86/86), `LauncherContracts`
> (real hung child: exit 3, the child and its descendant gone by PID+creation-time identity, the
> owned scratch removed, neighbouring files byte-identical) and `DistContracts` (83 checks,
> including byte-preservation of a populated output). … `crash_probe --crash` wrote a real report
> … and `lrelease` reports 1016 finished, 0 unfinished translations."
> — `AGENTS.md:792-806` ("Observed acceptance state", remediation session 2026-10-11)

The catalogue count is independently reproducible from the .ts: `grep -c "<message" translations/mikai_uk.ts`
→ `1016`, with 0 `unfinished`/`vanished` entries.

---

## Appendix — commands used for this inventory

```
get-Item VersionInfo   (Get-Item 'C:/temp/qt6/6.8.3/mingw_64/bin/Qt6Core.dll').VersionInfo
                       (Get-Item 'C:/temp/qt6/6.12.0/mingw_64/bin/Qt6Core.dll').VersionInfo
                       (Get-Item 'F:/ZKS/mikai_janai/build/release/mikai.exe').VersionInfo
windeployqt --version  6.8.3      (6.8.3 install)
                       6.12.0     (6.12.0 install)
g++ --version          g++.exe (GCC) 16.2.0
grep QT_VERSION / QT_GCC_* <qt>/mingw_64/mkspecs/qconfig.pri
objdump -p mikai.exe            | grep "DLL Name"    (build/release and dist-mikai copies)
objdump -p Qt6Core.dll          | grep "DLL Name"    (both installs)
objdump -p qwebp.dll            | grep "DLL Name"    (libstdc++-6.dll ⇒ MinGW build)
objdump -f qwebp.dll                              (PE32+ x86-64)
find C:/temp/w64dev/w64devkit -name 'libstdc++*'      (only .a / .la / .py / .json — no .dll.a)
ls C:/temp/w64dev/w64devkit/lib/gcc/x86_64-w64-mingw32/   (single dir: 16.2.0)
ls C:/temp/qt6/6.8.3/mingw_64/bin/  | grep '^lib'        (the three MinGW runtime DLLs)
find dist-mikai -type f | sort
find dist-mikai -type d | sort
git log -1 --format="%H %s" ; git status --porcelain ; git rev-parse --abbrev-ref HEAD ; git diff --stat
git show 6bfa80f:CMakePresets.json
grep -c "<message" translations/mikai_uk.ts            1016
grep -c "type=\"unfinished\"" translations/mikai_uk.ts 0
grep -rn "Qt6::" --include=CMakeLists.txt .
grep -rn "find_package" --include=CMakeLists.txt --include=*.cmake .
grep -rn "#include <Qt" src/ tests/
grep -rniE "<facility pattern>" src/ tests/ native/ CMakeLists.txt   (§2.4, all zero except the
                                                                      protobuf text matches)
grep -n "CMakeLists\\.txt:[0-9]" docs/qt612/inventory.md            (citation cross-check)
```

Note on method: file:line citations were re-verified against the working tree **after** the
concurrent migration diff landed (§1.8); every `CMakeLists.txt` range quoted in this document was
checked against the live file, and the `6bfa80f` baseline line numbers are given where the two
differ (§2.1).
