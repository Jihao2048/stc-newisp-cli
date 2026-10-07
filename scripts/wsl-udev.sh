#!/usr/bin/env bash
# Give the current user access to the STC HID device inside WSL.
#
# A hidraw node is created as root:root with mode 0600, so opening the device
# as a normal user fails with EACCES before any protocol work can happen. The
# portable fix is a udev rule; inside WSL systemd/udev is not always running, so
# this also chmods the node that is already present and reports which approach
# worked.
#
# Run with sudo.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "this script needs root: sudo $0" >&2
    exit 1
fi

RULE=/etc/udev/rules.d/60-newisp.rules

echo "=== installing udev rule ==="
cat > "$RULE" <<'EOF'
# STC USB devices: ISP interface (34BF:1001) and the USB bridge (34BF:FF09).
# Both are HID devices; the bridge is not a CDC/ACM port.
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="34bf", MODE="0666"

# In case a variant enumerates as a tty instead.
SUBSYSTEM=="tty", ATTRS{idVendor}=="34bf", MODE="0666"
EOF
echo "  wrote $RULE"

if command -v udevadm >/dev/null 2>&1; then
    echo
    echo "=== reloading udev ==="
    udevadm control --reload-rules 2>/dev/null && echo "  control --reload-rules ok" \
        || echo "  control --reload-rules failed (udev may not be running in WSL)"
    udevadm trigger 2>/dev/null && echo "  trigger ok" \
        || echo "  trigger failed"
fi

echo
echo "=== fixing the node that is already present ==="
# usbipd forwarding does not re-run udev for an already-attached device, so make
# the existing node usable now rather than asking for a re-plug.
for n in /dev/hidraw*; do
    [ -e "$n" ] || continue
    chmod 0666 "$n"
    echo "  chmod 0666 $n"
done

echo
echo "=== result ==="
ls -l /dev/hidraw* 2>/dev/null || echo "  no hidraw nodes present"
