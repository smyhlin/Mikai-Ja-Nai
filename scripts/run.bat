@echo off
REM mikai launcher. The app is a C++/Qt6 executable built by CMake.
setlocal
cd /d "%~dp0.."

set "BUILD_DIR=build-cmake"
set "APP_EXE=%BUILD_DIR%\mikai.exe"
set "TOOLCHAIN=C:/temp/w64dev/w64devkit"
set "QT_PREFIX=C:/temp/qt6/6.8.3/mingw_64"

if exist "%APP_EXE%" goto :launch

echo [mikai] %BUILD_DIR%\mikai.exe not found. Build it with:
echo.
echo   cmake -S . -B %BUILD_DIR% -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM=%TOOLCHAIN%/bin/make.exe -DCMAKE_CXX_COMPILER=%TOOLCHAIN%/bin/g++.exe -DCMAKE_PREFIX_PATH=%QT_PREFIX% -DMPV_RUNTIME_DIR="%%LOCALAPPDATA%%/MPVBackend/runtime"
echo   cmake --build %BUILD_DIR% --target mikai
echo.
pause
exit /b 1

:launch
REM Dynamic Qt + libmpv: the DLLs have to sit beside the exe. build-cmake/
REM already carries a windeployqt drop plus libmpv-2.dll; for a fresh tree run
REM windeployqt on the exe and copy libmpv-2.dll in from MPV_RUNTIME_DIR.
if not exist "%BUILD_DIR%\Qt6Core.dll" (
    echo [mikai] %BUILD_DIR% is missing its Qt DLLs. Run:
    echo [mikai]   windeployqt "%CD%\%APP_EXE%"
    echo.
)

start "" "%APP_EXE%" %*
exit /b 0
