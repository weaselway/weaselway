# Bluetooth modules for the WSL kernel, rebuilt with CONFIG_RFKILL=m. The
# stock kernel has no rfkill, so gsd-rfkill reports no Bluetooth and GNOME
# hides the adapter even though bluez works. Built with wsl-kernel's
# mkModules, in the prepared trees, with vmlinux untouched.
{ wsl-kernel }:

{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.weaselway.bluetooth;

  # Of the common USB sticks only Realtek (RTL8761BUV/CU) needs firmware;
  # Barrot, ATS2851 and CSR clones run without. All of rtl_bt is ~1.5 MiB,
  # linux-firmware as a whole 1.9 GiB.
  realtekFirmware = pkgs.runCommand "wsl-bluetooth-firmware" { } ''
    mkdir -p $out/lib/firmware
    cp -a ${pkgs.linux-firmware}/lib/firmware/rtl_bt $out/lib/firmware/
  '';

  # What gets shipped and loaded instead of the stock modules.
  shipped = [
    "net/rfkill/rfkill.ko"
    "net/bluetooth/bluetooth.ko"
    "net/bluetooth/rfcomm/rfcomm.ko"
    "net/bluetooth/bnep/bnep.ko"
    "net/bluetooth/hidp/hidp.ko"
    "drivers/bluetooth/btusb.ko"
    "drivers/bluetooth/btintel.ko"
    "drivers/bluetooth/btbcm.ko"
    "drivers/bluetooth/btrtl.ko"
    "drivers/bluetooth/bcm203x.ko"
  ];

  # Stock modules the above import from. Built only so modpost knows their
  # symbol CRCs; the running kernel loads its own copies.
  symversOnly = [
    "crypto/ecc.ko"
    "crypto/ecdh_generic.ko"
    "drivers/usb/common/usb-common.ko"
    "drivers/usb/core/usbcore.ko"
    "drivers/hid/hid.ko"
  ];

  # lib/modules/<release>/ for every release, modprobe -d picks `uname -r`.
  modules =
    wsl-kernel.legacyPackages.${pkgs.stdenv.hostPlatform.system}.mkModulesAll
      "wsl-bluetooth-modules-all"
      (release: {
        pname = "wsl-bluetooth-modules";
        inherit release;
        # DELL_RBTN defaults to m with RFKILL; it is its own module and not wanted.
        config = "-m RFKILL -d DELL_RBTN";
        allowed = "RFKILL(_[A-Z0-9_]+)?";
        targets = shipped ++ symversOnly;
        install = shipped;
      });

  # udev passes FIRMWARE (the file name) and DEVPATH (the sysfs request).
  loadFirmware = pkgs.writeShellScript "wsl-load-firmware" ''
    req=/sys$DEVPATH
    f=${config.hardware.firmware}/lib/firmware/$FIRMWARE
    echo 1 > $req/loading
    if [ -e "$f" ]; then
      cat "$f" > $req/data
    elif [ -e "$f.zst" ]; then
      ${pkgs.zstd}/bin/zstd -dc "$f.zst" > $req/data
    elif [ -e "$f.xz" ]; then
      ${pkgs.xz}/bin/xz -dc "$f.xz" > $req/data
    else
      echo -1 > $req/loading
      exit 0
    fi
    echo 0 > $req/loading
  '';
in
{
  options.weaselway.bluetooth.enable = lib.mkEnableOption ''
    Bluetooth with rfkill: loads Bluetooth modules rebuilt with CONFIG_RFKILL
    in place of the stock ones, so the desktop shows the adapter. Attach the
    adapter with usbipd after the distro started'';

  config = lib.mkIf cfg.enable {
    hardware.bluetooth.enable = true;

    # The loader below decompresses, the kernel never reads these files.
    hardware.firmware = [ realtekFirmware ];

    # Must run before anything loads the stock bluetooth.ko, which can't be
    # swapped out once in use: before udev sees a usbip-attached adapter and
    # before bluetoothd.
    systemd.services.wsl-bluetooth-modules = {
      description = "Load Bluetooth modules with rfkill support";
      wantedBy = [ "sysinit.target" ];
      before = [
        "sysinit.target"
        "systemd-udev-trigger.service"
        "bluetooth.service"
      ];
      unitConfig.DefaultDependencies = false;
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
      };
      path = [ pkgs.kmod ];
      script = ''
        if [ -e /sys/module/bluetooth ] && [ ! -e /sys/module/rfkill ]; then
          echo "stock bluetooth.ko is already loaded, run wsl --shutdown and attach the adapter after boot" >&2
          exit 1
        fi
        if [ ! -d ${modules}/lib/modules/$(uname -r) ]; then
          echo "no Bluetooth modules for kernel $(uname -r), supported: ${toString wsl-kernel.lib.releases}" >&2
          exit 1
        fi
        # The kernel loads firmware from the WSL VM's root, which has no
        # /nix/store; let it ask udev for it instead (rule below).
        echo 1 > /proc/sys/kernel/firmware_config/force_sysfs_fallback

        # Stock dependencies first, -d only sees the modules from this package.
        modprobe -a ecdh_generic usbcore hid
        modprobe -d ${modules} -a rfkill btusb bcm203x rfcomm bnep hidp
      '';
    };

    # Answers the kernel's sysfs firmware requests from hardware.firmware.
    # NixOS-WSL turns udev off.
    services.udev.enable = true;
    services.udev.extraRules = ''
      SUBSYSTEM=="firmware", ACTION=="add", RUN+="${loadFirmware}"
    '';
  };
}
