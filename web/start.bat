@echo off
REM Launch the color-mixer-batch web UI. Builds the CLI first if missing.
cd /d "%~dp0\.."

if not exist build\color_match_batch.exe (
    echo [web] CLI not built — building now...
    call build.bat || exit /b 1
)

echo [web] starting server...
python web\app.py %*
