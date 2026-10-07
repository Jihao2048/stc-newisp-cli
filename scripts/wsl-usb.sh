#!/usr/bin/env bash
# Show the USB devices that usbipd-win has forwarded into WSL.
set -u

echo "=== kernel modules ==="
lsmod 2>/dev/null | grep -E 'vhci|usbip' || echo "  (none listed)"

echo
echo "=== lsusb ==="
if command -v lsusb >/dev/null 2>&1; then
    lsusb
else
    echo "  lsusb not installed; reading sysfs instead"
    for d in /sys/bus/usb/devices/*/; do
        [ -f "$d/idVendor" ] || continue
        printf '  %s:%s  %s\n' \
            "$(cat "$d/idVendor")" "$(cat "$d/idProduct")" \
            "$(cat "$d/product" 2>/dev/null || echo '?')"
    done
fi

echo
echo "=== serial nodes ==="
ls -l /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo "  no ttyUSB / ttyACM nodes"

echo
echo "=== hidraw nodes ==="
ls -l /dev/hidraw* 2>/dev/null || echo "  no hidraw nodes"

echo
echo "=== the STC device, if present ==="
found=0
for d in /sys/bus/usb/devices/*/; do
    [ -f "$d/idVendor" ] || continue
    vid=$(cat "$d/idVendor")
    pid=$(cat "$d/idProduct")
    if [ "$vid" = "34bf" ]; then
        found=1
        echo "  $d -> $vid:$pid $(cat "$d/product" 2>/dev/null)"
        echo "    driver interfaces:"
        for i in "$d"*:*; do
            [ -d "$i" ] || continue
            printf '      %s' "$(basename "$i")"
            [ -f "$i/interface" ] && printf '  %s' "$(cat "$i/interface")"
            echo
        done
        echo "    tty nodes attached here:"
        find "$d" -name 'tty*' -maxdepth 4 2>/dev/null | sed 's/^/      /' || true
    fi
done
if [ "$found" = "0" ]; then
    echo "  no device with vendor 34bf is present"
fi

echo
echo "=== permissions of the current user ==="
echo "  user  : $(id -un)"
echo "  groups: $(id -Gn)"
