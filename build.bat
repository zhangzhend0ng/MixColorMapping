@echo off
REM One-shot build without cmake. Tries MSVC (cl.exe) first, falls back to g++.
REM Output: build\color_match_batch.exe
setlocal enabledelayedexpansion

cd /d "%~dp0"
if not exist build mkdir build

set SRC=src\main.cpp src\match_search.cpp src\color_io.cpp vendor\filament_mixer.cpp vendor\prusa_fdm_mixer.cpp

where cl >nul 2>&1
if not errorlevel 1 (
    echo [build] cl /std:c++17 /O2 /EHsc /W4 /permissive-
    cl /nologo /std:c++17 /O2 /EHsc /W4 /permissive- ^
       /Isrc /Ivendor ^
       %SRC% ^
       /Fe:build\color_match_batch.exe /Fo:build\
    if not errorlevel 1 goto :ok
    echo [warn] MSVC build failed, trying g++...
)

where g++ >nul 2>&1
if errorlevel 1 (
    echo [error] Neither cl.exe nor g++ found on PATH.
    echo         Run from "x64 Native Tools Command Prompt for VS", or install MinGW g++.
    exit /b 1
)

echo [build] g++ -std=c++17 -O2
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic ^
    -Isrc -Ivendor ^
    %SRC% ^
    -o build\color_match_batch.exe
if errorlevel 1 (
    echo [error] g++ build failed
    exit /b 1
)

:ok
del /q build\*.obj 2>nul
echo [ok] build\color_match_batch.exe
