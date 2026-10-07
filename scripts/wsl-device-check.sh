#!/usr/bin/env bash
# Run the Linux build against the board that usbipd-win forwarded into WSL.
set -u

BIN="${HOME}/newisp-build/newisp"

echo "=== newisp list ==="
"$BIN" list

echo
echo "=== newisp list -v (all HID devices) ==="
"$BIN" list -v | head -20

echo
echo "=== permissions on the hidraw node ==="
ls -l /dev/hidraw* 2>/dev/null || echo "  no hidraw nodes"
echo "  current user: $(id -un)"
