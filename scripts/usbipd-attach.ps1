# Attach an STC USB device to WSL through usbipd-win.
#
# Run this in an ADMINISTRATOR PowerShell: `bind` changes a driver binding on
# the host, which requires elevation. `attach` on its own does not, but keeping
# both in one script avoids a confusing half-done state.
#
# Two different STC devices turn up on this machine, and which one you want
# depends on what is being tested:
#
#   34BF:1001  the factory USB ISP interface. This is the one to forward for a
#              real burn from Linux -- it is what the HID transport talks to.
#
#   34BF:FF09  the USB bridge, which provides COM13 and COM14 on Windows. It is
#              a HID device, not a CDC/ACM port. Useful for checking that
#              enumeration works, but it does not answer the ISP probe.
#
# Once attached, the device disappears from Windows and appears inside WSL as
# /dev/hidraw* (or /dev/ttyUSB* for the bridge), which is what lets the Linux
# build talk to real hardware.
#
# USBPcap (installed with Wireshark) is known to conflict with usbipd-win, so
# --force is required on this machine.

$ErrorActionPreference = 'Stop'

$usbipd = 'C:\Program Files\usbipd-win\usbipd.exe'
$busid  = '2-10'      # verify with `usbipd list` -- the BUSID changes as boards
                      # are plugged and unplugged
$distro = 'Ubuntu-22.04'

if (-not (Test-Path $usbipd)) {
    throw "usbipd not found at $usbipd"
}

Write-Host '=== devices ==='
& $usbipd list

Write-Host ''
Write-Host "=== bind $busid ==="
# A device that was bound before still needs a fresh bind after it is unplugged
# and replugged, because the binding is keyed to the port and the instance.
& $usbipd bind --busid $busid --force

Write-Host ''
Write-Host "=== attach $busid ==="
# usbipd-win 5.x takes "--wsl [DISTRIBUTION]" as one option and no longer
# accepts a separate --distribution flag; in fact 5.x no longer needs the
# distribution named at all, since an attached device is visible to every WSL 2
# distribution. The distribution is still passed to keep this script working
# with the 3.x/4.x releases, which did require it.
& $usbipd attach --wsl $distro --busid $busid

Write-Host ''
Write-Host '=== state ==='
& $usbipd list

Write-Host ''
Write-Host 'Done. Inside WSL, check with:'
Write-Host '  ls -l /dev/hidraw* /dev/ttyUSB*'
Write-Host ''
Write-Host 'The hidraw node is created root:root mode 0600, so a normal user needs'
Write-Host 'a udev rule (or a chmod) before the device can be opened:'
Write-Host '  sudo bash /mnt/c/cmdisp/newisp-cross/scripts/wsl-udev.sh'
Write-Host ''
Write-Host 'To give the device back to Windows:'
Write-Host "  $usbipd detach --busid $busid"
