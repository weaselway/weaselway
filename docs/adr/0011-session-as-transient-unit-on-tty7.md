# 0011. The session runs in a transient system unit on tty7, without a display manager

Status: Accepted

## Context

A KMS compositor needs a logind session on `seat0` that is active and owns the
DRM and input devices. A display manager would create one, but there is no
console to log in on, and the session should start from a WSL shell.

The VTs belong to the kernel, which all WSL distros share. Another distro with
systemd runs a getty on `tty1`, and that getty hangs the VT up when it starts,
which ends a session on it. The session first ran on `tty1`, and the README
asked for Weaselway to be the first distro started.

## Decision

- `ww-start-session` runs the session command (`gnome-session`,
  `startplasma-wayland` or `custom-weaselway-session`) in a transient system
  unit, `weaselway-session.service`, with `PAMName=login` and `XDG_SEAT=seat0`.
  Creating the unit needs `sudo`.
- The session is on `tty7` (`b26cafc`). The script switches to it with `chvt`
  first: a compositor whose session is not the active one on the seat does not
  render, and KWin stays at 1 FPS for a while after it becomes active.
- The unit starts with an empty environment. The script passes on `PATH`
  without the Windows directories, `XDG_DATA_DIRS`, `LD_LIBRARY_PATH`, the
  d3d12 variables and the adapter. `WEASELWAY_SESSION_ENV` adds more.
- The script removes `WAYLAND_DISPLAY` and `DISPLAY` from the user manager's
  environment first. A compositor that finds them there tries to run nested.

## Consequences

- The order in which distros are started does not matter (tested with Ubuntu
  started before and after the session).
- A second distro that also uses `tty7` would collide.
- `journalctl -u weaselway-session` has the compositor's output.
- The user needs `sudo`, which the image grants without a password (see 0021).
