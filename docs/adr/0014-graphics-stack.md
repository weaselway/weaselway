# 0014. The patched Mesa is the graphics driver package, found through LD_LIBRARY_PATH

Status: Accepted

## Context

Rendering needs the weaselway Mesa (its d3d12 driver knows the dxgdrm node)
and the Windows user-mode drivers that WSL mounts under `/usr/lib/wsl`.
Replacing `pkgs.mesa` would rebuild a large part of nixpkgs. Mesa and the
Windows drivers load libraries with `dlopen` by bare name, and NixOS has no
search path on which they would be found.

## Decision

- `weaselway-mesa` is nixpkgs' Mesa with `src` replaced and the driver list
  cut down to d3d12, llvmpipe, softpipe and zink, plus the
  `microsoft-experimental` and swrast Vulkan drivers. The module passes it to
  `hardware.graphics.package`. It does not replace `pkgs.mesa`.
- The source is a release branch of the Mesa fork (now `mesa-26.2.6-wsl`),
  which can be newer than nixpkgs' Mesa (26.1.8). Rusticl is off, because
  its crates are pinned to nixpkgs' release. Once nixpkgs' Mesa is 27 or
  newer, the fork's `main` is built instead, since nixpkgs' package fails on
  older majors.
- LLVM is linked statically. Intel's driver compiles shaders with its own
  `libLLVM-14`, and glibc binds GNU unique symbols process-wide by name, so
  with a shared `libLLVM` loaded first it used that library's tables and
  crashed (Firefox on startup, through `libgbm`).
- `wsl.useWindowsDriver` puts `libd3d12`, `libdxcore` and `libd3d12core` into
  `/run/opengl-driver/lib`.
- `LD_LIBRARY_PATH=/run/opengl-driver/lib` is set for shells and in
  `/etc/environment.d/05-weaselway-nixos.conf` for the user manager.
- KWin's setcap wrapper is disabled when Plasma is enabled, because NixOS's
  wrappers drop `LD_LIBRARY_PATH`.
- `libedit.so.2`, a link to nixpkgs' `libedit.so.0`, is added for Intel's
  driver, whose `libLLVM-9.so` needs the Debian soname. `libz.so.1` is added
  for the same driver, for processes that do not load zlib themselves, such
  as Vulkan clients on dzn.
- `weaselway.adapter` selects the GPU by a substring of its name. It sets
  `MESA_D3D12_DEFAULT_ADAPTER_NAME`, which d3d12 and dzn both read. Unset,
  both take the first integrated adapter, else the first.

## Consequences

- No other package is rebuilt for Mesa. Applications load it at run time from
  `/run/opengl-driver`.
- Without the variable, d3d12 fails silently and rendering falls back to
  llvmpipe. Anything that drops the environment (setuid wrappers, sandboxes)
  loses the GPU.
- Drivers from other vendors may need other Debian sonames. Only Intel's is
  covered.
- Vulkan clients on the compositor's adapter present through dma-bufs from
  dxgdrm 1.1 (Mesa wsl-adr 0013, dxgdrm 0021), so Mesa and dxgdrm have to be
  updated together. Clients on another adapter fall back to shm. A client
  whose `MESA_D3D12_DEFAULT_ADAPTER_NAME` differs from the compositor's takes
  that path too.
