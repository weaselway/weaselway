# 0004. Frames go through the wslg shared memory, with no codec fallback

Status: Accepted

## Context

The frames have to get from the distro to a window on Windows at 60 frames per
second and at sizes up to 4K. WSL creates a shared-memory device, virtiofs
with DAX, that the Windows side can map, but only when GUI applications are
enabled. Nothing in the guest can create it. RDP's gfxredir channel hands
frames over through such a share. The other RDP paths encode every frame.

mutter's RDP backend had a codec path as a fallback, and for a while the share
was optional (`e9ddfe2`).

## Decision

- weaselwayd copies each frame's damaged region into a gfxredir buffer in the
  share. A client that does not support gfxredir, or cannot map the share, is
  disconnected, with the reason in weaselwayd's log. This is intentional.
- weaselwayd keeps three buffers in the pool: one the client is reading, one a
  readback is landing in, and one free for the next frame. Each buffer tracks
  the region it is missing relative to the newest frame.
- `prep-session.sh` mounts the share at
  `/run/wsl/virtiofs-mounts/weaselway-wslg`, private, with
  `/mnt/wslg-shared-memory` a link to it (`7c4fc8d`). WSL unmounts other
  virtiofs mounts when the first `wsl.exe` of the other elevation starts, and
  that dropped the viewer.
- Without the share, `weaselway-prep` fails with a message that points to
  `guiApplications`, and weaselwayd exits.

## Consequences

- Only the patched FreeRDP viewer (see 0018) can connect. A stock RDP client
  cannot.
- `guiApplications=false` in `.wslconfig` makes Weaselway unusable.
- The share lives in the namespace of the Windows logon session that started
  the VM. A viewer in another session (SSH, Remote Desktop, a scheduled task)
  cannot open it. `ww-start-viewer` checks for it and says so instead of
  showing a white window.
- There is no encoding cost, and only damaged regions are copied.
