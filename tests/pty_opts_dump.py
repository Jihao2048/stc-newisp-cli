#!/usr/bin/env python3
"""Capture the exact option-block packet the tool sends.

The human-readable log wraps at 16 bytes and is easy to miscount, so this runs
the same burn against a PTY fake chip and prints the raw payload with indices.
"""

import os
import pty
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pty_test import FakeChip, make_hex  # noqa: E402

import subprocess  # noqa: E402


def main():
    binary = sys.argv[1]
    master, slave = pty.openpty()
    name = os.ttyname(slave)

    chip = FakeChip(master)
    chip.start()

    hex_path = "/tmp/newisp-opts-test.hex"
    make_hex(hex_path, 256)

    proc = subprocess.run(
        [binary, "burn", "-f", hex_path, "-d", name, "-F", "24M"],
        capture_output=True, text=True, timeout=180)

    time.sleep(0.3)
    chip.stop()

    print(f"exit code: {proc.returncode}")
    print("=" * 66)
    print(proc.stdout)

    print("=" * 66)
    opts = chip.options
    if opts is None:
        print("FAIL: no option block was written")
        return 1

    # The fake chip records payload[3:], so the first two bytes here are the
    # 5A A5 length-marker pair that precedes the option block on the wire, not
    # option byte 0. The real block starts at offset 2.
    print(f"captured payload: {len(opts)} bytes "
          f"(2 marker bytes + 40 option bytes)")
    print()

    body = opts[2:] if len(opts) >= 42 else opts
    for i in range(0, len(body), 8):
        chunk = body[i:i + 8]
        print(f"  [{i:2d}] " + " ".join(f"{b:02X}" for b in chunk))

    print()
    if len(body) >= 40:
        count = (body[24] << 16) | (body[25] << 8) | body[26]
        print(f"opts[24..26] count  = 0x{count:06X} ({count}) "
              f"-> {count * 256} Hz")
        print(f"opts[27]    vrtrim  = 0x{body[27]:02X}")
        print(f"opts[28..30] trim   = "
              f"{body[28]:02X} {body[29]:02X} {body[30]:02X}")
        print(f"opts[31..39] block2 = "
              f"{' '.join(f'{b:02X}' for b in body[31:40])}")
        print()
        print("Expected for a calibrated 24 MHz target: opts[24..26] carries the")
        print("frequency the handshake actually measured, and opts[28..30] the")
        print("trim triple it derived -- not the 24 MHz lookup-table fallback,")
        print("which only applies when no measurement was possible.")
    else:
        print(f"FAIL: expected 40 option bytes, got {len(body)}")
        return 1

    os.close(slave)
    os.close(master)
    return 0


if __name__ == "__main__":
    sys.exit(main())
