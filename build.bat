@echo off
REM One-shot build without cmake. Uses cl.exe (MSVC) on PATH.
REM Output: build\color_match_batch.exe
setlocal enabledelayedexpansion

cd /d "%~dp0"
if not exist build mkdir build

where cl >nul 2>&1
if errorlevel 1 (
    echo [error] cl.exe not found on PATH. Run from a "x64 Native Tools Command Prompt for VS".
    exit /b 1
)

set SRC=src\main.cpp src\match_search.cpp src\color_io.cpp vendor\filament_mixer.cpp vendor\prusa_fdm_mixer.cpp

echo [build] cl /std:c++17 /O2 /EHsc /W4 /permissive-
cl /nologo /std:c++17 /O2 /EHsc /W4 /permissive- ^
   /Isrc /Ivendor ^
   %SRC% ^
   /Fe:build\color_match_batch.exe /Fo:build\

if errorlevel 1 (
    echo [error] build failed
    exit /b 1
)
del /q build\*.obj 2>nul
echo [ok] build\color_match_batch.exe
