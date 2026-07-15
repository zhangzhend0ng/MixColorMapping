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

echo "[build] $CXX -std=c++17 -O2"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    -Isrc -Ivendor \
    "${SRC[@]}" \
    -o build/color_match_batch

echo "[ok] build/color_match_batch"
