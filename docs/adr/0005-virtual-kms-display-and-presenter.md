# 0005. The compositor runs unmodified on a virtual KMS display

Status: Accepted, supersedes 0001

## Context

The RDP backend in mutter (see 0001) limited Weaselway to GNOME on a forked
mutter, and the backend had to be rebased for every mutter release. Every
Wayland compositor has a KMS backend, and dxgdrm already provided a render
node for Mesa's d3d12 driver.

## Decision

- dxgdrm also provides a virtual display: one CRTC, a primary plane with
  `FB_DAMAGE_CLIPS`, a cursor plane with a hotspot, and one connector. The
  compositor drives it on its regular KMS backend, like a monitor (see dxgdrm
  0011).
- weaselwayd is the userspace half of that display. It waits for each commit
  on the dxgdrm node, reads the frame back, and serves it to the Windows
  client over RDP. The client's input comes back as uinput devices (see 0008).
- The RDP server, audio and clipboard code were ported from mutter's backend
  into weaselwayd.
- This started as a spike (`31b432c`, 3 October 2026) and became the default a
  day later (`9293e03`). The mutter fork's backend is no longer built.

## Consequences

- GNOME, Plasma and any other compositor with a KMS backend run without
  changes. The two compositor patches the image carries fix problems unrelated
  to RDP (see 0013).
- The clipboard is the only feature that needs the compositor's cooperation
  (see 0009).
- A KMS compositor needs a logind session on `seat0` that owns the devices
  (see 0011).
- weaselwayd is a long-running daemon that is not bound to the compositor. It
  keeps running when the compositor exits and serves the next one.
- Two features of the old backend were not ported: the client's scale factor
  and the error frame.
