# 0013. Compositor patches only for problems of their own, applied to nixpkgs

Status: Accepted

## Context

A compositor needs no patch to run on the virtual display (see 0005). Some
problems showed up there anyway:

- mutter: the overview kept its old size when the stage was resized, which
  happens whenever the viewer's window is resized.
- mutter: a failed device open closed fd 0, which could be the KMS fd. GNOME
  hung when WSL's vgem came back through udev.
- mutter: with two or more monitors, every view but the last rendered with a
  viewport sticking out past its framebuffer. Intel Gen9's D3D12 driver
  squeezes stencil-clipped draws with such a viewport, which left trails
  while dragging windows.
- KWin: it reported the whole buffer as damaged in every frame, so every frame
  was read back in full. Hovering over Plasma's panel read back 100% of the
  screen, and 2-8% with the fix.

## Decision

- Each fix is a commit in a fork (`weaselway/mutter` `main`,
  `weaselway/kde-kwin` `master`), kept in a form that can be submitted
  upstream.
- The image does not build the forks. The overlay applies the same commit as a
  patch to nixpkgs' package: `nix/mutter-stage-relayout.patch`,
  `nix/mutter-kms-fd0.patch` and `nix/mutter-viewport-in-fb.patch`, and for
  KWin a backport per release (`kwin-6.6-…`, `kwin-6.7-…`, `kwin-6.8-…`),
  picked by version. aquamarine and Hyprland follow the same rule (0030).
- The patched mutter replaces `pkgs.mutter`, so gnome-shell links it. The
  patched KWin is only built when Plasma is enabled.

## Consequences

- Everything else in GNOME and Plasma comes from nixpkgs unchanged.
- mutter, gnome-shell and KWin are not on cache.nixos.org and come from
  weaselway's cache (see 0017).
- A nixpkgs update that moves KWin to a new release needs a new backport.
- The viewport patch only applies from mutter 50.4, which has the fractional
  viewport helper it builds on. `distro/install.py` leaves it out for 50.0 to
  50.3 (0036).
- KWin's patch on `master` has only been compiled as the backports. Rotated
  outputs and direct scanout are untested with it.
