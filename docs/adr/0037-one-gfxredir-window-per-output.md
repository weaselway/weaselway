# 0037. One gfxredir window and pool per output

Status: Accepted

## Context

A viewer with several monitors, or several windows on one, needs the desktop
in more than one piece. RDP's usual answer is one large desktop that the
client cuts into monitors. With gfxredir that would be one buffer the size of
the bounding box of all monitors: capped in size, mostly empty when the
monitors differ in size or position, and recreated whenever any monitor
changes. dxgdrm now has up to four outputs with their own modes, rates and
vblanks (dxgdrm 0023), so each screen can stay separate.

gfxredir already has window ids, pools and buffers, as WSLg uses it for one
window per application.

## Decision

- Output `i` is monitor `i` of the client's layout, in the order of its
  monitor array (at connect) or the Display Control `MONITOR_LAYOUT` PDU
  (later), up to `DXGDRM_MAX_OUTPUTS`. weaselwayd connects, resizes and
  disconnects outputs with `DXGDRM_SET_MODE`, with each monitor's size,
  position (the suggested X and Y) and physical size.
- Each output has its own frame and readback state in weaselwayd and its own
  gfxredir pool, a shared-memory file with three buffers of that output's
  size. Ids: gfxredir window `i`, pool `i + 1`, buffer `(i << 8) | (slot + 1)`
  (`8d36c2c`).
- A resize recreates only the pool of the output whose size changed, with its
  next frame. The pools of the other outputs and their presents in flight
  survive the Deactivate-All that follows a desktop resize (`301014d`).
- The viewer keeps a table of pools and draws each window from the buffers
  presented for its window id, the window's index in the sorted monitor
  array. Acks, refresh rate (0029) and present timing (0031) are per window.
  `/sdl-windows:<n>` opens n windows side by side and sends the layout with n
  monitors right after the first activation; it does not use FreeRDP's
  `UseMultimon`, which brings fullscreen windows and display hotplug with it.
  Right Shift+Right Ctrl+N and +X add and remove a window at runtime, and the
  viewer sends the new layout; weaselwayd drops the pools of removed outputs.
- Input stays in RDP's one coordinate space. The viewer adds its window's
  monitor origin to each event. weaselwayd maps the point to the monitor it
  falls in, then onto the uinput absolute axis spanning the outputs where
  the compositor put them.
- wwdisplay's `REFRESH_RATE` and `PRESENT_TIMING` carry the monitor index in
  what was a reserved field. No compatibility with the old PDUs, ids or
  dxgdrm uapi: viewer, weaselwayd and dxgdrm are updated together.

## Consequences

- Monitors of different sizes and refresh rates each get a buffer of their
  own size and a vblank in their own phase. A window the client is slow to
  draw holds back only its own output (0007).
- The compositor maps the absolute pointer onto the bounding box of its
  logical layout, which differs from the viewer's as soon as an output is
  scaled, and with KWin and Hyprland, which arrange the outputs on their
  own. weaselwayd reads that layout with `xdg_output` (wl_output's name
  `Virtual-N` is output N-1) and maps the pointer onto it. Until the
  compositor has said where every output is, it uses the suggested
  positions and the mode sizes.
- One render thread presents all windows, so a window on a slow monitor can
  delay the others.
- There is one RDP pointer. Its image comes from whichever output has a
  cursor plane.
- `/multimon` takes the same per-window path but has not been tested.
