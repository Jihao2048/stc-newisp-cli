#!/usr/bin/env bash
# Report the state of the Linux-side build environment.
set -u

echo "=== toolchain ==="
printf 'g++       : %s\n' "$(g++ --version 2>/dev/null | head -1 || echo MISSING)"
printf 'cmake     : %s\n' "$(cmake --version 2>/dev/null | head -1 || echo MISSING)"
printf 'pkg-config: %s\n' "$(pkg-config --version 2>/dev/null || echo MISSING)"

echo
echo "=== hidapi ==="
if pkg-config --exists hidapi-hidraw 2>/dev/null; then
    echo "hidapi-hidraw: $(pkg-config --modversion hidapi-hidraw)"
elif pkg-config --exists hidapi 2>/dev/null; then
    echo "hidapi: $(pkg-config --modversion hidapi)"
else
    echo "hidapi: MISSING"
fi

echo
echo "=== permissions ==="
echo "user  : $(id -un)"
echo "groups: $(id -Gn)"
if id -Gn | grep -qw dialout; then
    echo "dialout: yes (serial ports will be accessible)"
else
    echo "dialout: NO -- serial access will need sudo or a usermod"
fi
if [ -d /etc/udev/rules.d ]; then
    echo "udev rules dir: present"
    ls /etc/udev/rules.d/ 2>/dev/null | head -20
else
    echo "udev rules dir: absent (container?)"
fi

echo
echo "=== current devices ==="
ls -l /dev/ttyUSB* /dev/ttyACM* /dev/hidraw* 2>/dev/null || echo "none present"
