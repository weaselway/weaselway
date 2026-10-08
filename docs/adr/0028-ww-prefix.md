# 0028. Commands carry a ww- prefix

Status: Accepted

## Context

The scripts were called `start-session`, `start-viewer` and
`install-viewer-link`, on a `PATH` shared with the desktop's own commands.

## Decision

The commands are `ww-start-session`, `ww-start-viewer` and
`ww-install-viewer-link` (`ac1c252`), so that they are recognisable as
weaselway's.

## Consequences

- A shortcut made by the old `install-viewer-link` calls the old names and has
  to be recreated.
- New commands get the same prefix.
