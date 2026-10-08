# 0024. RDP on a Hyper-V socket, with a throwaway certificate and no authentication

Status: Accepted

## Context

The viewer and weaselwayd are on the same machine, the viewer on the Windows
host and weaselwayd in the WSL VM. FreeRDP's server refuses to initialise a
peer without a certificate, even with standard RDP security.
`winpr-makecert` is not on `PATH` on NixOS.

## Decision

- weaselwayd listens on the vsock port (see 0003), not on a network socket.
- It generates a self-signed RSA-2048 certificate in memory with libcrypto at
  startup, and never writes it. TLS is on, NLA is off.
- The viewer runs with `/cert:ignore`.
- `--tcp N` listens on 127.0.0.1 instead, unauthenticated, for a client on the
  Linux side when debugging.

Reason, from the comment in `weaselwayd/rdp.c`: the transport is a Hyper-V
socket inside the machine, so nothing verifies the certificate and the
connection needs no credentials.

## Consequences

- No password prompt and no certificate warning.
- Any process on the Windows host that can open a Hyper-V socket to the VM can
  connect, see the screen and send input. Nothing authenticates the client.
- With `--tcp`, any local process in the VM can do the same.
