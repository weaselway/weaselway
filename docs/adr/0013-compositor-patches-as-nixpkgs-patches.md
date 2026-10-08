# 0013. Compositor patches only for problems of their own, applied to nixpkgs

Status: Accepted

## Context

A compositor needs no patch to run on the virtual display (see 0005). Two
problems showed up there anyway:

- mutter: the overview kept its old size when the stage was resized, which
  happens whenever the viewer's window is resized.
- KWin: it reported the whole buffer as damaged in every frame, so every frame
  was read back in full. Hovering over Plasma's panel read back 100% of the
  screen, and 2-8% with the fix.

## Decision

- Each fix is a commit in a fork (`weaselway/mutter` `main`,
  `weaselway/kde-kwin` `master`), kept in a form that can be submitted
  upstream.
- The image does not build the forks. The overlay applies the same commit as a
  patch to nixpkgs' package: `nix/mutter-stage-relayout.patch`, and for KWin a
  backport per release (`kwin-6.6-…`, `kwin-6.7-…`), picked by version.
- The patched mutter replaces `pkgs.mutter`, so gnome-shell links it. The
  patched KWin is only built when `weaselway.plasma.enable` is set.

## Consequences

- Everything else in GNOME and Plasma comes from nixpkgs unchanged.
- mutter, gnome-shell and KWin are not on cache.nixos.org and come from
  weaselway's cache (see 0017).
- A nixpkgs update that moves KWin to a new release needs a new backport.
- KWin's patch on `master` has only been compiled as the backports. Rotated
  outputs and direct scanout are untested with it.
