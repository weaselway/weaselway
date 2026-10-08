# 0012. The session keeps the distro running

Status: Accepted

## Context

WSL stops a distro 15 seconds after the last `wsl.exe` that asked for it
exits, whatever runs inside. Closing the terminal that started the session, or
the viewer, therefore ended the session.

## Decision

- `ww-start-session` starts a hidden `wsl.exe` that loops while
  `weaselway-session.service` is active, and returns once it is gone
  (`404d875`).
- It is started through PowerShell, so that it is not attached to the
  terminal's console, which would take it along when closed.

## Consequences

- The distro keeps running for as long as the session does, with no WSL
  window open. Once the session is stopped, WSL stops the distro after 15
  seconds.
- Closing the viewer does not end the session. `ww-start-viewer` reconnects.
- The behaviour depends on WSL's idle rule, which WSL could change.
