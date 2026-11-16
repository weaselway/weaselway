# Bluetooth modules for the WSL kernel, rebuilt with CONFIG_RFKILL=m, and the
# firmware USB sticks need. Used by the NixOS module (bluetooth.nix) and, as one
# tarball per kernel release, by distro/install.py.
{ wsl-kernel, pkgs }:

rec {
  # Of the common USB sticks only Realtek (RTL8761BUV/CU) needs firmware;
  # Barrot, ATS2851 and CSR clones run without. All of rtl_bt is ~1.5 MiB,
  # linux-firmware as a whole 1.9 GiB.
  firmware = pkgs.runCommand "wsl-bluetooth-firmware" { } ''
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

  # wsl-bluetooth-<release>.tar.gz with lib/modules/<release> and
  # lib/firmware, attached to every release for distro/install.py.
  tarballs = pkgs.runCommand "wsl-bluetooth-tarballs" { } ''
    mkdir -p $out
    for dir in ${modules}/lib/modules/*; do
      release=$(basename "$dir")
      rm -rf stage
      mkdir -p stage/lib/modules
      # Writable copies without links into the store, which a distro lacks.
      cp -rL --no-preserve=mode "$dir" stage/lib/modules/
      cp -rL --no-preserve=mode ${firmware}/lib/firmware stage/lib/
      tar -C stage --sort=name --owner=0 --group=0 --numeric-owner --mtime=@1 \
        -czf "$out/wsl-bluetooth-$release.tar.gz" lib
    done
  '';
}
