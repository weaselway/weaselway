#!/usr/bin/env bash

# Loads the Bluetooth modules with rfkill from the weaselway release in place
# of WSL's own, once per boot, before udev sees a usbip-attached adapter. The
# distro counterpart of wsl-bluetooth-modules in nix/bluetooth.nix; installed
# by distro/install.py --bluetooth.

set -euo pipefail

ROOT="${WEASELWAY_BLUETOOTH_ROOT:-/opt/weaselway/bluetooth}"

if [ -e /sys/module/bluetooth ] && [ ! -e /sys/module/rfkill ]; then
    echo "stock bluetooth.ko is already loaded, run wsl --shutdown and attach the adapter after boot" >&2
    exit 1
fi
if [ ! -d "${ROOT}/lib/modules/$(uname -r)" ]; then
    echo "no Bluetooth modules for kernel $(uname -r) in ${ROOT}, run distro/install.py again" >&2
    exit 1
fi

# The kernel loads firmware from the WSL VM's root, not from the distro; let it
# ask udev for it instead (load-firmware.sh).
echo 1 > /proc/sys/kernel/firmware_config/force_sysfs_fallback

# Stock dependencies first, -d only sees the modules from the release.
modprobe -a ecdh_generic usbcore hid
modprobe -d "${ROOT}" -a rfkill btusb bcm203x rfcomm bnep hidp
