# 0008. The client's input becomes uinput devices

Status: Accepted

## Context

The compositor reads its input through libinput from `/dev/input`. mutter's
RDP backend injected input through mutter's own API, and had its own gesture
recognizer for the client's touchpad contacts. Neither exists for an
unmodified compositor.

## Decision

- weaselwayd creates a keyboard, a pointer and a multitouch touchpad on
  `/dev/uinput`, and writes the client's events to them. libinput in the
  compositor handles them like any other devices.
- A second pointer moves relatively and takes RDP relative mouse events.
  The absolute pointer cannot go past the edge of the screen, and the
  Windows cursor stops there too, so neither pushes against the barriers
  compositors put at the edge (GNOME's hot corner needs 100 px of push).
  While the cursor sits on such an edge of a viewer window, the viewer sends
  what the mouse moves past it, read from Raw Input, as relative motion.
- With `/mouse:relative` the viewer captures the mouse and sends all its
  motion as relative events. The relative pointer then moves the session's
  pointer, so weaselwayd sends the client the pointer's new position (RDP
  pointer position updates), and the viewer holds the visible Windows
  cursor there.
- The client's touchpad contacts (MS-RDPEI, three fingers and more) go to the
  touchpad as they are (`6e10ba2`). libinput recognises the gestures, as it
  would on the machine's own pad.
- With the viewer's `/sdl-touchpad-raw` the client sends every contact, from
  the first finger on, and libinput also moves the pointer, taps and scrolls
  with them. The viewer holds the Windows cursor still while fingers are
  down, and weaselwayd sends it the pointer's position from the cursor plane,
  as for `/mouse:relative`, until the client sends an absolute mouse
  position. A left click while
  fingers are down is the touchpad's own button, so libinput's clickpad
  handling applies (two fingers make a right click).
- The touchpad reports a surface of 150 x 100 mm, about a large laptop pad.
  libinput's gesture thresholds are in millimetres, and the real pad's size is
  not known.
- Fingers the client stops reporting are lifted after half a second, and when
  it disconnects.
- The lock keys are kept in step with the client's (`d29a91f`).
- A udev rule gives `/dev/uinput` to the group `input`, and the user is in it,
  so weaselwayd creates its devices without root.

## Consequences

- Input works the same in every compositor, including gestures.
- The session sees an ordinary keyboard. The layout is set in the desktop, not
  in the viewer.
- Touchscreens are not forwarded. There is no touchscreen device.
- With `/sdl-touchpad-raw` the pointer moves as on a Linux laptop, not as
  set up in Windows. Its speed depends on how far the real pad is from
  150 x 100 mm. While fingers are down a mouse cannot move the cursor.
- Any process of the user can create input devices, because of the `input`
  group.
