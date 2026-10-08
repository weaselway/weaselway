# 0020. Each WSL workaround lives in the module, next to the failure it fixes

Status: Accepted

## Context

WSL and NixOS-WSL change things a desktop session relies on. Each of the
following failures was observed on a WSL installation.

## Decision

The module works around them, and ARCHITECTURE.md records the failure next to
each setting:

- `wslg-session.service`, which WSL's user generator creates, is masked. It
  replaced `$XDG_RUNTIME_DIR/pulse/native` with a link to the system distro's
  PulseAudio, and the desktop showed none of the session's audio devices.
- `PULSE_SERVER`, which WSL sets in every process, is unset in login shells.
- `wsl.interop.register = true`, because systemd mounts its own
  `binfmt_misc` after WSL registered `WSLInterop`.
- `systemd-binfmt` gets `/etc/binfmt.d/nixos.conf` as its argument and no
  `ExecStop`. Without an argument it flushes all rules through a status file
  that WSL mounts read-only, fails, and makes `nixos-rebuild switch` exit 4.
- NetworkManager and wpa_supplicant are disabled. GNOME enables them,
  wpa_supplicant fails in WSL, and that fails every `nixos-rebuild switch`.
- udev is enabled, because NixOS-WSL disables it and dxgdrm's rule needs it.
- `prep-session.sh` takes `/tmp/.X11-unix` back from WSL's read-only bind
  mount, and the NixOS-WSL mount unit for `X0` is masked.

## Consequences

- Each item can be removed once WSL or NixOS-WSL fixes the cause. Nothing
  detects that. The checks in ARCHITECTURE.md show the state of two of them,
  `/proc/sys/fs/binfmt_misc/WSLInterop` and `/run/user/1000/pulse/native`.
- `PULSE_SERVER` stays set for commands started without a login shell, as in
  `wsl -d Weaselway -- <program>`.
- New WSL releases can add failures of the same kind.
