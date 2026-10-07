#!/usr/bin/env bash
# Verify the --force-serial option and the updated HID-only messaging.
set -u

BIN="${HOME}/newisp-build/newisp"

echo "=== 1. help shows --force-serial ==="
"$BIN" --help | grep -A 3 'force-serial' || echo "NOT FOUND"

echo
echo "=== 2. unit tests still pass ==="
"${HOME}/newisp-build-tests/tests/newisp_tests" 2>/dev/null || {
    echo "  (test binary missing; rebuilding would be needed)"
}

echo
echo "=== 3. --force-serial is accepted (device open fails, but option parses) ==="
"$BIN" detect --device /dev/nonexistent-tty --force-serial
echo "exit: $?"

echo
echo "=== 4. unknown option is still rejected ==="
"$BIN" detect --no-such-option 2>&1 | head -2
echo "exit: ${PIPESTATUS[0]}"

echo
echo "=== 5. burn with --force-serial parses ==="
"$BIN" burn -f /nonexistent.hex --device /dev/nonexistent-tty --force-serial 2>&1 | head -4
