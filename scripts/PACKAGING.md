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
