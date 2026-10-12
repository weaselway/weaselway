# 0030. aquamarine and Hyprland are patched to follow the window size

Status: Accepted

## Context

When the viewer's window is resized, weaselwayd sets a new mode on dxgdrm, and
dxgdrm sends a hotplug event with the connector still connected. KWin and
mutter read the new modes and switch to the preferred one. Hyprland kept the
size it started with, for two reasons:

- Its backend, aquamarine, reads a connector's modes only when the connector
  goes from disconnected to connected. On a hotplug for a connector that
  stays connected, it keeps the old modes.
- Hyprland tests a new mode with the last frame's buffer if the format
  matches, whatever its size. For a larger mode, that buffer does not cover
  the CRTC, and dxgdrm rejects the test (`drm_atomic_helper_check_plane_state`
  without positioning). Smaller modes worked.

Hyprland already re-applies its monitor rule when aquamarine emits an empty
state event for an output. With the default `preferred` rule, that picks the
preferred mode.

## Decision

- After a hotplug, aquamarine compares each connected output's modes with the
  connector's. If they differ, it replaces them and emits the empty state
  event. It reads the modes with `drmModeGetConnectorCurrent`, since the scan
  before has probed the connectors already.
- Hyprland keeps the present buffer for a test only if it also has the
  swapchain's size, which is the mode's. Otherwise it attaches one from the
  swapchain.
- Each fix is a commit on `main` of a fork (`weaselway/aquamarine`,
  `weaselway/Hyprland`), kept in a form that can be submitted upstream. The
  overlay applies backports to nixpkgs' packages
  (`nix/aquamarine-0.11-mode-changes.patch`,
  `nix/hyprland-0.55-modeset-buffer-size.patch`), as for KWin (see 0013).
- dxgdrm does not report the connector as disconnected and connected again
  instead. Every compositor would tear down and recreate its output on each
  resize. Nor does it accept a primary plane smaller than the CRTC: weaselwayd
  would have to handle frames smaller than the mode, for all compositors.

## Consequences

- Hyprland follows the viewer's window size, as GNOME and Plasma do.
- aquamarine and Hyprland are rebuilt. Neither is on cache.nixos.org or in
  weaselway's cache, so a system with Hyprland builds both.
- A nixpkgs update that moves aquamarine or Hyprland to a new release needs
  new backports.
