#!/usr/bin/env bash
# Configure, build and run the unit tests on Linux from inside WSL.
set -euo pipefail

SRC=/mnt/c/cmdisp/newisp-cross
BUILD="${HOME}/newisp-build-tests"

echo "=== configure (tests on) ==="
cmake -S "$SRC" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DNEWISP_BUILD_TESTS=ON \
    2>&1 | tail -12

echo
echo "=== build ==="
cmake --build "$BUILD" -j"$(nproc)" 2>&1 | tail -40

echo
echo "=== run ==="
"$BUILD/tests/newisp_tests"
