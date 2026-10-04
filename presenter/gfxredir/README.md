# Vendored gfxredir server channel

The same four files mutter's RDP backend carries in
`src/backends/rdp/gfxredir`, copied from there unchanged. They come from FreeRDP
**3.30.0** (`channels/gfxredir/`), with the mechanical changes listed in that
directory's README.

They are here for the same reason: distributions build FreeRDP with the channel
off, so `freerdp/server/gfxredir.h` is installed but `libfreerdp-server3`
exports none of its symbols. The channel only needs the public WTS
virtual-channel API, so compiling it into the presenter is enough.

When the system FreeRDP moves to a new major/minor, re-copy the files from the
matching tag instead of patching them here.
