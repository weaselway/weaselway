# 0018. The Windows viewer runs from inside the image

Status: Accepted

## Context

The viewer is a FreeRDP build for Windows with gfxredir, touchpad gestures and
the microphone. In the Ubuntu setup, `install.ps1` downloaded it to Windows,
which had to be kept in step with the session.

## Decision

- The image contains the viewer, `sdl-freerdp.exe` with `SDL3.dll` and
  `SDL3_ttf.dll`, from the `weaselway/freerdp` flake (`44d5ad0`). It is
  cross-compiled with mingw.
- `ww-start-viewer` runs it from the Nix store through `\\wsl.localhost` and
  WSL interop, and passes its own arguments on to it. `WEASELWAY_VIEWER`
  selects another build.
- `ww-install-viewer-link` puts a shortcut on the Windows desktop that starts
  the distro and the session if needed, then the viewer.
- The viewer disables SDL's handling of Alt+F4, so it goes to the session.

## Consequences

- Nothing is installed on Windows, and the viewer always matches weaselwayd.
  Uninstalling is `wsl --unregister`, plus deleting the desktop shortcut if
  one was made.
- Interop must work. systemd remounts `binfmt_misc` at boot and loses WSL's
  handler, so the module sets `wsl.interop.register` (see 0020).
- The viewer works only in the Windows session that started WSL (see 0004).
