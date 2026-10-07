#!/usr/bin/env python3
"""End-to-end test of the ISP protocol over a pseudo-terminal.

A PTY pair gives the tool a real character device that behaves like a serial
port: termios settings, DTR/RTS ioctls and select() all work. A scripted fake
chip answers on the master side, so the whole stack -- serial channel, probing,
handshake, erase, program and option write -- is exercised without hardware.

The fake chip implements the STC32-family protocol as the tool expects it. What
this validates is our side of the wire: framing, checksums, the sequence of
commands and the handling of each reply. It cannot validate that the real
silicon agrees with the byte layout, which only hardware can show.

Usage: pty_test.py <path-to-newisp-binary>
"""

import os
import pty
import select
import struct
import subprocess
import sys
import threading
import time

# Chip identity used by the fake target: STC8H3K60S4, magic 0xF743.
#
# This part has a serial ISP monitor, which is what the test needs: the 8051U
# family (magic 0xF8B4) is HID-only and the tool correctly refuses to program it
# over a COM port, so it cannot be used to exercise the serial path.
MAGIC = 0xF743
BSL_VERSION = 0x72
BSL_STEPPING = ord('U')
CLOCK_HZ = 24000000


def checksum16(data: bytes) -> int:
    return sum(data) & 0xFFFF


def build_packet(payload: bytes, direction: int = 0x68) -> bytes:
    """Frame a payload the way the STC ISP expects on the wire."""
    length = len(payload) + 6
    body = bytes([direction, (length >> 8) & 0xFF, length & 0xFF]) + payload
    cs = checksum16(body)
    return bytes([0x46, 0xB9]) + body + bytes([(cs >> 8) & 0xFF, cs & 0xFF, 0x16])


def parse_packet(buf: bytearray):
    """Extract one packet from buf, or None. Removes what it consumed."""
    for i in range(len(buf) - 5):
        if buf[i] != 0x46 or buf[i + 1] != 0xB9:
            continue
        direction = buf[i + 2]
        if direction not in (0x68, 0x6A):
            continue
        length = (buf[i + 3] << 8) | buf[i + 4]
        if length < 6:
            continue
        payload_len = length - 6
        end = i + 5 + payload_len + 2
        if end >= len(buf):
            continue
        if buf[end] != 0x16:
            continue
        cs_in = (buf[end - 2] << 8) | buf[end - 1]
        if checksum16(bytes(buf[i + 2:end - 2])) != cs_in:
            continue
        payload = bytes(buf[i + 5:i + 5 + payload_len])
        del buf[:end + 1]
        return payload
    return None


def status_payload() -> bytes:
    """The 0x50 status packet the serial ISP monitor sends after a wakeup."""
    p = bytearray(32)
    p[0] = 0x50
    # Clock frequency, big endian, at offset 1..4.
    p[1] = (CLOCK_HZ >> 24) & 0xFF
    p[2] = (CLOCK_HZ >> 16) & 0xFF
    p[3] = (CLOCK_HZ >> 8) & 0xFF
    p[4] = CLOCK_HZ & 0xFF
    p[9] = 0xFF   # msr[0]
    p[10] = 0xFF  # msr[1]
    p[11] = 0xFF  # msr[2]
    p[15] = 0x3E  # msr[3]
    p[16] = 0xFE  # msr[4]
    p[17] = BSL_VERSION
    p[18] = BSL_STEPPING
    p[20] = (MAGIC >> 8) & 0xFF
    p[21] = MAGIC & 0xFF
    return bytes(p)


class FakeChip(threading.Thread):
    """Answers ISP commands on the master side of a PTY."""

    def __init__(self, master_fd):
        super().__init__(daemon=True)
        self.master = master_fd
        self.rx = bytearray()
        self.log = []
        self.erased = False
        self.blocks = {}
        self.options = None
        self.running = True
        self.saw_wakeup = False

    def send(self, payload: bytes):
        data = build_packet(payload)
        os.write(self.master, data)
        self.log.append(("tx", payload[:1].hex(), len(payload)))

    def handle(self, payload: bytes):
        op = payload[0] if payload else None
        self.log.append(("rx", f"{op:02x}" if op is not None else "none",
                         len(payload)))

        if op == 0x00:
            # Calibration round 1: report counts for the requested trim pairs.
            resp = bytearray([0x00, 0x08])
            for i in range(4):
                base = 0x1000 + i * 0x400
                resp += struct.pack(">HH", base, base + 0x300)
            self.send(bytes(resp))
        elif op == 0x01:
            # Baud rate set / calibration round 3.
            self.send(bytes([0x01]))
        elif op == 0x03:
            self.erased = True
            self.blocks.clear()
            self.send(bytes([0x03]) + bytes(range(1, 8)))
        elif op in (0x02, 0x22):
            addr = (payload[1] << 8) | payload[2]
            self.blocks[addr] = payload[3:]
            self.send(bytes([0x02, 0x54]))
        elif op == 0x04:
            self.options = bytes(payload[3:])
            self.send(bytes([0x04, 0x54]))
        elif op == 0x05:
            self.send(bytes([0x05]))
        elif op == 0x07:
            self.send(bytes([0x07, 0x54]))
        else:
            # Unknown command: a real monitor stays silent, and the test should
            # see the tool time out rather than invent a reply.
            self.log.append(("warn", f"unhandled op {op}", 0))

    def run(self):
        while self.running:
            try:
                r, _, _ = select.select([self.master], [], [], 0.05)
            except (OSError, ValueError):
                break
            if not r:
                continue
            try:
                data = os.read(self.master, 4096)
            except OSError:
                break
            if not data:
                break
            self.rx.extend(data)

            # The wakeup byte is not framed; note it and consume it.
            while self.rx and self.rx[0] == 0x7F:
                self.saw_wakeup = True
                del self.rx[:1]
                if not self.saw_wakeup:
                    break
                # Answer the wakeup with the status packet, once.
                if len([e for e in self.log if e[1] == "50"]) == 0:
                    self.send(status_payload())

            while True:
                pkt = parse_packet(self.rx)
                if pkt is None:
                    break
                self.handle(pkt)

    def stop(self):
        self.running = False


def make_hex(path: str, size: int = 256):
    """Write a small Intel HEX image to path."""
    lines = []
    for base in range(0, size, 16):
        chunk = bytes(((base + i) & 0xFF) for i in range(16))
        rec = bytes([16, (base >> 8) & 0xFF, base & 0xFF, 0x00]) + chunk
        cs = (-sum(rec)) & 0xFF
        lines.append(":" + (rec + bytes([cs])).hex().upper())
    lines.append(":00000001FF")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    if len(sys.argv) < 2:
        print("usage: pty_test.py <newisp binary>", file=sys.stderr)
        return 2
    binary = sys.argv[1]

    master, slave = pty.openpty()
    slave_name = os.ttyname(slave)

    chip = FakeChip(master)
    chip.start()

    hex_path = "/tmp/newisp-pty-test.hex"
    make_hex(hex_path, 256)

    print(f"pty: {slave_name}")
    print(f"hex: {hex_path}")

    cmd = [binary, "burn", "-f", hex_path, "-d", slave_name]
    print("cmd: " + " ".join(cmd))
    print("=" * 62)

    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    print(proc.stdout)
    if proc.stderr.strip():
        print("--- stderr ---")
        print(proc.stderr)

    # Give the fake chip a moment to finish handling the last command.
    time.sleep(0.3)
    chip.stop()
    os.close(slave)
    os.close(master)

    print("=" * 62)
    print(f"exit code      : {proc.returncode}")
    print(f"saw wakeup 0x7F: {chip.saw_wakeup}")
    print(f"erase seen     : {chip.erased}")
    print(f"blocks written : {len(chip.blocks)}")
    print(f"options written: {chip.options is not None}")

    ok = True
    if proc.returncode != 0:
        print("FAIL: tool reported failure")
        ok = False
    if not chip.saw_wakeup:
        print("FAIL: no wakeup byte seen")
        ok = False
    if not chip.erased:
        print("FAIL: erase command never arrived")
        ok = False
    if len(chip.blocks) != 4:
        print(f"FAIL: expected 4 program blocks, saw {len(chip.blocks)}")
        ok = False
    if chip.options is None:
        print("FAIL: option block never written")
        ok = False

    print("=" * 62)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
