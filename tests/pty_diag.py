#!/usr/bin/env python3
"""Diagnose why SetParity fails on a PTY.

Checks what the kernel actually accepts for a PTY: which baud constants are
honoured, whether PARENB can be toggled after the port is open, and what
tcsetattr returns for each attempt.
"""

import os
import pty
import termios
import time


def describe(fd, label):
    attrs = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
    print(f"{label}:")
    print(f"  cflag  = 0x{cflag:08x}"
          f"  CS8={'CS8' if (cflag & termios.CSIZE) == termios.CS8 else 'other'}"
          f"  PARENB={'on' if cflag & termios.PARENB else 'off'}"
          f"  PARODD={'on' if cflag & termios.PARODD else 'off'}"
          f"  CRTSCTS={'on' if cflag & termios.CRTSCTS else 'off'}")
    print(f"  iflag  = 0x{iflag:08x}  INPCK={'on' if iflag & termios.INPCK else 'off'}")
    print(f"  speed  = ispeed {ispeed} ospeed {ospeed}")


def try_set(fd, label, **kw):
    """Try tcsetattr with the given modifications; report the result."""
    try:
        attrs = termios.tcgetattr(fd)
    except OSError as e:
        print(f"{label}: tcgetattr FAILED: {e}")
        return False

    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs

    if kw.get("even") is True:
        cflag |= termios.PARENB
        cflag &= ~termios.PARODD
    elif kw.get("even") is False:
        cflag &= ~termios.PARENB
        cflag &= ~termios.PARODD
    if "speed" in kw:
        ispeed = kw["speed"]
        ospeed = kw["speed"]

    try:
        termios.tcsetattr(fd, termios.TCSANOW,
                          [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])
        print(f"{label}: OK")
        return True
    except OSError as e:
        print(f"{label}: FAILED: {e}")
        return False


def main():
    master, slave = pty.openpty()
    name = os.ttyname(slave)
    print(f"pty: {name}\n")

    # Open the way the tool does.
    fd = os.open(name, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    print("opened with O_RDWR|O_NOCTTY|O_NONBLOCK")

    import fcntl
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
    print("cleared O_NONBLOCK\n")

    describe(fd, "after open")

    print()
    try_set(fd, "set B115200", speed=termios.B115200)
    try_set(fd, "set even parity", even=True)
    try_set(fd, "set no parity", even=False)
    try_set(fd, "set B230400", speed=termios.B230400)
    try_set(fd, "set B460800", speed=termios.B460800)
    try_set(fd, "set B921600", speed=termios.B921600)

    print()
    describe(fd, "final")

    print("\n--- modem bits ---")
    for bit_name in ("TIOCM_DTR", "TIOCM_RTS", "TIOCM_CTS", "TIOCM_DSR"):
        bit = getattr(termios, bit_name, None)
        if bit is None:
            print(f"  {bit_name}: not defined")
            continue
        try:
            bits = fcntl.ioctl(fd, termios.TIOCMGET, b"\x00" * 4)
            import struct
            val = struct.unpack("i", bits)[0]
            print(f"  {bit_name}: {'set' if val & bit else 'clear'}")
        except OSError as e:
            print(f"  {bit_name}: TIOCMGET failed: {e}")
            break

    print("\n--- baud round trip ---")
    for name_, const in (("B2400", termios.B2400), ("B9600", termios.B9600),
                         ("B115200", termios.B115200)):
        if try_set(fd, f"  set {name_}", speed=const):
            ig, og, cf, lf, isp, osp, cc = termios.tcgetattr(fd)
            print(f"    read back: ispeed={isp} ospeed={osp} "
                  f"(requested {const})")

    os.close(fd)
    os.close(slave)
    os.close(master)


if __name__ == "__main__":
    main()
