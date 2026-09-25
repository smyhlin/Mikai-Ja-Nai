@echo off
setlocal
cd /d "%~dp0.."
where uv >NUL 2>NUL
if errorlevel 1 (
    echo [mikai] 'uv' not found on PATH. Install it: https://docs.astral.sh/uv/
    pause
    exit /b 1
)
call uv sync
if errorlevel 1 (
    echo [mikai] 'uv sync' failed.
    pause
    exit /b 1
)
call uv run mikai-wrapper
if errorlevel 1 (
    echo [mikai] app exited with an error.
    pause
    exit /b 1
)
