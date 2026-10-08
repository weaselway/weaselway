# 0001. mutter's RDP backend serves the desktop

Status: Superseded by 0005

## Context

WSLg shows Linux windows on Windows through a Weston in WSL's system distro
and an RDP connection to `msrdc.exe`. It has no way to show a whole desktop.
The goal was a full GNOME session in a window on Windows, rendered on the GPU
that WSL exposes.

## Decision

The first versions (28 August to 4 October 2026) used three forks:

- **mutter** with an RDP backend (`-Drdp=enabled`, the `*-wslg` branches).
  gnome-shell rendered into a virtual monitor (`--virtual-monitor`), and the
  backend
  served the frames over RDP, with gfxredir shared memory and a codec path as
  fallback.
- **wslg**: a custom WSLg system image (`install-system-image.sh`), whose
  WSLGd published the vsock port, the VM ID and the share's path in
  `/mnt/wslg/mutter-rdp.env`.
- **mesa** with the d3d12 changes, built as Ubuntu packages (`+weaselN`
  suffixes, a PPA, `apt-mark hold`), installed by `install.ps1` and the
  `install-*.sh` scripts, until the NixOS image (see 0002) built the same
  forks with Nix.

## Consequences

- Only GNOME could run, and only on a mutter built from the fork. Every
  mutter release needed a rebase of the backend.
- Users had to replace WSL's system distro with a custom image
  (`systemDistro=` in `.wslconfig`).
- The archive's packages replaced the patched ones unless they were held.
- The RDP, audio and clipboard code of the backend was later ported into
  weaselwayd (see 0005), and the `*-wslg` branches stay in the mutter fork for
  reference.
