# Vendored gfxredir server channel

Four files from FreeRDP **3.30.0** (`channels/gfxredir/`). They were copied
unchanged from the RDP backend mutter used to carry (`src/backends/rdp/gfxredir`
on the `*-wslg` branches of weaselway/mutter); the README there lists the
mechanical changes against FreeRDP's own.

They are here because distributions build FreeRDP with the channel
off, so `freerdp/server/gfxredir.h` is installed but `libfreerdp-server3`
exports none of its symbols. The channel only needs the public WTS
virtual-channel API, so compiling it into weaselwayd is enough.

When the system FreeRDP moves to a new major/minor, re-copy the files from the
matching tag instead of patching them here.
