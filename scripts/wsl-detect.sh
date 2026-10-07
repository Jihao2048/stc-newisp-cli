#!/usr/bin/env bash
# Real-hardware check from the Linux build, against the device that
# usbipd-win forwarded into WSL.
#
# The forwarded device is 34BF:FF09, the STC USB bridge. That is a different
# product ID from the ISP interface (34BF:1001), so this exercises the
# enumeration and open path rather than a full burn; whether it answers the ISP
# probe depends on which firmware the bridge is running.
set -u

BIN="${HOME}/newisp-build/newisp"

echo "=== list ==="
"$BIN" list
echo

echo "=== detect --hid (device chosen automatically) ==="
"$BIN" detect --hid -v
echo "exit: $?"
echo

echo "=== detect --hid --device /dev/hidraw0 ==="
"$BIN" detect --hid --device /dev/hidraw0 -v
echo "exit: $?"
