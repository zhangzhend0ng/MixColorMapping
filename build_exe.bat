@echo off
REM Build a single self-contained color_mix_web.exe.
REM Output: dist\color_mix_web.exe
REM
REM Embeds: Python interpreter + openpyxl + compiled CLI + static HTML.
REM End users need nothing else installed - just double-click the .exe.
setlocal enabledelayedexpansion

cd /d "%~dp0"

echo [1/3] Ensure compiled CLI exists
if not exist "build\color_match_batch.exe" (
    echo      build\color_match_batch.exe missing, building via build.bat ...
    call build.bat
    if errorlevel 1 (
        echo [error] CLI build failed. Run build.sh in git-bash first, or fix build.bat.
        exit /b 1
    )
    if not exist "build\color_match_batch.exe" (
        echo [error] CLI binary still not found after build.bat.
        exit /b 1
    )
) else (
    echo      build\color_match_batch.exe OK
)

echo [2/3] Ensure PyInstaller + openpyxl
REM Prefer the official "py" launcher (ships with Python installer, always on
REM PATH); fall back to "python" if py is unavailable.
set "PY=py"
where py >nul 2>&1
if errorlevel 1 (
    set "PY=python"
    where python >nul 2>&1
    if errorlevel 1 (
        echo [error] Neither 'py' nor 'python' found on PATH. Install Python 3 from python.org.
        exit /b 1
    )
)
%PY% -m pip install --quiet --upgrade pyinstaller openpyxl
if errorlevel 1 (
    echo [error] pip install failed.
    exit /b 1
)

echo [3/3] PyInstaller --onefile bundle
REM  --onefile  : single exe output
REM  --console  : keep a window so the user sees the URL and can Ctrl+C
REM               to stop the server cleanly (important for non-tech users)
REM Data files: bundle the static HTML dir and the CLI binary so they are
REM extractable at runtime via sys._MEIPASS.
if exist build\pyi rmdir /s /q build\pyi

REM Icon + version info (optional — warn but continue if absent, so the build
REM still works on a fresh checkout without the assets).
set ICON_ARG=
set VER_ARG=
if exist "assets\app.ico" (
    set ICON_ARG=--icon assets\app.ico
) else (
    echo [warn] assets\app.ico not found - building without custom icon.
)
if exist "assets\version_info.txt" (
    set VER_ARG=--version-file assets\version_info.txt
) else (
    echo [warn] assets\version_info.txt not found - building without version metadata.
)

%PY% -m PyInstaller --noconfirm --clean --onefile --console ^
    --name color_mix_web ^
    --distpath dist ^
    --workpath build\pyi ^
    !ICON_ARG! ^
    !VER_ARG! ^
    --add-data "web\static;static" ^
    --add-data "build\color_match_batch.exe;build" ^
    web\app.py
if errorlevel 1 (
    echo [error] PyInstaller failed.
    exit /b 1
)

echo.
echo [ok] dist\color_mix_web.exe
echo      Double-click to launch, then open http://localhost:8008
echo      (Ctrl+C in the window to stop.)
endlocal
