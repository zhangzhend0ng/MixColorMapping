#!/usr/bin/env bash
# One-shot build without cmake. Requires g++ or clang++ on PATH.
# Output: build/color_match_batch
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p build

CXX="${CXX:-g++}"
SRC=(
    src/main.cpp
    src/match_search.cpp
    src/color_io.cpp
    vendor/filament_mixer.cpp
    vendor/prusa_fdm_mixer.cpp
)

# Static-link libgcc/libstdc++ so the binary runs on machines without MinGW
# runtime DLLs on PATH (the C++ code has no other dynamic deps).
echo "[build] $CXX -std=c++17 -O2 (static runtime)"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    -static-libgcc -static-libstdc++ \
    -Isrc -Ivendor \
    "${SRC[@]}" \
    -o build/color_match_batch

echo "[ok] build/color_match_batch"
