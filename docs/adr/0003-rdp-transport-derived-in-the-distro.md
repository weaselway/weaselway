# 0003. The RDP transport is derived in the user distro

Status: Accepted

## Context

With the custom system image (see 0001), WSLGd published the vsock port, the
VM ID and the shared memory's path in `/mnt/wslg/mutter-rdp.env`, and the
session waited for that file. This tied weaselway to a modified system distro
and to the moment WSLGd wrote the file.

## Decision

Everything the transport needs is found from the user distro (`4dc05db`):

- The vsock port is fixed at 3389, as `WEASELWAY_VSOCK_PORT` in
  `environment.d/10-weaselway.conf`. weaselwayd listens on it and
  `ww-start-viewer` reads it back.
- The VM ID comes from `wslinfo --vm-id -n`. NixOS-WSL has no `/bin/wslinfo`,
  so `ww-start-viewer` falls back to `exec -a wslinfo /init --vm-id -n` when
  `wslinfo` is not on the `PATH`.
- The shared memory's NT path for the client is `WSL\<VM ID>\wslg`, and its
  virtiofs tag is always `wslg`.

## Consequences

- The session works with WSL's stock system distro, which made it possible to
  drop the custom system image (see 0019).
- The port is a constant. Two sessions in two distros of the same VM would
  both try to listen on it.
- The values depend on WSL internals (the `/init` multi-call binary, the
  share's tag and NT path) that WSL does not document and could change.
