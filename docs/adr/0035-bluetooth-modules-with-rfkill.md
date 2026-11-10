# 0035. Bluetooth modules are rebuilt with rfkill and loaded in place of WSL's

Status: Accepted

## Context

A USB Bluetooth adapter can be attached to WSL with `usbipd`, and bluez
works with WSL's own `bluetooth.ko`. WSL's kernel is built without
`CONFIG_RFKILL`, so gsd-rfkill finds no Bluetooth and GNOME hides the
adapter. A custom kernel through `kernel=` in `.wslconfig` is not an option
(see [0022](0022-dxgdrm-keyed-on-the-kernel-release.md)).

## Decision

- `weaselway.bluetooth.enable` (off by default) builds `rfkill.ko`, the
  Bluetooth core, `rfcomm`, `bnep`, `hidp` and the USB drivers (`btusb`,
  `btintel`, `btbcm`, `btrtl`, `bcm203x`) for every kernel release dxgdrm
  supports, with `mkModules` from wsl-kernel, the flake dxgdrm takes its
  kernel trees from (dxgdrm 0022). The build fails if enabling `RFKILL`
  changes any other option, so the modules match the running kernel.
- `wsl-bluetooth-modules.service` loads them with `modprobe -d` before udev
  triggers and before bluetoothd, since the stock `bluetooth.ko` cannot be
  swapped out once loaded.
- The kernel loads firmware from the VM's root, which has no `/nix/store`.
  The unit turns on the sysfs fallback, and a udev rule answers from
  `hardware.firmware`, decompressing zstd or xz itself.
- Firmware: all of `rtl_bt/` from linux-firmware (about 1.5 MiB). Of the
  common cheap USB sticks (RTL8761BUV/CU, Barrot BR8554, Actions ATS2851,
  CSR8510 clones), all supported by WSL's `btusb`, only Realtek needs any.
  All of linux-firmware would be 1.9 GiB.
- The NixOS module lives in this flake, not in dxgdrm, which stays a DRM
  driver. It uses `dxgdrm.inputs.wsl-kernel`, so both are built for the same
  releases.

## Consequences

- An adapter attached before the unit ran gets the stock modules. The unit
  then fails, and only `wsl --shutdown` helps. Attach adapters after boot.
- Intel, Qualcomm and Broadcom BCM20702 adapters get no firmware, MediaTek
  no driver (`BT_HCIBTUSB_MTK` is off in WSL's config).
- A new kernel release gets Bluetooth once dxgdrm supports it, like dxgdrm
  itself.
