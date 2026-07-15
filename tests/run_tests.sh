#!/usr/bin/env bash
# Build & run the golden regression tests. Exit code 0 = pass.
set -euo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-g++}"
mkdir -p build

echo "[test] building golden checks"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Isrc -Ivendor \
    tests/test_golden.cpp \
    vendor/filament_mixer.cpp vendor/prusa_fdm_mixer.cpp \
    -o build/test_golden

echo "[test] running"
./build/test_golden
