# 0021. The image user is nixos with the password nixos, and sshd is off

Status: Accepted

## Context

WSL sets up `/run/user/1000` only, so the session's user must have uid 1000.
The image first had a user without a password. Plasma locks the screen after a
while idle, and an account without a password cannot unlock it (`dbfa629`).

## Decision

- The user is `nixos`, uid 1000, with the password `nixos`. `sudo` does not ask
  for it. The README tells users to change it with `passwd`.
- The user is in `render`, `video` and `input` (see 0008).
- sshd is disabled. `configuration.nix` has the line to enable it, for
  debugging.

## Consequences

- The lock screen works out of the box.
- Every installation starts with the same known password. With sshd enabled
  and mirrored WSL networking, the distro is reachable from the LAN, so the
  password must be changed or a key added first.
- Any process of the user can become root.
