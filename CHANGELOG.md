# Changelog

Notable changes per release. Pre-releases (`-rcN`) are folded into the
release they led to. Weekly nixpkgs updates are not listed.

## Unreleased

- Mesa: `eglExportDMABUFImageMESA` fails with `EGL_BAD_ALLOC` when d3d12
  cannot share the texture, instead of returning success without fds. GTK 4
  apps (ptyxis, for example) closed one of their own fds after that, which
  flooded the next terminal with garbage and dropped pasted text.

## v1.6.0 - 2026-11-09

- Mesa with dzn presenting through dma-bufs, and dxgdrm v1.3.0.
- Module: `libz.so.1` for Intel's WSL driver.
- ADRs for dma-buf presentation, preemption limits and CPU priority.

## v1.5.0 - 2026-11-05

- Mesa 26.2.6, linked statically against LLVM.
- Mesa: dxgkrnl context priorities, write-back upload heaps, exportable VA
  surfaces for Chromium, dzn write-back host-visible memory on UMA GPUs, and
  a fix to dzn adapter enumeration.
- FreeRDP 3.33.0 and viewer v1.2.0.
- weaselwayd: dxgdrm's vblank lines up with the viewer's monitor, a frame is
  acknowledged before its readback is issued, all threads run at nice -11
  and request high GPU priority. `egl-load` generates GPU load for testing.
- Full preemption is set when the session starts.
- Images are built and cached on nixos-unstable too.
- Ubuntu: dxgdrm v1.2.2 and viewer v1.2.0.

## v1.4.1 - 2026-10-30

- weaselwayd: FreeRDP ExtSecurity off, the viewer failed logon with FreeRDP
  3.32.
- Ubuntu 26.04 install script: no snapd, Chromium from the xtradeb PPA,
  Firefox from Mozilla's apt repository.

## v1.4.0 - 2026-10-14

- The refresh rate follows the monitor the viewer is on.
- Hyprland follows the window size and gets a hardware cursor.
- Readback through a compute shader where the GPU has ES 3.1.
- WSL kernel 6.18.54.1: dxgdrm v1.2.0, VA-API on dxgdrm, no card0.
- Scripts moved to `bin/`.

## v1.3.0 - 2026-10-08

- Plasma is enabled with its stock option, one desktop per system.
- The networking setup is left to the GNOME image.
- PipeWire applies the sink volume to the forwarded monitor.

## v1.2.0 - 2026-10-08

- A Plasma image next to the GNOME one. The custom system image is gone.
- The session runs on tty7 and keeps the distro running while it lasts. It
  no longer has to be the first distro.
- Clipboard sharing with KWin through `ext-data-control-v1`.
- The viewer returns once it is running (`DEBUG=1` keeps it in the
  foreground).
- The images set the password `nixos`, for the lock screen.
- Architecture decision records in `docs/adr`.

## v1.1.0 - 2026-10-06

- The KMS session is the default: weaselwayd presents the virtual KMS
  display to the viewer over RDP, so any compositor with a KMS backend works.
  The Ubuntu setup is dropped.
- weaselwayd: audio, clipboard sharing with Windows, damage-only frames,
  fence-paced readback, a "No compositor running" placeholder.
- Custom sessions for other compositors.
- Commands carry a `ww-` prefix.
- A desktop shortcut for the viewer.
- dxgdrm v1.1.0, support for the 6.18.40.1 WSL kernel.

## v1.0.0 - 2026-10-02

- First release: a NixOS-WSL image running GNOME on mutter's RDP backend,
  with the patched Mesa, mutter and FreeRDP fetched from the weaselway forks.
- Windows viewer shipped in the image, audio and microphone through
  PipeWire, touchpad gestures, clipboard.
- Binary cache on weaselway.cachix.org.
