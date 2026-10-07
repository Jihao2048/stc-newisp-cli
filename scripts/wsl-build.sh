#!/usr/bin/env bash
# Configure and build newisp on Linux from inside WSL.
#
# The tree lives on the Windows filesystem, which is visible under /mnt/c but
# is slow for builds and does not preserve the executable bit. The build
# therefore happens in a directory under $HOME and only the source is read from
# the mount.
set -euo pipefail

SRC=/mnt/c/cmdisp/newisp-cross
BUILD="${HOME}/newisp-build"

echo "=== configure ==="
cmake -S "$SRC" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    2>&1 | tail -25

echo
echo "=== build ==="
cmake --build "$BUILD" -j"$(nproc)" 2>&1 | tail -60

echo
echo "=== result ==="
ls -l "$BUILD/newisp" 2>/dev/null || echo "binary NOT produced"
