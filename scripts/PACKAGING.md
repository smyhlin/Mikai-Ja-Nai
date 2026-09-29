# Packaging

The app is C++/Qt6, built by CMake into `build-cmake/mikai.exe`. There is no
Python left: `scripts/run.bat` launches the exe and only prints build
instructions when it is missing.

## Build

```sh
cmake -S . -B build-cmake -G "MinGW Makefiles" \
  -DCMAKE_MAKE_PROGRAM=<w64devkit>/bin/make.exe \
  -DCMAKE_CXX_COMPILER=<w64devkit>/bin/g++.exe \
  -DCMAKE_PREFIX_PATH=<qt>/6.8.3/mingw_64 \
  -DMPV_RUNTIME_DIR="%LOCALAPPDATA%/MPVBackend/runtime"
cmake --build build-cmake --target mikai
```

`MPV_RUNTIME_DIR` has no default and CMake hard-fails without it. Configure
needs the toolchain paths passed explicitly: a bare `-G "MinGW Makefiles"`
fails with `CMAKE_MAKE_PROGRAM is not set` when the toolchain is not on `PATH`.

## WebP (required)

Posters are served as `.webp`, and the app asks Qt to decode them. A base Qt
install has no WebP decoder: its `imageformats` folder holds only `qgif`,
`qico`, `qjpeg` and `qsvg`. Without `qwebp.dll` the app still runs, the
network still works, and every image is simply blank — `QPixmap` returns a null
image for a format it has no handler for and reports nothing.

So `qwebp.dll` is a hard runtime dependency, and it has to be in two places:

- the Qt install, or `windeployqt` has nothing to copy:
  `<qt>/6.8.3/mingw_64/plugins/imageformats/qwebp.dll`
- the deployed bundle, which `windeployqt` then places automatically.

The plugin must match the Qt build's toolchain. A MinGW app needs the
`win64_mingw` plugin; an MSVC one will not load it, and the reverse is also
true. Check with `objdump -p qwebp.dll | grep "DLL Name"` — a MinGW build
imports `libstdc++-6.dll`, an MSVC build imports `VCRUNTIME140.dll`.

This Qt tree was installed with `aqtinstall`, so it has no Maintenance Tool.
The module came from the Qt online repository instead:

```sh
curl -LO https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_680/qt6_680/qt.qt6.680.addons.qtimageformats.win64_mingw/6.8.0-0-202410030750qtimageformats-Windows-Windows_10_22H2-Mingw-Windows-Windows_10_22H2-X86_64.7z
tar -xf 6.8.0-*.7z            # bsdtar reads 7z
cp 6.8.0/mingw_64/plugins/imageformats/qwebp.dll <qt>/6.8.3/mingw_64/plugins/imageformats/
```

`TestModels::webpIsDecodable` fails the build if the plugin is missing, and
`TestLiveOracle::aRealPosterWebpDecodes` (keyed) pulls a real poster and puts
it through the same decode the UI does.

## Run

`scripts\run.bat` builds nothing: it launches `build-cmake/mikai.exe`.

## Deploy

`mikai.exe` links Qt dynamically, so it needs the Qt runtime beside it or on
`PATH`; a fresh exe with no deployed DLLs exits with `0xC0000135`. `build-cmake/`
already carries a full `windeployqt` drop, so the ordinary build directory is
shippable as-is. For a clean tree:

```sh
windeployqt build-cmake/mikai.exe
```

`windeployqt` does not copy `libmpv-2.dll`; it lives in `MPV_RUNTIME_DIR` and
has to be copied in by hand alongside the Qt DLLs.

`mikai.exe` is `WIN32_EXECUTABLE`: it has no console window. Diagnostics go to
the debugger output (`OutputDebugString`), not stdout.

## Tests

```sh
cmake --build build-cmake --target mikai_tests
ctest --test-dir build-cmake -R mikai_tests
```

Hermetic except the live oracle, which runs only with both set:

```sh
set MIKAI_LIVE=1 & set MIKAI_API_KEY=mk_... & ctest --test-dir build-cmake -R mikai_tests
```

## Install

```cmake
install(TARGETS mikai RUNTIME DESTINATION .)
install(CODE "execute_process(COMMAND windeployqt --release --no-translations \
  --no-system-d3d-compiler --no-opengl-sw $<TARGET_FILE:mikai>)")
```

`--no-opengl-sw` is deliberate: the shell paints its own chrome and never falls
back to software OpenGL, so shipping the swiftshader DLLs would only add weight.
