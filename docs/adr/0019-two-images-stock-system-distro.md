# 0019. Two images, GNOME and Plasma, on WSL's stock system distro

Status: Accepted

## Context

Once the transport was derived in the user distro (see 0003), the custom WSLg
system image was no longer needed: the shared memory exists whenever GUI
applications are enabled, and the stock system distro's Weston and PulseAudio
do not get in the way. Installing Plasma on the GNOME image means a long build
of the patched KWin.

GitHub limits a release asset to 2 GiB.

## Decision

- Two images are built and released, `nixos-weaselway-gnome-<tag>.wsl` and
  `nixos-weaselway-plasma-<tag>.wsl` (`569d373`). They differ only in
  `configuration.nix`. The Plasma one sets `weaselway.plasma.enable` and
  `weaselway.session = "plasma"`, and does not install GNOME.
- The custom system image and `ww-install-system-image` are gone.
- The images leave out what is large and rarely needed: Orca and
  speech-dispatcher (about 650 MB of voices, `f59a539`), and in the Plasma
  image Elisa,
  the X11 KWin, krdp, extra wallpapers, the PIM runtime and Discover
  (`224be3a`). `configuration.nix` says how to add them back.
- dxgdrm's package carries no reference to the kernel tree (see dxgdrm 0010).

## Consequences

- Each image is about 1.4 to 1.95 GB. The kernel tree, the kernel toolchain or
  the voices would each push an image over the limit if they found their way
  back into the closure.
- The module enables no desktop; each image's `configuration.nix` does
  (`edb74dd`). A system built from the module alone has to enable one, and
  `weaselway.session` defaults to `gnome`.
- WSL's system distro keeps running its own Weston and PulseAudio next to the
  session.
