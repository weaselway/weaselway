# 0025. Other compositors start through a session script the user provides

Status: Accepted

## Context

Any compositor with a KMS backend can run on the virtual display (see 0005),
but each needs its own environment and command line. Building a session type
for each into `ww-start-session` would not scale.

## Decision

- `ww-start-session custom` runs `custom-weaselway-session` from `PATH` as the
  session's main process, in the same logind session on `seat0` as the
  built-in sessions (`20c3eb4`). The script sets what its compositor needs,
  including `XDG_CURRENT_DESKTOP`, and executes it.
- `weaselway.session = "custom"` makes it the default.
- `examples/custom-weaselway-session` starts sway through a `nix-shell`
  shebang, without a rebuild.
- The built-in sessions are `gnome` and `plasma`. `gnome-shell` and `kwin`
  start the bare compositor, for debugging.

## Consequences

- sway, Weston, Hyprland and others run without changes to weaselway.
- Compositors other than mutter and KWin have seen little testing.
- The clipboard needs `ext-data-control-v1` from the compositor (see 0009).
