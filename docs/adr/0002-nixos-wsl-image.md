# 0002. Distribute as a NixOS-WSL image

Status: Accepted

## Context

The Ubuntu setup (see 0001) was a set of scripts that built and installed
patched packages, units and configuration into an existing distro. It broke
when an archive update replaced a patched package that was not held
(`2944556`), when a script was run twice, or when a download changed. The patched Mesa, mutter, the kernel module and FreeRDP
had to be built at matching versions, and nothing recorded which versions
belonged together.

## Decision

- Weaselway is a NixOS-WSL image, built from this repository's flake
  (`62d84bc`). The Ubuntu setup was removed with the switch to the KMS
  session (`9293e03`).
- `nixosModules.weaselway` declares everything the scripts used to set up:
  Mesa, dxgdrm, weaselwayd, the audio configuration, the units and the
  scripts. `weaselway.enable` turns it on.
- Every component comes from a flake input, locked in `flake.lock`: nixpkgs,
  NixOS-WSL, the Mesa source, dxgdrm and FreeRDP.
- The image is imported with `wsl --install --from-file`. Nothing is
  installed on the Windows side (see 0018).

## Consequences

- One commit of this repository fixes the versions of all components. An
  update is `nix flake update` and `nixos-rebuild switch`, and a rollback is
  `nixos-rebuild switch --rollback`.
- Users get NixOS, whether they know it or not. Changing the system means
  editing `/etc/nixos/configuration.nix` (see 0016).
- Packages that differ from nixpkgs have to be built by someone. CI pushes
  them to a binary cache (see 0017).
- Other distributions are not supported. The module could be reused on a
  NixOS-WSL system of one's own.
