# newisp

A cross-platform command-line programmer (flasher) for **STC 8051-family microcontrollers**.

`newisp` was ported from a Windows-only WinUI 3 / C++/WinRT desktop application to a portable
C++17 CLI built with CMake. It runs on Windows, macOS and Linux, and speaks to a target chip over
either a serial (UART) link or the factory USB HID ISP interface.

> **中文文档**：见 [README.zh-CN.md](README.zh-CN.md)。 / Chinese documentation:
> see [README.zh-CN.md](README.zh-CN.md).

## Table of contents

- [Features](#features)
- [Supported chip families](#supported-chip-families)
- [Transports](#transports)
- [Building](#building)
  - [Linux (Ubuntu/Debian)](#linux-ubuntudebian)
  - [macOS (Homebrew)](#macos-homebrew)
  - [Windows](#windows)
  - [Building without HID](#building-without-hid)
- [Encoding note (important)](#encoding-note-important)
- [Usage](#usage)
  - [Commands](#commands)
  - [Options](#options)
  - [Examples](#examples)
- [Testing](#testing)
- [Architecture](#architecture)
- [Hardware notes](#hardware-notes)
- [Verification status](#verification-status)
- [Limitations](#limitations)
- [License](#license)

## Features

- Single self-contained CLI binary for Windows, macOS and Linux.
- Two transports: classic STC serial ISP monitor and the factory USB HID ISP interface.
- Automatic chip identification by magic (chip ID) lookup against a table of roughly 1300 parts.
- Full burn sequence: erase, 64-byte block writes, and option-byte programming.
- Intel HEX image parsing.
- Frequency, baud rate and overclock handling with human-friendly argument forms
  (`115.2k`, `24M`, `11.0592M`, ...).

## Supported chip families

| Family | Notes |
| --- | --- |
| STC89 / STC90 | Serial only |
| STC10 / STC11 / STC12 | Includes 12A / 12B variants; serial only |
| STC15 | Includes 15A variant; serial only |
| STC8 / STC8A / STC8C / STC8F / STC8G / STC8H | Serial and USB HID |
| STC16 | Serial and USB HID |
| STC32 | Serial and USB HID |
| STC8051U / AI8051U | USB HID only (see below) |

The **8051U** family has **no serial ISP monitor at all**. It can only be programmed over USB HID.
The tool detects this case and reports it explicitly instead of failing with an obscure error.

## Transports

### Serial (UART)

The classic STC ISP monitor. The tool pulses DTR and spams the `0x7F` wakeup byte in order to catch
a chip that is still executing user code. The baud rate is negotiated with the chip, starting at
2400 baud and moving up to the user-selected rate (default `115200`).

### USB HID

The factory USB ISP interface, identified by **VID `0x34BF` / PID `0x1001`**, implemented by STC32,
STC8H, STC8G and 8051U parts. The user must hold the **BOOT pin low while plugging the board in**.
No baud rate negotiation exists over USB.

## Building

Requirements:

- CMake 3.20 or newer
- A C++17 compiler

Source layout:

| Path | Contents |
| --- | --- |
| `include/newisp/` | Public headers |
| `src/core/` | Platform-independent code: chip tables, Intel HEX parser, ISP protocols (header-only classes) |
| `src/transport/` | Platform-specific transport implementations |
| `src/cli/` | Command-line front end |

### Linux (Ubuntu/Debian)

```sh
sudo apt install build-essential cmake pkg-config libudev-dev libhidapi-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

On Linux the user usually needs to be in the `dialout` group in order to open serial ports:

```sh
sudo usermod -aG dialout $USER   # then log out and back in
```

For HID access, a udev rule may be needed. Opening a HID device as a normal user requires read and
write permission on `/dev/hidrawN`; a rule for the STC ISP interface looks like this:

```
# /etc/udev/rules.d/60-newisp.rules
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="34bf", ATTRS{idProduct}=="1001", MODE="0666"
```

Reload and re-plug afterwards:

```sh
sudo udevadm control --reload-rules && sudo udevadm trigger
```

> Under WSL2 the USB device tree is not exposed by default, so `hidraw` and `/dev/ttyUSB*` nodes are
> absent. Pass the board through with `usbipd-win` (`usbipd list`, then `usbipd attach --wsl`) if you
> want to burn from WSL itself.

### macOS

With Homebrew, on Apple silicon:

```sh
brew install cmake hidapi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

**Homebrew no longer supports Intel Macs.** Version 7.0.0 moved them to "Tier 3"
and the install script now refuses to run on anything that is not Apple silicon,
so an Intel machine needs the two dependencies built by hand. This is the route
that was used to verify the macOS port in this repository:

```sh
# 1. CMake. macOS ships python3, so pip can supply it.
python3 -m pip install --user cmake
echo 'export PATH="$HOME/Library/Python/3.9/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
cmake --version

# 2. hidapi, built from source against IOKit.
cd ~
curl -L -o hidapi.tar.gz \
  https://github.com/libusb/hidapi/archive/refs/tags/hidapi-0.14.0.tar.gz
tar xzf hidapi.tar.gz
cd hidapi-hidapi-0.14.0
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j4
sudo cmake --install build

# 3. newisp.
cd ~/newisp-cross
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

`CMAKE_POLICY_VERSION_MINIMUM=3.5` is needed because hidapi 0.14.0 asks for a
CMake older than 3.5 and CMake 4.x refuses those projects outright.

Serial ports on macOS are `/dev/cu.*`. Prefer `cu` over `tty`: opening a `tty`
waits for DCD, which a self-powered board never asserts, so the open hangs.

There is also `docs/macos-build.md`, which is a longer walkthrough of the same
steps including the Command Line Tools install.

### Windows

Use the **Visual Studio 2022 x64 developer prompt**. No external dependency is needed: the HID
backend talks to the system HID class driver through `hid.dll` and SetupAPI, both of which ship
with Windows.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

There is also `scripts/win-build.ps1`, which locates MSVC through `vcvars64.bat`, finds a CMake
installation, and configures with the Visual Studio generator. `scripts/win-smoke.ps1` exercises
the argument handling and device enumeration of the resulting binary.

The Windows build links only against `setupapi` and `hid`; nothing has to be downloaded, installed
or shipped alongside the executable.

### Building without HID

HID support can be turned off with `-DNEWISP_ENABLE_HID=OFF`, which produces a **serial-only**
binary that reports `this build has no USB HID support` when `--hid` or `list` is used.

On macOS and Linux the same thing happens automatically when `hidapi` is not installed. This is
deliberate: a missing optional dependency should not turn into a link error, and serial alone is a
working tool. On Windows the backend is built from the system HID class driver, so HID is always
available there unless it is explicitly disabled.

## Encoding note (important)

All source files are **UTF-8 with BOM**, and on MSVC the build additionally adds `/utf-8`.

Both are required because the log messages contain Chinese text. Without them, MSVC reads the
sources in the system ANSI code page (936 / GBK) and fails with `newline in constant` errors.

**Do not strip the BOMs.**

## Usage

```
newisp list                              list serial and HID devices
newisp detect   [transport] [device]     probe and identify a chip
newisp burn -f FILE [options] [device]   erase, write and set options
```

### Options

| Option | Description |
| --- | --- |
| `-s`, `--serial` | Use the serial transport (default) |
| `-H`, `--hid` | Use the USB HID transport |
| `-d`, `--device NAME` | Device to open: `COM7`, `/dev/ttyUSB0`, `/dev/cu.usbserial-*`, or a HID interface path. If omitted and exactly one candidate exists, it is used; if several exist, the tool refuses and lists them. |
| `-f`, `--file FILE` | Intel HEX image (required for `burn`) |
| `-b`, `--baud RATE` | Baud rate, default `115200`. Accepts forms such as `115200`, `115.2k`, `1M` |
| `-F`, `--freq HZ` | Target IRC frequency, default 24 MHz. Accepts forms such as `24000000`, `24M`, `11.0592M` |
| `-O`, `--overclock` | Use the 46–50 MHz overclock band (defaults to 47 MHz if no `-F` is given) |
| `-e`, `--eeprom SIZE` | EEPROM split, e.g. `4K`. `0` keeps the chip's current setting |
| `--clock internal\|external` | Clock source for the STC89/12/15 option bytes |
| `--family NAME` | Fallback family to use when probing identifies nothing |
| `-q`, `--quiet` | Reduce output |
| `-v`, `--verbose` | Increase output |
| `-h`, `--help` | Show help |
| `-V`, `--version` | Show version |

### Examples

List everything the host can see:

```sh
newisp list
```

Probe a chip on a specific serial port:

```sh
newisp detect -s -d /dev/ttyUSB0
```

Probe a chip over USB HID:

```sh
newisp detect --hid
```

Burn an image at 24 MHz:

```sh
newisp burn -f firmware.hex -d COM7
```

Burn with an explicit baud rate, frequency and EEPROM split:

```sh
newisp burn -f firmware.hex -b 115.2k -F 24M -e 4K -d /dev/cu.usbserial-1420
```

Use the overclock band (defaults to 47 MHz):

```sh
newisp burn -f firmware.hex --overclock -d COM7
```

Force a family when identification fails, with the external clock source:

```sh
newisp burn -f firmware.hex --family STC8H --clock external
```

## Testing

### Unit tests

The unit tests need no hardware and no framework. Configure with
`-DNEWISP_BUILD_TESTS=ON` and run `ctest`, or invoke the binary directly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DNEWISP_BUILD_TESTS=ON
cmake --build build -j
./build/tests/newisp_tests
```

They cover the Intel HEX parser (extended linear and segment addresses, CRLF input, lowercase
digits, truncated records, the EOF record, filling gaps with `0xFF`), the chip table (known IDs,
the unknown-ID placeholder, no null entries), the family predicates and protocol selection
(including the frequency caps, the legacy 115200 clamp, which families support the `0x05` prepare
step and which are reachable over HID), and the packet framing (exact bytes and checksums, round
trips for every protocol, tolerance of leading noise, rejection of bad checksums and lost
terminators, waiting for a packet split across two reads, and status decoding that rejects the
wrong family or an implausible clock).

### Protocol test over a pseudo-terminal

| Test / script | Purpose |
| --- | --- |
| `tests/pty_test.py` | End-to-end protocol test. Creates a pseudo-terminal (PTY) pair, runs a scripted fake STC chip on the master side, and drives the real binary against it. Validates the full burn sequence: wakeup, probe, calibration handshake, `0x05` prepare, erase, 64-byte block writes, option-byte write. Run with `python3 tests/pty_test.py /path/to/newisp`. **Linux/macOS only** (needs a PTY). |
| `tests/pty_diag.py` | Reports which termios settings a tty actually accepts (useful when a driver rejects parity). |
| `tests/pty_opts_dump.py` | Dumps the exact option-block bytes the tool writes, with decoded frequency and trim fields. |
| `scripts/wsl-build.sh` | Build inside WSL. |
| `scripts/wsl-tests.sh` | Configure with tests enabled, build and run them inside WSL. |
| `scripts/wsl-smoke.sh` | Smoke test inside WSL. |
| `scripts/wsl-ptytest.sh` | Run the PTY protocol test inside WSL. |
| `scripts/win-build.ps1` | Windows build helper. |
| `scripts/win-smoke.ps1` | Windows smoke test. |

## Architecture

```
include/newisp/
  IChannel.h            transport interface (Open/Write/Read/Flush/SetBaudRate/DTR/RTS)
  serial_channel.h      serial backend
  hid_channel.h         USB HID backend
  stc_protocol.h        protocol base: packet framing, wait/pulse helpers, shared formatting
  protocol_stc89.h      STC89/90 (ProtocolStc89 legacy 1-byte-checksum framing, ProtocolStc89A)
  protocol_stc12.h      STC10/11/12 (12A/12B variants)
  protocol_stc15.h      STC15 (15A variant)
  protocol_stc8_uart.h  STC32/STC8G/STC8 over serial
  protocol_stc8_hid.h   STC32/STC8G/STC8 over USB HID
  protocol_factory.h    chip name -> protocol
  chip_table.h          chip ID (magic) -> part name, ~1300 entries
  chip_logic.h          family predicates, frequency/baud/overclock tables
  hex_file.h            Intel HEX parser
  session.h             detect/burn sequences
  options.h             CLI options
  text.h                UTF-8 text helpers, hex formatting, monotonic clock

src/core/               platform-independent: chip table, chip logic, HEX parser
src/transport/          platform-specific: serial (Win32 or termios), USB HID
src/cli/                argument parsing, session driver, main
tests/                  unit tests and the PTY protocol test
```

The protocol layer is written against `stcisp::IChannel` and never names a concrete transport.

The STC32/STC8G/STC8 families have a separate class per transport, because their status framing and
calibration differ genuinely. STC89, STC12 and STC15 are serial-only.

### Transport backends

| Platform | Serial | USB HID |
| --- | --- | --- |
| Windows | `CreateFile` + `DCB` + `EscapeCommFunction` | `setupapi.dll` + `hid.dll` (system HID class driver) |
| macOS | `termios` + `TIOCMGET`/`TIOCMSET` | hidapi (IOKit) |
| Linux | `termios` + `TIOCMGET`/`TIOCMSET` | hidapi (hidraw) |

The Windows HID backend uses the system HID class driver directly rather than hidapi, so the Windows
build has no external dependency at all. On macOS and Linux, hidapi is packaged everywhere and keeps
the code short.

Serial enumeration is platform-specific too: the SetupAPI port class on Windows, IOKit
`IOSerialBSDClient` services on macOS, and a scan of `/dev` cross-checked against sysfs on Linux.

One behavioural note carried over from the original: opening a serial port asserts DTR on most
platforms, and on many USB-serial bridges (CH340, CP2102, FT232) that is wired to the board's reset
line. Both backends therefore clear DTR and RTS immediately after opening, so the board is not held
in reset before the reset pulse is sent deliberately.

Parity is applied on a best-effort basis. Some tty devices — pseudo-terminals in particular, and a
few USB bridge drivers — reject it with `EINVAL`. When that happens the speed and framing still
take effect, the port records the parity it actually has, and `SetParity` reports the truth rather
than claiming success. The older STC families run at 8N1 anyway.

## Hardware notes

### Overclock

46 and 47 MHz were measured to work on an AI8051U34K64; 48 MHz falls back to a much lower clock, so
the physical IRC limit sits just under 48 MHz. The 49/50 MHz entries are extrapolated and
unreliable. The STC8G family tops out near 35 MHz, and overclock is refused for it.

### Option bytes

Option bytes carry the watchdog, reset behaviour and clock configuration. A wrong value can leave a
chip unable to start, so the tool **echoes the exact MSR bytes it is about to write**. It reads the
current MSR from the status packet and writes those values back, rather than blindly writing `0xFF`.

### Serial auto-reset

If the board has no auto-reset circuit (DTR wired to reset), the user must power-cycle the board
during the handshake.

### EEPROM split

Only the STC32 relatives expose a user-selectable EEPROM/code split. Setting `-e` for a family that
does not support it is ignored.

## Verification status

What has actually been exercised for this port, so the state of the code is not overstated:

| Platform | Build | Unit tests | Smoke test | Protocol test |
| --- | --- | --- | --- | --- |
| Linux (Ubuntu 22.04, GCC 11, CMake 3.22) | yes | yes, all passing | yes | yes, full burn sequence passes against the PTY fake chip |
| Windows (MSVC 19.44, CMake 4.4) | yes | not run | yes; device enumeration finds real ports and HID devices | **yes, against an AI8051U34K64 over USB HID** |
| macOS 13.1 (AppleClang 14, CMake 4.4) | yes | not run | yes; IOKit serial scan and hidapi enumeration both answer | **yes, against an AI8051U34K64 over USB HID** |

The Windows HID enumeration was checked against the machine's actual HID devices: 17 were found and
their product strings and vendor/product IDs decoded correctly.

The macOS build was done in a VM with the dependencies built from source (Homebrew no longer
installs on Intel Macs). Its serial list came back empty, which is correct for a VM with no serial
hardware, and the HID enumeration found the board:

```
未发现串口设备
STC ISP 设备 (VID 34BF PID 1001): 1 个
  USB-ISP [34BF:1001] (DevSrvsID:4294968331)
```

### Hardware burn test (AI8051U34K64 over USB HID)

A real AI8051U34K64 core board was programmed over the factory USB HID ISP interface. Every burn
completed with exit code 0 and the full sequence was acknowledged by the chip:

| Step | Result |
| --- | --- |
| Probe | identified `AI8051U34K64`, magic `0x78B4`, BSL 7.4U |
| `0x01` baud set | answered `01` |
| `0x05` prepare | answered `05` |
| `0x03` erase | answered `03`, returned the real UID `78 B4 C9 28 09 FA 88` |
| `0x02`/`0x22` program | 13 of 13 blocks for an 810-byte image, each answered `02 54` |
| `0x07` finish | answered `07 54` |
| `0x04` options | answered `04 54` |

Three target frequencies were written and each was then read back by re-probing the chip, which is
the real test: the chip reloads the option block and the frequency counter in its status packet
reports the new clock.

| Target | `opts[24..26]` count | trim written | Re-probed clock |
| --- | --- | --- | --- |
| 24 MHz | `0x016E36` | `48 20 01` | 24.000 MHz |
| 40 MHz | `0x02625A` | `73 30 01` | — |
| 45 MHz | `0x02AEA5` | `A6 30 01` | 44.999 MHz |

The 44.999 MHz reading is normal IRC tolerance, and the vendor ISP tool reports the same value for
the same option bytes. The written option block was verified byte for byte against the constants in
`protocol_stc8_hid.h`, including the frequency count, the reference-voltage trim and the trim triple.

The same 45 MHz burn was then repeated unchanged on Windows, Linux (through usbipd into WSL) and
macOS. All three read back the identical chip UID `78 B4 C9 28 09 FA 88`, which is the strongest
evidence that the protocol implementation matches across platforms.

### The serial path on real hardware

An STC8H8K64U was programmed over serial at 460800 baud. The calibration handshake ran for real:
the tool measured the IRC, derived `trim_adj=67, trim_range=0x20, trim_divider=1` and switched the
host to 460800, after which all 265 blocks of a 16920-byte image were written and the option block
was accepted.

### A cross-platform bug that only hardware could expose

The port had a genuine bug that no amount of compiling or unit testing would have caught, because
it only appears when the same code talks to a real device through two different HID stacks:

| Platform | First byte returned by `hid_read` |
| --- | --- |
| Windows (`hid.dll`) | a **report ID** -- the HID stack reserves the first byte, so the payload starts at index 1 |
| Linux (`hidraw`) | **payload** -- the kernel hands over the report body only, so it starts at index 0 |

Skipping the first byte unconditionally shifted every packet by one on Linux, so the `46 B9` frame
marker never landed where the parser expected it and every probe reported "no chip detected".

The fix does not encode an assumption about the backend. It looks at the data: an STC packet always
begins with `46 B9`, so whichever index holds that marker is where the payload starts. That works on
all three platforms and tolerates a future change in hidapi's behaviour.

## Limitations

- The PTY test validates the **host side** of the wire protocol: framing, checksums, command
  sequence and reply handling. Its fake chip answers the STC32-family serial protocol, so the older
  families are covered by the unit tests rather than end to end.
- The PTY test is Linux/macOS only, because it needs a real terminal device pair. There is no
  equivalent on Windows, where the serial backend is a different API entirely.
- Serial burning has been exercised on Windows (COM13, STC8H8K64U, 460800 baud) and inside WSL
  through usbipd, but not on a physical Linux host with a real adapter.
- The macOS unit tests have not been run yet. The build and a real burn both succeeded there.
- The binaries are not "double-click and go". They are command-line tools with no Apple developer
  signature; handing one to someone else requires clearing the quarantine attribute
  (`xattr -d com.apple.quarantine newisp`) and keeping the executable bit, and an Intel binary needs
  Rosetta 2 on Apple silicon.

## License

This project is licensed under the **GNU General Public License v3.0**. See [LICENSE](LICENSE).
