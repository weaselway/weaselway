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
- `wsl.useWindowsDriver` puts `libd3d12`, `libdxcore` and `libd3d12core` into
  `/run/opengl-driver/lib`.
- `LD_LIBRARY_PATH=/run/opengl-driver/lib` is set for shells and in
  `/etc/environment.d/05-weaselway-nixos.conf` for the user manager.
- KWin's setcap wrapper is disabled when Plasma is enabled, because NixOS's
  wrappers drop `LD_LIBRARY_PATH`.
- `libedit.so.2`, a link to nixpkgs' `libedit.so.0`, is added for Intel's
  driver, whose `libLLVM-9.so` needs the Debian soname.
- `weaselway.adapter` selects the GPU by a substring of its name.

## Consequences

- No other package is rebuilt for Mesa. Applications load it at run time from
  `/run/opengl-driver`.
- Without the variable, d3d12 fails silently and rendering falls back to
  llvmpipe. Anything that drops the environment (setuid wrappers, sandboxes)
  loses the GPU.
- Drivers from other vendors may need other Debian sonames. Only Intel's is
  covered.
