#!/usr/bin/env bash
# Smoke-test the freshly built binary. Nothing here needs a real chip:
# every case exercises argument handling, the device list or the error paths.

BIN="${HOME}/newisp-build/newisp"
fail=0

run() {
    echo "=============================================================="
    echo "\$ newisp $*"
    echo "--------------------------------------------------------------"
    "$BIN" "$@"
    rc=$?
    echo "--- exit code: $rc ---"
    echo
    return 0
}

run --version
run --help
run list
run detect --device /dev/nonexistent-tty
run burn
run burn -f /nonexistent.hex --device /dev/nonexistent-tty
run burn -f /nonexistent.hex --baud notanumber
run badcommand
run detect --freq 24M --device /dev/nonexistent-tty

echo "=============================================================="
echo "all smoke cases executed"
