# 0009. The clipboard has a backend for mutter and one for ext-data-control-v1

Status: Accepted

## Context

A client without a window cannot read or set the clipboard in Wayland. There
is no protocol for it that every compositor implements: KWin and the wlroots
compositors have `ext-data-control-v1`, mutter does not, and mutter's
remote-desktop D-Bus interface exists only in mutter.

## Decision

weaselwayd runs two backends side by side (`selection.c`), and passes each
request to the one that finds its compositor. What it offers goes to both.

- **mutter** (`selection-mutter.c`, `bcda15d`): the D-Bus interface that
  gnome-remote-desktop uses. weaselwayd creates a remote desktop session and
  enables its clipboard, and never starts the session. Starting it is what
  makes gnome-shell show a screen-sharing indicator. mutter closes the session
  when the screen locks, and is not there before the session starts, so the
  session is requested again every three seconds until there is one.
- **ext-data-control-v1** (`selection-wayland.c`, `5bc0331`): weaselwayd is a
  Wayland client. It watches `$XDG_RUNTIME_DIR` for `wayland-*` sockets and
  connects to the first compositor that offers the protocol, and again when
  the compositor restarts.

On the RDP side the cliprdr channel was ported from mutter's backend. Text,
HTML and images are converted between Windows clipboard formats and MIME types
(`formats.c`), and `make check` tests the conversions.

## Consequences

- The clipboard works with mutter, KWin and, untested, the wlroots
  compositors. A compositor with neither interface has no clipboard.
- Files are not carried.
- With ext-data-control-v1, weaselwayd takes a selection event to be its own
  while its source has not been cancelled. KWin cancels the old source first,
  but the protocol does not require it.
- What is read from the session's clipboard has a size limit (`db8bffa`).
- `--no-clipboard` keeps the two clipboards separate.
