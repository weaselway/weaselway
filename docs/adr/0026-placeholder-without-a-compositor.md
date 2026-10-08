# 0026. A placeholder screen while no compositor owns the display

Status: Accepted

## Context

weaselwayd outlives the compositor (see 0005). With no compositor, the client
got no screen size and therefore no frames, or the last frame the previous
compositor left behind.

## Decision

- While no client holds DRM master on dxgdrm (`DXGDRM_FRAME_OWNED`), the client
  gets a dark screen with "No compositor running" centred, as large as its
  window (`932a955`). It is sent once per client, resize or reconnect.
- When a compositor takes over, its first frame goes out whole.
- The text is drawn from a small built-in bitmap font (`placeholder.c`) with
  only the letters of the message, rather than a font library for one line.

## Consequences

- A viewer started before the session shows why it is empty.
- Changing the message means adding glyphs for its letters.
