# 0006. weaselwayd is written in C, on one GLib main loop

Status: Accepted

## Context

The presenter started as a C stub. It was ported to Rust (`3e78ee7`), so that
every descriptor, EGL image and buffer has an owner that releases it. The next
step was to run FreeRDP's server in it, with code ported from mutter's RDP
backend, which is C against FreeRDP 3.

## Decision

- weaselwayd is C. The Rust port was reverted the same day (`59fe03e`):
  sharing code with FreeRDP 3 is much easier from C. The port stays in history
  for a later rewrite.
- It is built with hardening flags, and has `asan` and `analyze` targets.
- It runs on one thread with a `GMainLoop` (`6e47b6c`). The DRM node is a unix
  fd source, the RDP server a `GSource` that watches FreeRDP's descriptors,
  the readback's fence a descriptor (see 0007), and the clipboard's D-Bus and
  Wayland connections are sources too.
- GIO provides D-Bus and sockets, `GOptionContext` the options, `GError` the
  startup errors, and `g_message()` the logging, with one log domain per part
  (`weaselwayd`, `rdp`, `input`, `audio`, `clipboard`).
- The audio streams have threads of their own, ended through a
  `GCancellable`. The playback thread drains FreeRDP's virtual channel
  manager as the main loop does, and a lock (`vcm_drain_lock`) serialises the
  two.

## Consequences

- Code from mutter's backend ports with few changes, and the comments there
  still explain the order of the FreeRDP calls.
- Memory safety rests on review, the sanitizer builds and the tests.
- `G_MESSAGES_DEBUG=<domain>` enables debug output for one part.
