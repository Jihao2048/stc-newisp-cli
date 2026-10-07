#!/usr/bin/env bash
# Verify the overclock band is gone from the command line.
set -u

BIN="${HOME}/newisp-build/newisp"

run() {
    echo "--- \$ newisp $* ---"
    "$BIN" "$@" 2>&1 | head -6
    echo "    exit: $?"
    echo
}

echo "=== 1. -O is rejected ==="
run -O

echo "=== 2. --overclock is rejected ==="
run --overclock

echo "=== 3. 47 MHz is rejected with a clear message ==="
run burn -f /nonexistent.hex --hid -F 47M

echo "=== 4. 50 MHz is rejected ==="
run burn -f /nonexistent.hex --hid -F 50M

echo "=== 5. 45 MHz is accepted (the new maximum) ==="
run burn -f /nonexistent.hex --hid -F 45M

echo "=== 6. help no longer mentions overclock ==="
if "$BIN" --help | grep -qi 'overclock'; then
    echo "  FAIL: help still mentions overclock"
    "$BIN" --help | grep -i overclock
else
    echo "  OK: no overclock in the help text"
fi
echo

echo "=== 7. help shows the 45 MHz ceiling ==="
"$BIN" --help | grep -A 2 -- '-F, --freq'
