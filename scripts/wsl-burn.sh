#!/usr/bin/env bash
# Full end-to-end burn from the Linux build against the forwarded board.
set -u

BIN="${HOME}/newisp-build/newisp"
HEX=/mnt/c/cmdisp/newisp/烧录测试文件/ai8051u.hex

echo "=== device ==="
"$BIN" list | sed -n '1,12p'
echo

echo "=== burn @ 45 MHz ==="
"$BIN" burn -f "$HEX" --hid -F 45M -v
rc=$?
echo
echo "=== exit code: $rc ==="
exit $rc
