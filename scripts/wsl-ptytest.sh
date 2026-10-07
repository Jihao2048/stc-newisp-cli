#!/usr/bin/env bash
# Run the PTY end-to-end protocol test against the built binary.
set -euo pipefail
python3 /mnt/c/cmdisp/newisp-cross/tests/pty_test.py "$HOME/newisp-build/newisp"
