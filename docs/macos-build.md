macOS 13 build guide / macOS 13 构建指南
=========================================

Target: macOS 13 (Ventura) in a VMware VM, no GPU acceleration.
Everything below is command line only; no graphics are involved at any point.

The commands are meant to be pasted into Terminal one block at a time.


------------------------------------------------------------------------------
Step 0 - check what you already have
------------------------------------------------------------------------------

    sw_vers
    xcode-select -p        # should print a path; if not, see step 1
    uname -m               # arm64 for Apple silicon, x86_64 for Intel

macOS 13 ships the Command Line Tools separately from Xcode. The compiler and
CMake are what we need; you do not have to install the full Xcode IDE.


------------------------------------------------------------------------------
Step 1 - install the command line tools (skip if step 0 found them)
------------------------------------------------------------------------------

    xcode-select --install

A dialog appears; click Install and wait. This brings in clang, make, git and
the SDK headers. Verify afterwards:

    clang --version
    xcode-select -p


------------------------------------------------------------------------------
Step 2 - install Homebrew
------------------------------------------------------------------------------

Homebrew is the usual way to get cmake and hidapi on macOS.

    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

On an Apple silicon machine Homebrew installs under /opt/homebrew and is NOT on
the PATH by default. The installer prints two commands to run; they look like:

    echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zprofile
    eval "$(/opt/homebrew/bin/brew shellenv)"

On an Intel machine the prefix is /usr/local and the PATH is already set up.

Verify:

    brew --version


------------------------------------------------------------------------------
Step 3 - install the build and HID dependencies
------------------------------------------------------------------------------

    brew install cmake hidapi pkg-config

hidapi is what the macOS USB HID backend uses (it wraps IOKit). cmake gives you
a much newer CMake than the one bundled with the tools.

Verify:

    cmake --version
    pkg-config --modversion hidapi-hidraw 2>/dev/null || brew list hidapi


------------------------------------------------------------------------------
Step 4 - get the sources onto the VM
------------------------------------------------------------------------------

Copy the newisp-cross directory into the VM. Any method is fine: a shared
folder, scp, git, or a zip dragged into the window.

The build directory should NOT live on a shared folder if you can avoid it:
shared-folder filesystems (VMware HGFS, Parallels, virtiofs) are slow and
sometimes break the executable bit. Copy the tree to the VM's own disk and
build there:

    cp -R /path/to/newisp-cross ~/newisp-cross
    cd ~/newisp-cross


------------------------------------------------------------------------------
Step 5 - build
------------------------------------------------------------------------------

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j"$(sysctl -n hw.ncpu)"

Expected tail of the output:

    [100%] Built target newisp

Check the binary:

    ls -l build/newisp
    ./build/newisp --version


------------------------------------------------------------------------------
Step 6 - run it
------------------------------------------------------------------------------

    ./build/newisp            # device list, then usage
    ./build/newisp list       # serial ports and HID devices

On a VM with no USB devices passed through, the serial list will be short or
empty and the HID list will show the built-in devices only. That is expected and
still proves the platform layer works: the serial list comes from IOKit and the
HID list from hidapi/IOKit, so a non-empty result means both APIs answered.

To see everything:

    ./build/newisp list -v


------------------------------------------------------------------------------
Step 7 - unit tests (no hardware needed)
------------------------------------------------------------------------------

    cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Debug -DNEWISP_BUILD_TESTS=ON
    cmake --build build-tests -j"$(sysctl -n hw.ncpu)"
    ./build-tests/tests/newisp_tests

Expected:

    all tests passed

These cover the Intel HEX parser, the chip table, the family predicates, the
protocol selection rules and the packet framing. They exercise no USB or serial
API, so they run fine in a VM.


------------------------------------------------------------------------------
Step 8 - protocol test over a pseudo-terminal
------------------------------------------------------------------------------

This one drives the real binary against a scripted fake chip through a PTY
pair, which exercises the termios backend end to end. macOS has PTYs, and this
needs no USB device at all.

    python3 tests/pty_test.py ./build/newisp

Expected:

    PASS
    exit code      : 0
    saw wakeup 0x7F: True
    erase seen     : True
    blocks written : 4
    options written: True

If python3 is missing, macOS 13 includes it through the command line tools; if
not, `brew install python`.


------------------------------------------------------------------------------
Step 9 - real hardware (optional)
------------------------------------------------------------------------------

Passing a USB device through to a VMware macOS guest is unreliable, especially
for HID class devices, because the guest needs to own the whole USB controller
or the specific device. If you want to try:

  1. Shut the VM down.
  2. In VMware settings, add the USB device to the guest's USB devices, or set
     USB compatibility so the controller is handed over.
  3. Boot the guest and plug the board in.
  4. Confirm the guest sees it:

         system_profiler SPUSBDataType | grep -A 8 -i stc

  5. Then:

         ./build/newisp detect --hid
         ./build/newisp burn -f /path/to/ai8051u.hex --hid -F 45M

For HID access as a normal user, no udev exists on macOS; the device is opened
through IOKit and usually needs no special permission. If opening fails, check
System Settings > Privacy & Security for an input-monitoring prompt.


------------------------------------------------------------------------------
What is expected to work in a VM, and what is not
------------------------------------------------------------------------------

Works without any USB passthrough:

  * cmake configure and build
  * the unit tests
  * the PTY protocol test (full burn sequence against the fake chip)
  * serial and HID device enumeration through IOKit
  * --help, --version, argument handling

Needs USB passthrough:

  * talking to a real board over HID or a USB-serial adapter


------------------------------------------------------------------------------
Reporting back
------------------------------------------------------------------------------

If something fails, these three outputs say the most:

    sw_vers
    cmake --version && clang --version
    cd ~/newisp-cross && cmake --build build 2>&1 | tail -40

Paste them and the failure is usually obvious from the first error.
