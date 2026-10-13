# 0007. Read back only the damage, asynchronously, and pace the compositor to it

Status: Accepted

## Context

The scanout buffers are D3D12 textures in GPU memory. Each frame has to be
read back into system memory before it can be copied into the shared memory.
A compositor renders as fast as its frame clock allows, and a client that
falls behind would otherwise collect a queue of frames.

## Decision

- weaselwayd imports the frame's shared handle into its own GL context (Mesa's
  d3d12 driver) and reads it back with `glReadPixels` into a pixel buffer
  object. That is the `CopyTextureRegion` and map a D3D12 presenter would do,
  without a second graphics API.
- Where the context has OpenGL ES 3.1, a compute shader samples the imported
  texture and stores the pixels into the same pixel buffer object instead
  (issue #3). d3d12's `CopyTextureRegion` from the texture into a linear
  buffer runs at about 1.6 to 1.8 GiB/s, while a shader store was measured
  at about twice the frame rate on an RTX 4070 SUPER. Without ES 3.1 (Intel
  HD 630 gets ES 3.0 from d3d12) weaselwayd stays on `glReadPixels`. The
  startup log and the statistics line name the path in use. On a GeForce
  940MX at 1280x768 a full-screen readback took 8.3 ms with the shader and
  11.3 ms with `glReadPixels`. At about 10% of the screen the shader was
  slower (2.7 against 2.0 ms), so it only pays off for large damage.
- Only the bounding box of the damage that dxgdrm reports is read back,
  widened to a multiple of 64 pixels (D3D12's 256-byte row pitch). One
  readback is outstanding at a time.
- The readback is collected when its fence signals. Mesa's d3d12 driver
  exports the fence as a descriptor (`EGL_ANDROID_native_fence_sync`, a
  sync_file through dxgdrm), which the main loop watches (`20f971d`). Without
  the extension, or with `WEASELWAY_NO_FENCE_FD=1`, a GL fence is polled every
  millisecond. A readback that takes longer than a second counts as lost.
- weaselwayd acknowledges a frame to dxgdrm (`DXGDRM_ACK_FRAME`) unless its
  damage cannot be read back yet, because the previous readback is
  outstanding or the client holds every buffer. dxgdrm withholds the
  compositor's page flip until then, for at most `flip_timeout_ms` (see
  dxgdrm 0013).
- A client that minimises its window (Suppress Output) counts as no client:
  nothing is read back, and frames are acknowledged at once (`e652e8a`). When
  it shows the window again it gets the whole screen.

## Consequences

- A slow client slows the compositor down. Frames do not queue up.
- Compositors have to report their damage. KWin did not, and is patched for it
  (see 0013).
- The EGL fence has to be the one that flushes the copy. d3d12 answers a flush
  with no work in it with a fence that stands for no work.
- `weaselwayd --verbose` logs the share of the screen read back per frame and
  how long it took.
