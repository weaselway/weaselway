# 0023. The user manager's environment selects d3d12 and runs WebKit unsandboxed

Status: Accepted

## Context

weaselwayd and the desktop's services run under the user manager, the session
in a system unit (see 0011), and all need the same rendering settings. WebKit's bubblewrap sandbox
binds `/dev/dri` but not `/dev/dxg`, which d3d12 needs. Its web process gets no
EGL display and aborts, so GNOME Web and other WebKit applications show
nothing (`14c33b3`).

## Decision

`/etc/environment.d/10-weaselway.conf` sets:

- `GALLIUM_DRIVER=d3d12`.
- `GSK_RENDERER=gl`. The file's comment: Vulkan is experimental with d3d12
  (dzn), and GL works.
- `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`, until WebKit binds `/dev/dxg`
  (fixed upstream in WebKit PR 75066).
- `WEASELWAY_VSOCK_PORT=3389` (see 0003).

`ww-start-session` passes `GALLIUM_DRIVER`, `GSK_RENDERER` and
`WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS` to the session's unit as well. The
two lists must agree.

## Consequences

- WebKit applications work, but their web content runs without a sandbox. A
  compromised page has the user's rights.
- The variable can be dropped once a WebKit with the fix is in nixpkgs.
