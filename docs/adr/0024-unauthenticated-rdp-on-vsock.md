# 0024. RDP on a Hyper-V socket, without encryption or authentication

Status: Accepted

## Context

The viewer and weaselwayd are on the same machine, the viewer on the Windows
host and weaselwayd in the WSL VM. The connection never leaves the machine,
and the viewer ignored the certificate, so the TLS layer that weaselwayd
used at first protected nothing.

FreeRDP's server refuses to initialise a peer without an RSA-2048
certificate, even with standard RDP security (the protocol's mode without
TLS). FreeRDP turns encryption off for standard RDP security only when the
peer is local. It counts a vsock peer as local only when it was built with
vsock support, and weaselwayd links against the FreeRDP of each
distribution.

## Decision

- weaselwayd listens on the vsock port (see 0003), not on a network socket.
- It offers standard RDP security only, with TLS and NLA (Network Level
  Authentication) off, and marks every client as local. FreeRDP then picks
  encryption level NONE.
- `weaselwayd/rdp.c` contains a fixed self-signed RSA-2048 certificate to
  satisfy FreeRDP. Its private key was thrown away. Without encryption,
  nothing uses the certificate.
- The viewer runs with `/sec:rdp`, so it does not try TLS first.
- `--tcp N` listens on 127.0.0.1 instead, unauthenticated, for a client on the
  Linux side when debugging.

## Consequences

- No password prompt, no certificate warning, and no TLS handshake or key
  generation at startup. weaselwayd no longer links libcrypto.
- Channel data may be written from two threads (see `flush_channels` in
  `rdp.c`). That is safe only because nothing is encrypted; legacy RDP
  encryption keeps RC4 state that two threads must not share.
- Any process on the Windows host that can open a Hyper-V socket to the VM can
  connect, see the screen and send input. Nothing authenticates the client.
  TLS did not change this, because the viewer did not check the certificate.
- With `--tcp`, any local process in the VM can do the same.
