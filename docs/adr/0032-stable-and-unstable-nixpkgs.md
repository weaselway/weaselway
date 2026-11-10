# 0032. One lock with a stable and an unstable nixpkgs

Status: Accepted

## Context

The image ran on the stable NixOS release only. Users who want newer
desktops had no cached way to get them: everything weaselway patches (Mesa,
mutter, KWin and what links them), plus dxgdrm's kernel trees and the
viewer's mingw toolchain, is missing from cache.nixos.org.

## Decision

- weaselway's flake has a second input, `nixpkgs-unstable`
  (`nixos-unstable`), in the same lock. `nixosConfigurations.wsl-gnome-unstable`
  and `wsl-plasma-unstable` build the image configurations with it.
- freerdp (the viewer) and NixOS-WSL follow the stable `nixpkgs`, so the
  mingw toolchain is the same path on both and is built once. dxgdrm does
  not follow it: its kernel trees come from wsl-kernel, which pins its own
  nixpkgs (dxgdrm 0022), so they are shared too and nixpkgs updates do not
  rebuild them.
- The image's `flake.nix` (0016) selects the channel with
  `weaselway.inputs.nixpkgs.lib.nixosSystem` or
  `weaselway.inputs.nixpkgs-unstable.lib.nixosSystem`, a one-line change.
  A nixpkgs of the user's own is used the same way, without
  `weaselway.inputs.nixpkgs.follows`, which would rebuild the viewer.
- CI builds the unstable systems for the cache only. There are no unstable
  images or releases.
- `update-nixpkgs.yml` updates `nixpkgs` on Fridays and `nixpkgs-unstable`
  on Saturdays, and commits the lock only if the build passes.
- Patches that no longer apply on unstable get a copy for the new version,
  picked by the package's version in `nix/overlay.nix` (0013).
- Separate `-debug` outputs are not pushed to Cachix (0017), to leave room
  for the second set of desktop packages.

## Consequences

- The Cachix cache holds two sets of desktop packages, which brings it closer
  to the free tier's limit and to evicting paths installs need.
- A KWin or Hyprland release on unstable can break its patch, and with it the
  weekly update, until the patch is ported.
- The image configurations must evaluate on both channels, for example
  `kwin-x11`, which is gone from unstable, is only excluded where it exists.
- The lock on `main` moves without a person committing it. Each such commit
  was built by CI, but not tested on a WSL machine.
