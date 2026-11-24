# 0029. The viewer reports its monitor's refresh rate

Status: Accepted

## Context

dxgdrm's mode had a fixed 60 Hz clock, and compositors run their frame clock
at the mode's rate. On a faster Windows monitor the desktop was capped at
60 fps (issue #2). The rate belongs to the monitor the viewer's window is on,
which only the viewer knows, and RDP has no field for it: the Display Control
monitor layout carries size, position and scale only.

## Decision

- The viewer (weaselway's FreeRDP) has a dynamic channel of its own,
  `wwdisplay`, DVC name `Weaselway::Display`. weaselwayd opens it once
  drdynvc is up; the client sends the refresh rate in millihertz for each
  monitor of its layout, when the channel opens and whenever it changes
  (window moved to another monitor, display mode changed). The PDU carries
  the monitor's index in the layout, which is also the output's (0037). The
  format is in that repository's `include/freerdp/channels/wwdisplay.h`.
  The server sends one PDU of its own on the channel, `INHIBIT` (0038).
- The rate is the physical one, the highest the monitor runs at:
  `QueryDisplayConfig` for the window's monitor, the larger of the path's
  rate and the target mode's signal rate. With Windows' dynamic refresh rate
  the first is the low virtual rate and the second the physical one. With
  G-Sync or FreeSync the desktop mode is already at the top of the VRR range.
  Elsewhere SDL's rate for the display is used.
- weaselwayd passes it in that output's `DXGDRM_SET_MODE` (dxgdrm 0018). A
  change of the rate alone sets a new mode of the same size.
- weaselwayd validates the PDU (length, type, monitor index, 1 to 1000 Hz)
  and ignores anything else.

## Consequences

- The compositor's frame clock matches the monitor. Frames are still held to
  the client's pace by `DXGDRM_ACK_FRAME` (0007), so a rate the client cannot
  keep up with costs nothing.
- A viewer without the channel refuses it, and the mode stays at dxgdrm's
  `refresh` parameter (60 Hz by default).
- Each output runs at the rate of the monitor its window is on, so outputs
  on different monitors can run at different rates.
- The Linux side offers no VRR (`vrr_capable`); that would be a change to
  dxgdrm's virtual display.
