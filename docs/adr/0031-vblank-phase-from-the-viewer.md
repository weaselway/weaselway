# 0031. The viewer's vblank sets the phase of dxgdrm's

Status: Accepted

## Context

dxgdrm's virtual vblank runs at the monitor's rate (0029, dxgdrm 0019), but
in a random phase to the monitor's vblank, and the two drift apart. On the
HD 630 at 1440p60 the phase moved by about 0.8 ms per second, through a whole
frame in 20 s, and jumped by a frame each time it wrapped. A frame that is
ready just after the monitor's vblank waits a whole refresh to be shown.

## Decision

- The viewer measures, for every frame it presents with vsync, how long
  before the monitor's next vblank the frame was ready: the time its upload
  finished (`QueryPerformanceCounter`) against the vblank of the monitor the
  window is on: a thread waits for it with `IDXGIOutput::WaitForVBlank` and
  follows the window to another monitor. DWM's timing
  (`DwmGetCompositionTimingInfo`) would be simpler, but it is the primary
  monitor's only. About twice a second the viewer sends the 10th percentile
  in the `PRESENT_TIMING` PDU on `wwdisplay`.
- weaselwayd moves dxgdrm's vblank (`DXGDRM_SHIFT_VBLANK`, dxgdrm 0020) by
  half the difference to a target of 3 ms, at most 2 ms per step, and not at
  all within 0.5 ms of it. A later vblank makes the frames later and the
  slack smaller.
- A feedback loop on what the viewer sees, not a mapping of the two clocks:
  it needs no estimate of how long a frame takes to get from mutter to the
  viewer, and it corrects the drift on its own.

## Consequences

- On the HD 630 with a light client the 10th percentile stays at 2-3.5 ms
  and the median at 3-4.5 ms, against 0-16 ms drifting without the loop. It
  finds the phase within a few seconds. The frame rate is unchanged (60 fps
  light, 43 fps GPU-bound with 16 taps).
- The loop only moves the phase, it does not correct the rate, so it keeps
  working against the drift. The drift means the rate dxgdrm runs at
  (60.048 Hz from `QueryDisplayConfig`) is not quite the monitor's; not
  investigated.
- When the compositor cannot keep up with the refresh rate, frames are not
  tied to the vblank, and the loop only moves the phase around. That costs
  nothing.
- The viewer has one more thread, which wakes on every vblank once frames
  are measured, and keeps running for the window. Its wake-up delay counts
  as slack.
- Without vsync (`/sdl-no-vsync`) the viewer sends nothing.
