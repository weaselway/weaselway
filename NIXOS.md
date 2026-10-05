# Weaselway reference

This document describes how weaselway is put together: the path a frame takes, where each component
comes from, what the NixOS module configures and why, and how to build and debug the image. For
installation and day-to-day use, see the [README](README.md).

## Architecture

```
compositor (mutter, KWin, sway, ...) on its KMS backend
   | gbm + EGL (mesa d3d12)            | libinput
   v                                   v
dxgdrm: card0 + renderD128        /dev/input/event*  (uinput)
   | commit: buffer, damage,           ^
   | cursor, mode                      |
   v                                   |
weaselwayd -- GL readback -> shared memory -- RDP on a vsock --> sdl-freerdp.exe
           \- audio (PipeWire)
```

The compositor runs on its regular KMS backend and has no knowledge of Windows. dxgdrm presents one
CRTC, a primary plane with `FB_DAMAGE_CLIPS`, a cursor plane with a hotspot, and one connector.

The scanout buffers are D3D12 textures. weaselwayd waits for a commit on the dxgdrm node, imports the
buffer into its own GL context (mesa d3d12), reads the damaged region back with `glReadPixels` into a
pixel buffer, and copies it into a gfxredir buffer in the shared memory that the Windows client maps.
There is no codec fallback. A client that does not support gfxredir is disconnected.

weaselwayd acknowledges a frame once the client has it, and dxgdrm withholds the compositor's
page-flip event until then. A slow client therefore slows the compositor down, and frames do not
queue up.

The client's keyboard, mouse and touchpad become uinput devices, which libinput handles like any
others. The size of the session follows the viewer's window: weaselwayd sets the mode on dxgdrm, and
the compositor sees a hotplug event.

The clipboard is the only feature that needs the compositor's cooperation. With mutter, weaselwayd
uses the D-Bus interface that gnome-remote-desktop uses (`org.gnome.Mutter.RemoteDesktop`). It
creates a session object with the clipboard enabled and never starts it, so gnome-shell shows no
screen-sharing indicator. Text, HTML and images are converted between Windows clipboard formats and
MIME types. Other compositors have no clipboard integration yet.

A KMS compositor needs a logind session on `seat0` that owns the devices. `ww-start-session` creates
one by running the compositor in a transient system unit with a PAM session, as a display manager
would.

## Components

Everything except the system distro VHD comes from the flake.

| Component | Source | Where it runs |
|---|---|---|
| mesa with the d3d12 changes | `weaselway/mesa`, branch `mesa-26.2.1-wsl` | user distro (`/run/opengl-driver`) |
| dxgdrm kernel module | `weaselway/dxgdrm` flake | user distro kernel, loaded from the store |
| weaselwayd | [weaselwayd/](weaselwayd) in this repo | user distro, as a unit of the user manager |
| mutter | nixpkgs, plus [one patch](nix/mutter-stage-relayout.patch) | user distro (gnome-shell links it) |
| KWin (optional) | nixpkgs, plus a [damage patch](nix/kwin-6.7-fb-damage-clips.patch) | user distro |
| Windows viewer `sdl-freerdp.exe` | `weaselway/freerdp` flake, `packages.sdl-freerdp` | Windows, started from the store through interop |
| system distro VHD | `weaselway/wslg` releases (`ww-install-system-image`) | WSL's system distro, `systemDistro=` in `.wslconfig` |

### The system distro

WSL creates the `wslg` shared-memory device only when GUI applications are enabled and a
`systemDistro` VHD is configured (`WslCoreVm.cpp`, `LXSS_ENABLE_GUI_APPS`). The device is virtiofs
with DAX, and nothing in the guest can create it. The frames are handed over through it. Without the
share, weaselwayd logs "the shared-memory share is not mounted" and exits.

The weaselway system image is minimal. Its WSLGd creates `/mnt/wslg/.X11-unix` and
`/mnt/wslg/runtime-dir`, which WSL's init in the user distro expects, bind mounts `versions.txt`, and
then idles. It publishes nothing to the user distro.

### Values the guest derives

| Value | Source |
|---|---|
| vsock port | fixed: `WEASELWAY_VSOCK_PORT=3389` in [environment.d/10-weaselway.conf](environment.d/10-weaselway.conf) |
| VM ID | `wslinfo --vm-id -n`. NixOS-WSL has no `/bin/wslinfo`, so the scripts call `exec -a wslinfo /init --vm-id -n` |
| shared-memory NT path for the client | `WSL\<VM ID, upper case, no braces>\wslg` |
| virtiofs tag | always `wslg`. `prep-session.sh` mounts it at `/mnt/wslg-shared-memory` |

### Compositor patches

A compositor needs no patch to run on the virtual display. The two that the image carries fix
problems that show up there but are unrelated to RDP:

- mutter: the overview keeps its old size when the stage is resized, which happens whenever the
  viewer's window is resized. The fix is the single commit on `main` of `weaselway/mutter`.
- KWin: it reported no damage to the kernel, so every frame was read back in full. The patch sets
  `FB_DAMAGE_CLIPS`. It is on `master` of `weaselway/kde-kwin`, with backports on `weaselway-6.6.6`
  and `weaselway-6.7.5`.

## Repositories

- **weaselway** (this repository): the flake, the NixOS module, the overlay, the image
  configuration, weaselwayd and the shell scripts.
- **mesa**: the image builds the branch `mesa-26.2.1-wsl` with nixpkgs' Mesa package definition.
  `main` carries the same commits on top of upstream main.
- **dxgdrm**: `main`. The module is built with the toolchain the WSL kernel itself is built with,
  the kernel.org crosstool gcc 13.2.0 with binutils 2.41. The kernel sets `CONFIG_MODVERSIONS` and
  rejects a module built with nixpkgs' gcc. The configure phase compares the regenerated `.config`
  with the captured one. References to the kernel tree are stripped from the `.ko`; they would add
  about 1.6 GiB to the image. `dxgdrm_drm.h` in that repository is the interface that weaselwayd and
  mesa use.
- **freerdp**: `main`. Its flake provides a Windows cross-compilation dev shell and
  `packages.sdl-freerdp`, which runs `build-freerdp.sh` with the SDL3, SDL3_ttf and OpenSSL archives
  fetched by Nix beforehand. Versions and hashes are read from the script.
- **mutter**: `main` is upstream main plus the relayout fix, kept in a form that can be submitted
  upstream. The image does not build the fork. It applies the same commit to nixpkgs' mutter as a
  patch. The `*-wslg` branches contain the retired RDP backend, from which weaselwayd's RDP and audio
  code was ported.
- **kde-kwin**: see [Compositor patches](#compositor-patches). The image applies the backport as a
  patch.
- **wslg**: the system image. Releases are named `v1.0.79-N`, and `ww-install-system-image.sh` pins
  one by its SHA-256.
- **NixOS-WSL**: upstream, used unmodified as a flake input.

## Flake layout

### Inputs

`nixpkgs` (nixos-26.05), `nixos-wsl`, `dxgdrm`, `freerdp`, and `mesa-src` (`flake = false`).

`mesa-src` and `freerdp` are fetched as `git+https://…?ref=…&shallow=1` and not as `github:` inputs.
GitHub tarballs of repositories with `eol=` rules in `.gitattributes` produce different NAR hashes
with different Nix versions, and CI then fails with "NAR hash mismatch". Both repositories have been
affected.

### Overlay

[nix/overlay.nix](nix/overlay.nix) defines these packages:

- `weaselway-mesa`: nixpkgs' mesa with `src` replaced and the driver list reduced to d3d12,
  llvmpipe, softpipe and zink, plus the `microsoft-experimental` and swrast Vulkan drivers. rusticl,
  teflon, intel-rt and the tools are disabled. It does not replace `pkgs.mesa`. The module passes it
  to `hardware.graphics.package`, so no other package is rebuilt.
- `mutter`: nixpkgs' mutter with the relayout patch. It replaces `pkgs.mutter`, so gnome-shell links
  it.
- `kdePackages.kwin`: nixpkgs' KWin with the damage patch for its release. It is only built when
  `weaselway.plasma.enable` is set.
- `weaselwayd`: built from [weaselwayd/](weaselwayd) with its Makefile, against nixpkgs' libglvnd
  and libgbm (which load the patched mesa at run time), `weaselway-freerdp`, and the uapi header from
  the dxgdrm input. The gfxredir server channel is compiled in from
  [weaselwayd/gfxredir](weaselwayd/gfxredir), because distributions build FreeRDP without it.
  weaselwayd is a GLib program with one main loop. It uses GIO for D-Bus and sockets and libpng for
  images on the clipboard. The build runs `make check`.
- `weaselway-freerdp`: nixpkgs' freerdp plus
  [nix/freerdp-dsp-ffmpeg-pcm-s16.patch](nix/freerdp-dsp-ffmpeg-pcm-s16.patch). Only weaselwayd
  links it.
- `weaselway-viewer`: `freerdp.packages.<build system>.sdl-freerdp`. The outputs are Windows
  binaries, so the build platform does not matter.
- `weaselway-scripts`: `ww-start-session`, `ww-start-viewer`, `ww-install-viewer-link` and
  `ww-install-system-image`, wrapped with `writeShellApplication`. The wrapper for `ww-start-viewer`
  sets `WEASELWAY_VIEWER` to the executable in the store.

### Modules and configurations

- [nix/module.nix](nix/module.nix) is `nixosModules.weaselway`. See
  [The NixOS module](#the-nixos-module).
- [nix/image/](nix/image) contains `configuration.nix` and `flake.nix`, which the image ships as its
  `/etc/nixos`. The image flake takes nixpkgs and NixOS-WSL from weaselway's lock and builds the same
  toplevel as `nixosConfigurations.wsl`. The hostname is `nixos`, so `nixos-rebuild` builds `#nixos`.
- [nix/image-lock.nix](nix/image-lock.nix) is `nixosModules.image`, imported by both
  configurations. On activation it writes `/etc/nixos/flake.lock` if none exists. The lock pins
  `github:weaselway/weaselway` to the commit being built (`self.rev`, `self.narHash`), with
  weaselway's own lock nested under it. The result is identical to what `nix flake lock` writes for
  that commit (verified against `5d8b960`).
  - The lock cannot be part of the tarball. `wsl.tarball.configPath` is read through
    `lib.cleanSource`, and a generated directory there is an import-from-derivation.
  - A build from a dirty tree has no `self.rev` and ships without a lock.
  - The `narHash` of a git checkout equals that of GitHub's tarball only because weaselway has no
    `.gitattributes`. Adding `eol=` or `export-ignore` rules would break this (see
    [Inputs](#inputs)).

### Outputs

- `packages.<system>`: `weaselway-mesa`, `mutter`, `weaselwayd`, `weaselway-scripts`,
  `weaselway-viewer` and `tarballBuilder` (the default).
- `devShells.<system>.default`: shellcheck and the build dependencies of weaselwayd.
- `checks.<system>.shellcheck`: runs shellcheck on the scripts with `--severity=error`.

## The NixOS module

Most settings in [nix/module.nix](nix/module.nix) work around a specific failure that was observed
on a WSL installation. This section records those failures.

### Graphics

- `wsl.useWindowsDriver` adds `libd3d12`, `libdxcore` and `libd3d12core` to
  `/run/opengl-driver/lib`.
- `LD_LIBRARY_PATH=/run/opengl-driver/lib` is set in `environment.sessionVariables` and in
  `/etc/environment.d/05-weaselway-nixos.conf`, which covers the user manager and therefore the
  session. mesa and the Windows drivers call `dlopen` with bare library names, and NixOS has no
  search path on which they would be found. Without the variable, d3d12 fails silently and rendering
  falls back to llvmpipe or kms_swrast. Stock nixpkgs mesa fails the same way. `ww-start-session`
  passes the variable on to the session's unit.
- KWin's setcap wrapper is disabled when Plasma is enabled. NixOS's wrappers drop
  `LD_LIBRARY_PATH`, and KWin then cannot create its gbm device.
- `wslDriverCompat` adds `libedit.so.2`, a link to nixpkgs' `libedit.so.0`, to
  `/run/opengl-driver/lib`. Intel's WSL driver ships a `libLLVM-9.so` that requires the Debian
  soname. Without it the Intel user-mode driver fails to load and d3d12 cannot create a device.
  Drivers from other vendors may need other libraries under their Debian names; check with `ldd` on
  `/usr/lib/wsl/drivers/*/*.so`.
- Verified on an Intel HD 630: `eglinfo` reports `D3D12 (Intel(R) HD Graphics 630)`.

### Kernel modules and device nodes

- `services.udev.enable` is set because NixOS-WSL disables udev, and dxgdrm's rule, which sets mode
  0666 on its nodes, needs it.
- The user is a member of `render` and `video` in case a node appears before udev applies the rule.
  The user is also in `input`: a udev rule assigns `/dev/uinput` to that group, so that weaselwayd
  can create its devices without root.
- `weaselway-prep.service` runs [libexec/prep-session.sh](libexec/prep-session.sh) once per boot,
  with `DXGDRM_KO` set to the store path of the module for `uname -r`. The script:
  - loads dxgdrm by path, because WSL's `/lib/modules` is an overlay that does not contain it;
  - loads `evdev` and `uinput` from WSL's own `/lib/modules`, where both are modules that nothing
    else loads;
  - takes `/tmp/.X11-unix` back from WSL's read-only bind mount, so that the compositor can create
    its X socket there;
  - mounts the `wslg` share. If the share is missing, the unit fails with a message that points to
    `systemDistro=`.
- The NixOS-WSL bind mount for X0 (`tmp-.X11\x2dunix-X0.mount`) is masked for the same reason.

### Session

- There is no display manager. [ww-start-session.sh](ww-start-session.sh) runs the session command
  (`gnome-session`, `startplasma-wayland` or `custom-weaselway-session`) in a transient system unit,
  `weaselway-session.service`, with `PAMName=login` and `XDG_SEAT=seat0` on `tty1`. This makes it a
  logind session on the seat, and logind gives the compositor the KMS node and the input devices.
  Creating the unit requires `sudo`.
- The unit starts with an empty environment. The script passes on `PATH` (without the Windows
  directories), `XDG_DATA_DIRS`, `LD_LIBRARY_PATH`, the d3d12 variables and the adapter.
- The script first removes `WAYLAND_DISPLAY` and `DISPLAY` from the user manager's environment. A
  compositor that finds them there, left over from a previous session, tries to run nested.
- `weaselwayd.service` is a unit of the user manager, wanted by `default.target`. It waits for a
  compositor and keeps running when the compositor exits, so it is not bound to the session.
- `/etc/environment.d/10-weaselway.conf` is the environment of the user manager, and therefore of
  weaselwayd and the desktop's services: `GALLIUM_DRIVER=d3d12`, `GSK_RENDERER=gl`,
  `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` and the vsock port. WebKit's sandbox does not bind
  `/dev/dxg`, so its web process cannot create an EGL display and crashes. The file explains this in
  detail.
- The module enables GNOME so that the image has a desktop to start. A system that only uses a
  custom session still gets it.
- `ww-start-viewer` runs `sdl-freerdp.exe` from the store through `\\wsl.localhost`. Windows loads
  `SDL3.dll` and `SDL3_ttf.dll` from the same directory.

### WSL integration

- `wslg-session.service` is masked (`systemd.user.units."wslg-session.service".enable = false`).
  WSL's user generator creates this unit. It links `$XDG_RUNTIME_DIR/pulse/native`, `wayland-0` and
  `wayland-0.lock` into `/mnt/wslg/runtime-dir`, for the PulseAudio and Weston of the stock system
  distro. It runs after `pipewire-pulse.socket` is listening and replaces the socket's directory
  entry. The socket keeps listening, but clients follow the link to a path that does not exist, and
  the desktop shows no audio devices.
- `PULSE_SERVER` is unset in `environment.extraInit`. WSL sets it in every shell it starts, pointing
  at the stock PulseAudio.
- `wsl.interop.register = true`. systemd mounts its own `binfmt_misc` at boot, after WSL has
  registered `WSLInterop`, and the handler is lost. Running any `.exe`, including the viewer, then
  fails with "Exec format error".
- NetworkManager and wpa_supplicant are disabled. GNOME enables them, wpa_supplicant fails to start
  in WSL, and that failure makes every `nixos-rebuild switch` fail. WSL manages the network itself.

### Audio

- PipeWire runs with its PulseAudio server and WirePlumber. PulseAudio itself is disabled.
- [pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf](pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf)
  is installed through `services.pipewire.configPackages` and ends up in
  `/etc/pipewire/pipewire.conf.d/`.
- It defines a permanent null sink, "Remote Desktop Audio", and two `protocol-simple` servers on
  127.0.0.1: port 4711 for playback (S16LE, stereo, 44.1 kHz) and port 4712 for the microphone
  (S16LE, mono, 44.1 kHz).
- weaselwayd connects to both while a viewer with audio is attached
  ([weaselwayd/audio.c](weaselwayd/audio.c)).

## Image configuration

[nix/image/configuration.nix](nix/image/configuration.nix) sets the following:

- The user is `nixos` with uid 1000. WSL only sets up `/run/user/1000`.
- `weaselway.enable` is set. `weaselway.adapter` and `weaselway.plasma.enable` are present but
  commented out.
- Orca and speech-dispatcher are excluded, because their voices take about 650 MB. A comment explains
  how to restore them.
- Flakes are enabled, channels are disabled, and git is installed because the flake has git inputs.
- sshd is disabled. Uncomment the line in `configuration.nix` to debug over SSH. The image contains
  no password, so run `passwd` first. With mirrored WSL networking the distro is reachable from the
  LAN.

## Building

```sh
sudo nix run .#tarballBuilder       # writes nixos.wsl
```

Images are also available from CI runs and releases, see [CI](#ci).

Several packages are not on cache.nixos.org and take time to build: the kernel tree for dxgdrm,
mesa, mutter, and gnome-shell, which is relinked against mutter. CI pushes them to
weaselway.cachix.org. A machine that uses it as a substituter (`extra-substituters`; the key is in
[nix/module.nix](nix/module.nix)) downloads them for any commit that CI has built. The patched
FreeRDP and the Windows viewer build quickly; the mingw gcc is cached for x86_64.

To build against unpushed commits of the other repositories, override the input, for example
`--override-input mesa-src 'git+file:../mesa?ref=mesa-26.2.1-wsl'`. The same works for `freerdp` and
`dxgdrm`.

### Building on aarch64

The image is x86_64. Do not build it under qemu-user emulation on an aarch64 machine. On Apple
silicon, with its 16K pages, the emulated build crashes while loading `libavcodec`, which breaks
mutter's g-ir-scanner, and gcc segfaults partway through mesa. Use an x86_64 machine as a remote
store instead:

```sh
nix build --eval-store auto --store ssh-ng://<user>@<builder> --no-link --print-out-paths \
  .#nixosConfigurations.wsl.config.system.build.toplevel .#tarballBuilder
# on the builder, as root:
cd /tmp && sudo /nix/store/<hash>-nixos-wsl-tarball-builder/bin/nixos-wsl-tarball-builder nixos.wsl
```

### Building weaselwayd alone

Run `make` in [weaselwayd/](weaselwayd). It needs `DXGDRM_INCLUDE` set to a dxgdrm checkout, and
FreeRDP 3, GLib, libpng, EGL, GLES, gbm and libdrm from pkg-config. `nix develop` provides all of
these.

- `make check` tests the format conversions of the clipboard.
- [tests/clipboard-rdp.sh](weaselwayd/tests/clipboard-rdp.sh) copies and pastes text, HTML and an
  image in both directions without Windows or a GPU. xfreerdp on Xvfb acts as the client, a headless
  mutter as the session, and `tests/rdp-harness` is weaselwayd's RDP server and clipboard without
  the screen.
- [tests/selection-mutter.sh](weaselwayd/tests/selection-mutter.sh) tests the mutter side alone.

The two shell tests are not part of the build. They need mutter, D-Bus and an X server.

## CI

[build-image.yml](.github/workflows/build-image.yml) runs on pushes to `main` and on pull requests.
[release-image.yml](.github/workflows/release-image.yml) runs on `v*` tags and attaches
`nixos-weaselway-<tag>.wsl` to the release. Both use the composite action
[.github/actions/build-image](.github/actions/build-image) on the `ubuntu-26.04` runner. actionlint
does not know that label yet, but it exists.

- The image is about 1.4 GiB. GitHub limits release assets to 2 GiB, so keep an eye on the closure:
  the kernel tree, the kernel toolchain or Orca's voices each add hundreds of MB if they find their
  way back in.
- Both workflows push every path they build to weaselway.cachix.org with `cachix/cachix-action`. The
  token is the `CACHIX_AUTH_TOKEN` secret, which is passed to the composite action as an input
  because composite actions cannot read secrets. A run with an empty cache took about 45 minutes.
  With a warm cache only the changed packages are compiled. Pull requests from forks receive no
  secrets and only read from the cache.
- The module configures the same cache as a substituter, so `nixos-rebuild` on a WSL machine
  downloads these paths too. An installation that predates this setting gets it with its first
  rebuild after updating. That rebuild still compiles everything unless it is run with
  `--option extra-substituters https://weaselway.cachix.org --option extra-trusted-public-keys <key>`.
- Cachix's free tier has limited storage and evicts the least recently used paths first. The kernel
  tree is the largest item.

## Debugging

### Deploying a build to a test machine

A build can be copied to a running installation without going through a full image.

- Only `root` is a trusted Nix user in the image, so `nix copy` as `nixos` is rejected. Copy to
  `ssh-ng://<host>?remote-program=sudo%20nix-daemon` instead.
- Activate it with `sudo nix-env -p /nix/var/nix/profiles/system --set <toplevel>` followed by
  `sudo <toplevel>/bin/switch-to-configuration switch`.
- Only switch to a toplevel that was built from the same configuration as the machine's
  `/etc/nixos`. The image configuration does not enable sshd. A machine that enabled it in its own
  `/etc/nixos` loses it, and with it your access, when it is switched to a build of the repository's
  configuration. Compare the two first.

### Checks

```sh
systemctl status weaselway-prep                 # dxgdrm loaded, share mounted?
ls -l /dev/dri; mountpoint /mnt/wslg-shared-memory
cat /mnt/wslg/versions.txt                      # which system distro is really running
GALLIUM_DRIVER=d3d12 EGL_LOG_LEVEL=debug nix shell nixpkgs#mesa-demos -c eglinfo -B -p surfaceless
LD_DEBUG=libs …                                 # which WSL/driver .so fails to load
strace -f -e trace=openat …                     # which vendor UMD / dependency is missing
cat /proc/sys/fs/binfmt_misc/WSLInterop         # interop handler present?
ls -la /run/user/1000/pulse/                    # native must be a socket, not a symlink into /mnt/wslg
wpctl status; pw-top -b -n 3                    # sink present, graph running, ERR column
ls -l /dev/dri /dev/uinput                      # card0, renderD128; uinput group input
loginctl; loginctl seat-status seat0            # the session is on seat0 and owns the devices
journalctl -u weaselway-session -b              # the compositor
journalctl --user -u weaselwayd -b              # weaselwayd: …, rdp: …, audio: …, clipboard: …
```

### weaselwayd

`weaselwayd --verbose` logs one line per frame (the buffer, the share of the screen that changed,
and the duration of the readback) and every step of a copy and paste. `G_MESSAGES_DEBUG=clipboard`
(or `rdp`, `audio`, `weaselwayd`) enables the same output for one part.

To run weaselwayd by hand, stop the unit first with `systemctl --user stop weaselwayd`. `--tcp N`
listens on 127.0.0.1 instead of the vsock, for a client on the Linux side. `--no-clipboard` keeps the
two clipboards separate.

For clipboard problems, the log states whether mutter granted access ("sharing the session's
clipboard"), and `busctl --user tree org.gnome.Mutter.RemoteDesktop` shows the session object.
mutter closes the session object when the screen is locked. weaselwayd requests a new one every three
seconds until it succeeds.

`ww-start-viewer /sdl-show-stats:2` and `/sdl-show-damage` show the client's view of the same data.

### The session

`WEASELWAY_SESSION_ENV="NAME=value …" ww-start-session` adds variables to the session's unit, for
example `QT_LOGGING_RULES=kwin_*.debug=true`.

### The audio bridge

`protocol-simple` accepts additional clients, so the stream that weaselwayd receives can be recorded
without a viewer. Play a tone into the sink and capture port 4711:

```sh
nix shell nixpkgs#sox -c sox -n -r 48000 -c 2 /tmp/tone.wav synth 8 sine 440 vol 0.3
(sleep 1; pw-play /tmp/tone.wav) & nix shell nixpkgs#socat -c timeout 5 socat -u TCP:127.0.0.1:4711 OPEN:/tmp/cap.raw,creat,trunc
```

Analyse `/tmp/cap.raw` as S16LE stereo at 44.1 kHz. It should contain 5 s of a 440 Hz tone with a
peak of about 9800.

## Design notes

These decisions and fixes are easy to undo by accident.

- **No codec fallback.** The client either supports gfxredir or is disconnected, with the reason in
  weaselwayd's log. This is intentional.
- **The certificate is generated in memory.** `winpr-makecert` is not on `PATH` on NixOS, so
  weaselwayd creates a self-signed RSA-2048 certificate with libcrypto. The certificate cannot be
  omitted: FreeRDP's server refuses to initialise a peer without one, even with standard RDP
  security.
- **Access to the channel manager is serialised.** weaselwayd's playback thread and its main loop
  both drain FreeRDP's virtual channel manager, and a lock keeps them apart.
- **Alt+F4 goes to the session.** On Windows, SDL turns Alt+F4 into a close request for the viewer's
  window unless it is told not to. The viewer disables this.
- **16-bit PCM is signed.** nixpkgs builds FreeRDP with FFmpeg. FreeRDP's FFmpeg DSP backend mapped
  16-bit PCM to `AV_CODEC_ID_PCM_U16LE`, which is unsigned. rdpsnd passes every packet through
  `freerdp_dsp_encode()`, even when source and target format are identical, so every sample was
  shifted by 32768 and speech was unintelligible. The microphone (audin, in the decode direction)
  was affected in the same way. The fix is the patch in `weaselway-freerdp`, which weaselwayd links,
  and a commit on `main` of `weaselway/freerdp`. A small C program against `libfreerdp3` reproduced
  the shift and confirmed byte-identical output with the fix.

## Known issues

- Two features of the RDP backend that mutter used to carry have not been ported: the client's
  scale factor, and the error frame (a client is disconnected instead, with the reason in the log).
  There is no touchscreen device, only the touchpad.
- The clipboard works with mutter only and does not carry files.
  - There is no compositor-neutral API. KWin and the wlroots compositors implement
    ext-data-control-v1, which would be a second implementation of
    [weaselwayd/selection.h](weaselwayd/selection.h). mutter does not implement it.
  - A client that connects with text or an image on its clipboard replaces the session's clipboard
    content.
  - Tested against mutter 50.4 with xfreerdp as the client
    ([tests/clipboard-rdp.sh](weaselwayd/tests/clipboard-rdp.sh)).
- Audio from browsers crackles (Chromium, Firefox and GNOME Web on YouTube); mpv is not affected.
  weaselwayd's log shows no dropped backlog and no stalls while it happens. `pw-top` showed errors on
  the playback stream, and the graph running at 48 kHz against the sink's 44.1 kHz. Raising
  `clock.min-quantum` to 1024 did not help.
- PipeWire leaves "Remote Desktop Microphone" nodes behind after a viewer reconnects, because
  protocol-simple keeps the node of the closed socket.
- The microphone opens, but samples from a real microphone have not been verified.
- Plasma locks the screen after a while, and there is no password to unlock it with unless one was
  set.
- KWin's patch on `master` has only been compiled as the 6.6 and 6.7 backports. Rotated outputs and
  direct scanout are untested with it.
- PipeWire runs without realtime scheduling. rtkit is enabled and running, but `data-loop.0` stays
  at `SCHED_OTHER`.
- dxgdrm's udev rule is not always applied in time. On one boot the render node had mode `0666`, as
  the rule intends. On another it was `root:render`. The group memberships cover both cases.
- Compositors other than mutter and KWin have seen little testing on the virtual display.
