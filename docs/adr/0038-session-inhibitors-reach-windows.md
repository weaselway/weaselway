# 0038. The session's sleep and idle inhibitors reach Windows

Status: Accepted

## Context

Linux applications ask the desktop not to sleep or blank the screen through
inhibitors: a video player while it plays, a download, or the user's
"do not suspend" switch. Windows knows nothing of them. With
`/sdl-allow-screensaver` as the viewer's default, Windows turned the monitor
off during a video in the session and went to sleep during a long download.

The desktops keep these inhibitors in different places:

- gnome-session keeps them in `InhibitedActions` on `org.gnome.SessionManager`
  (bit 8 for idle, bit 4 for suspend). Only the suspend ones also reach
  logind, as a `block-weak` "sleep" inhibitor; idle ones, the kind video
  players take, stay inside GNOME.
- Plasma's PowerDevil keeps them in its PolicyAgent
  (`org.kde.Solid.PowerManagement.PolicyAgent`) and also adopts logind's
  `block` inhibitors.
- Other compositors leave them to logind (`systemd-inhibit`).

## Decision

- weaselwayd watches all three over D-Bus (`weaselwayd/inhibit.c`) and
  combines them into two flags. An idle inhibitor sets both "keep the system
  awake" and "keep the display on", because an idle desktop is what suspends
  after a while. A sleep inhibitor sets only the first. All three sources
  report changes with signals, so nothing polls.
- The NixOS module installs PowerDevil with Plasma. nixpkgs adds it only
  with `powerManagement.enable`, which NixOS-WSL turns off, and without it
  only KWin knows of the apps' inhibitors.
- The flags go to the viewer on the `wwdisplay` channel (0029) as a new PDU,
  `INHIBIT`, the first one that the server sends. weaselwayd sends it once the
  channel is open and on every change.
- The viewer holds one Windows power request (`PowerCreateRequest`) and sets
  `PowerRequestSystemRequired` and `PowerRequestDisplayRequired` from the
  flags. It clears both when the channel closes; Windows clears them when
  the viewer exits.
- While Windows is locked, the viewer drops the display request, so that
  locking turns the monitor off as before. A message-only window on a thread
  of its own gets the lock notifications (`WTSRegisterSessionNotification`).

## Consequences

- Windows stays awake, and the display stays on, for as long as the session
  asks and a viewer is connected. Without a viewer nothing reaches Windows.
- `powercfg /requests` lists the viewer with the reason "The Weaselway
  desktop asks to stay awake".
- The system request also holds while Windows is locked.
- Old viewers ignore the PDU, and old weaselwayd versions never send it.
- The request stops idle sleep only. Closing the lid or choosing Sleep still
  puts Windows to sleep.
