# 0033. GPU priority through dxgkrnl context priorities, set in Mesa

Status: Accepted

## Context

weaselwayd's readback and the compositor share the GPU with every other GL
and Vulkan application. Under load from another process, a readback waited
behind that process's work, and frames arrived late.

On WSL, D3D12 creates a kernel context in dxgkrnl (the WSL GPU kernel
driver) for each device and each command queue. Measured on an Intel HD 630
and an NVIDIA 940MX with the WSL libraries of 2023:

- The D3D12 queue priority `HIGH` has no effect.
  `D3D12_COMMAND_QUEUE_FLAG_ALLOW_DYNAMIC_PRIORITY` fails with
  `E_INVALIDARG`.
- `D3DKMTSetContextSchedulingPriority` accepts -7 to 7 without privileges.
  Raising our own context to 7 has no effect.
- Lowering the contexts of the other process to -3 or below brings the
  latency of small submissions under load close to idle: on Intel the
  median went from 3.84 ms to 1.73 ms at -3 and 1.58 ms at -7, on NVIDIA from 2.58 ms to 1.64 ms.
  -1 has no effect. Long single submissions are not preempted on Intel;
  on NVIDIA their p90 went from 28.7 ms to 12 ms.
- With the hook in Mesa, the result is the same whether the measuring
  process is at 0 or also at -3. What matters is that the loading process
  is below 0, not the difference between the two.

D3D12 offers no way to pass a kernel context priority. `libd3d12core.so`
creates the contexts of devices and queues through
`D3DKMTCreateContextVirtual` in `libdxcore.so`.

## Decision

- Our Mesa fork hooks context creation in `libd3d12core.so`
  (`src/microsoft/common/d3dkmt_priority.c`). Before the first device is
  created, it opens `libd3d12core.so` and replaces the GOT (global offset
  table, the table of addresses the library calls other libraries through)
  entries of `D3DKMTCreateContextVirtual` and `D3DKMTDestroyContext`. Every
  new context gets its priority right after it is created.
- Every context starts at -3 (medium). A context that asks for high
  priority gets 0, the Windows default. Low is -7. Realtime is not offered.
- The d3d12 gallium driver advertises low, medium and high through
  `EGL_IMG_context_priority`. All contexts of a screen share one D3D12
  queue, so the priority applies to the whole process: the highest
  priority any context asked for.
- Dozen (Mesa's Vulkan driver on D3D12) advertises `VK_KHR_global_priority`
  and `VK_EXT_global_priority` with low, medium and high, set per queue.
  Each D3D12 queue creates exactly one kernel context on the calling
  thread, so the priority is passed to the hook in a thread-local variable
  while the queue is created. Realtime fails with `VK_ERROR_NOT_PERMITTED`.
- weaselwayd requests high priority for its EGL context. mutter, KWin,
  wlroots and Hyprland already request it for their render contexts.

## Consequences

- Everything is in the drivers the image already uses. There is no
  `LD_PRELOAD` and no daemon, and the policy follows the EGL and Vulkan
  priority interfaces that compositors use.
- The hook depends on how `libd3d12core.so` is linked: lazy binding with a
  writable GOT, and the plain name by which `libd3d12.so` loads it. If a
  new WSL library breaks this, Mesa logs a warning and priorities stay at
  the default; rendering is not affected.
- An application that uses its own D3D12 or a stock Mesa is not lowered.
- The hook sees only contexts that `libd3d12core.so` creates. A GL process
  has two of them, the device's and the queue's. The vendor driver creates
  more contexts of its own through `libdxcore.so`; they stay at 0. The
  measurements above were taken with this limit.
- `MESA_D3DKMT_PRIORITY_DEBUG=1` with `MESA_LOG_LEVEL=info` logs the
  priority of each context.
