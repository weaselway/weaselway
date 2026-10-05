# weaselway on NixOS-WSL

This is the reference for weaselway. It records how things fit together, why they are the way they
are, what has been checked on a real machine, and how to debug. Installing and updating the image is
in the [README](README.md).

## How a frame gets to Windows

```
mutter / KWin (stock, native backend)
   | gbm + EGL (mesa d3d12)            | libinput
   v                                   v
dxgdrm: card0 + renderD128        /dev/input/event*  (uinput)
   | commit: buffer, damage,           ^
   | cursor, mode                      |
   v                                   |
weaselwayd -- GL readback -> shared memory -- RDP on a vsock --> sdl-freerdp.exe
           \- audio (PipeWire)
```

- The compositor runs on its normal KMS backend and knows nothing about Windows. dxgdrm gives it one
  CRTC, a primary plane with `FB_DAMAGE_CLIPS`, a cursor plane with a hotspot, and one connector.
- Its scanout buffers are D3D12 textures. weaselwayd waits for a commit on the dxgdrm node, imports
  the buffer into its own GL context (mesa d3d12), reads the damaged part back with `glReadPixels`
  into a pixel buffer, and copies it into a gfxredir buffer in the shared memory the Windows client
  maps. gfxredir is the point of the whole project, so there is no codec fallback: a client that
  cannot do it is disconnected.
- weaselwayd acknowledges a frame only once the client has it, and dxgdrm delays the compositor's
  page-flip event until then. A slow client slows the compositor down instead of frames piling up.
- The client's keyboard, mouse and touchpad become uinput devices that libinput picks up like any
  others. The session's size follows the viewer's window: weaselwayd sets the mode on dxgdrm, and the
  compositor sees a hotplug.
- The clipboard is the one thing weaselwayd asks the compositor for. With mutter it goes through the
  D-Bus interface gnome-remote-desktop uses (`org.gnome.Mutter.RemoteDesktop`): a session object
  whose clipboard is enabled and which is never started, so gnome-shell shows no "screen is being
  shared" indicator. Text, HTML and images cross, converted between Windows clipboard formats and
  mime types.
- The compositor needs a logind session on `seat0` that owns the devices. `start-session` creates
  one with a transient system unit that has a PAM session, which is what a display manager does.

## The pieces

On NixOS all of them come from the flake except the system distro VHD.

| Piece | Where it comes from | Where it runs |
|---|---|---|
| mesa with the d3d12 changes | `weaselway/mesa` branch `mesa-26.2.1-wsl` | user distro (`/run/opengl-driver`) |
| dxgdrm kernel module | `weaselway/dxgdrm` flake | user distro kernel (loaded from the store) |
| weaselwayd | [weaselwayd/](weaselwayd) in this repo | user distro, a unit of the user manager |
| mutter | nixpkgs, plus [one patch](nix/mutter-stage-relayout.patch) | user distro (gnome-shell links it) |
| KWin (optional) | nixpkgs, plus a [damage patch](nix/kwin-6.7-fb-damage-clips.patch) | user distro |
| Windows viewer `sdl-freerdp.exe` | `weaselway/freerdp` flake, `packages.sdl-freerdp` | Windows, started from the store via interop |
| system distro VHD | `weaselway/wslg` releases (`install-system-image`) | WSL's system distro, `.wslconfig` `systemDistro=` |

**Why the system distro is still needed.** WSL only creates the `wslg` shared-memory device when GUI
apps are on and a `systemDistro` VHD is configured (`WslCoreVm.cpp`, `LXSS_ENABLE_GUI_APPS`). That is
virtiofs with DAX. Nothing in the guest can create it, and it is where the frames are handed over.
Without the share, weaselwayd logs "the shared-memory share is not mounted" and exits.

The weaselway system image is minimal. Its WSLGd only creates `/mnt/wslg/.X11-unix` and
`/mnt/wslg/runtime-dir`, which WSL's user-distro init expects, bind mounts `versions.txt`, and idles.
It publishes nothing.

**What the guest works out itself.**

| Value | Source |
|---|---|
| vsock port | fixed, `WEASELWAY_VSOCK_PORT=3389` in [environment.d/10-weaselway.conf](environment.d/10-weaselway.conf) |
| VM ID | `wslinfo --vm-id -n`. NixOS-WSL has no `/bin/wslinfo`, so scripts call `exec -a wslinfo /init --vm-id -n` |
| shared-memory NT path for the client | `WSL\<VM ID, upper case, no braces>\wslg` |
| virtiofs tag | always `wslg`. `prep-session.sh` mounts it at `/mnt/wslg-shared-memory` |

**The patches the compositors carry** have nothing to do with RDP:

- mutter: the overview keeps its old size when the stage is resized, which the viewer's window does
  all the time. The fix is the one commit on `main` of `weaselway/mutter`.
- KWin: it sent no damage to the kernel, so every frame was read back whole. The patch makes it set
  `FB_DAMAGE_CLIPS`. It lives on `master` of `weaselway/kde-kwin`, with backports on
  `weaselway-6.6.6` and `weaselway-6.7.5`.

## Repos and branches

- **weaselway** (this repo): the flake, the NixOS module, the overlay, the image config, weaselwayd,
  and the shell scripts.
- **mesa**: the image builds `mesa-26.2.1-wsl`, which matches nixpkgs' mesa release. `main` carries
  the same commits on upstream main.
- **dxgdrm**: `main`. The module is built with the WSL kernel's own toolchain, kernel.org crosstool
  gcc 13.2.0 with binutils 2.41. The kernel has `CONFIG_MODVERSIONS`, so a module built with nixpkgs'
  gcc is rejected. The configure phase diffs the regenerated `.config` against the captured one. The
  `.ko` is stripped of references to the kernel tree, which would otherwise pull about 1.6 GiB into
  the image. `dxgdrm_drm.h` there is the interface weaselwayd and mesa use.
- **freerdp**: `main`. The flake has a Windows cross dev shell and `packages.sdl-freerdp`, which runs
  `build-freerdp.sh` with the SDL3, SDL3_ttf and OpenSSL archives pre-fetched by Nix (versions and
  hashes are read out of the script).
- **mutter**: `main` is upstream main plus the relayout fix, kept so it can be sent upstream. The
  image does not build the fork; it applies the same commit to nixpkgs' mutter as a patch. The
  `*-wslg` branches are the retired RDP backend, which weaselwayd's RDP and audio code was ported
  from.
- **kde-kwin**: see above. The image applies the backport as a patch.
- **wslg**: the system image. Releases are `v1.0.79-N` and `install-system-image.sh` pins one by
  SHA-256.
- **NixOS-WSL**: upstream, used unmodified as a flake input.

## Flake layout (this repo)

- `inputs`
  - `nixpkgs` (nixos-26.05), `nixos-wsl`, `dxgdrm`, `freerdp`
  - `mesa-src` (`flake = false`)
  - `mesa-src` and `freerdp` are fetched as `git+https://…?ref=…&shallow=1`, **not** `github:`. GitHub
    tarballs of repos with `eol=` rules in `.gitattributes` get different NAR hashes from different
    Nix versions, and CI then fails with "NAR hash mismatch". This has hit both mesa and freerdp.
- [nix/overlay.nix](nix/overlay.nix)
  - `weaselway-mesa`: nixpkgs mesa with `src` swapped and the driver list cut to
    d3d12/llvmpipe/softpipe/zink plus the `microsoft-experimental`/swrast Vulkan drivers. rusticl,
    teflon, intel-rt and the tools are off. It does **not** replace `pkgs.mesa`: it goes in through
    `hardware.graphics.package`, so nothing else rebuilds.
  - `mutter`: nixpkgs' with the relayout patch, replaced outright so gnome-shell links it.
  - `kdePackages.kwin`: nixpkgs' with the damage patch for its release. Only built when
    `weaselway.plasma.enable` is set.
  - `weaselwayd`: built from [weaselwayd/](weaselwayd) with its Makefile, against nixpkgs' libglvnd
    and libgbm (which load the patched mesa at run time), `weaselway-freerdp`, and the uapi header
    from the dxgdrm input. The gfxredir server channel is compiled in from
    [weaselwayd/gfxredir](weaselwayd/gfxredir), because distributions build FreeRDP without it.
    It is a GLib program: one main loop, GIO for D-Bus and sockets, libpng for images on the
    clipboard. The build runs `make check`.
  - `weaselway-freerdp`: nixpkgs freerdp plus
    [nix/freerdp-dsp-ffmpeg-pcm-s16.patch](nix/freerdp-dsp-ffmpeg-pcm-s16.patch). Only weaselwayd
    links it.
  - `weaselway-viewer`: `freerdp.packages.<build system>.sdl-freerdp`. These are Windows binaries, so
    the build machine doesn't matter.
  - `weaselway-scripts`: `start-session`, `start-viewer`, `install-viewer-link` and
    `install-system-image`, wrapped with `writeShellApplication`. The wrapper for `start-viewer` sets `WEASELWAY_VIEWER` to the exe in the
    store.
- [nix/module.nix](nix/module.nix): `nixosModules.weaselway`. See the next section.
- [nix/image/](nix/image): `configuration.nix` and `flake.nix`, which the image also ships as its
  `/etc/nixos`. The image flake takes nixpkgs and NixOS-WSL from weaselway's lock and builds the same
  toplevel as `nixosConfigurations.wsl`. The hostname is `nixos`, so `nixos-rebuild` builds `#nixos`.
- [nix/image-lock.nix](nix/image-lock.nix): `nixosModules.image`, imported by both of those. On
  activation it writes `/etc/nixos/flake.lock` if there is none, pinning `github:weaselway/weaselway` to
  the commit being built (`self.rev`, `self.narHash`) with weaselway's own lock nested under it. The
  result is identical to what `nix flake lock` writes for that commit (checked against `5d8b960`).
  - It can't go into the tarball: `wsl.tarball.configPath` goes through `lib.cleanSource`, and a
    generated directory there is an import-from-derivation.
  - A build from a dirty tree has no `self.rev` and ships without a lock, as before.
  - A git checkout's `narHash` equals GitHub's tarball's only because weaselway has no
    `.gitattributes`. Adding `eol=`/`export-ignore` rules would break this (see `inputs` above).
- `packages.<sys>`: `weaselway-mesa`, `mutter`, `weaselwayd`, `weaselway-scripts`,
  `weaselway-viewer`, `tarballBuilder` (`default`).
- `nix develop` gives shellcheck; `nix flake check` runs it over the scripts at `--severity=error`.

## What the module does, and why

Each line in [nix/module.nix](nix/module.nix) exists because something broke without it. Most were
found on a real WSL install.

- **Graphics**
  - `wsl.useWindowsDriver` puts `libd3d12`, `libdxcore` and `libd3d12core` into `/run/opengl-driver/lib`.
  - `LD_LIBRARY_PATH=/run/opengl-driver/lib` is set both in `environment.sessionVariables` and in
    `/etc/environment.d/05-weaselway-nixos.conf` (for the user manager, and so the session). mesa and
    the Windows drivers dlopen by bare name, and NixOS has no search path they would be found on.
    Without this, d3d12 fails silently and you get llvmpipe/kms_swrast. Stock nixpkgs mesa fails the
    same way. `start-session` hands the variable on to the session's unit.
  - KWin's setcap wrapper is disabled when Plasma is on: NixOS's wrappers drop `LD_LIBRARY_PATH`,
    and KWin then cannot create its gbm device.
  - `wslDriverCompat` puts `libedit.so.2` (a link to nixpkgs' `libedit.so.0`) into
    `/run/opengl-driver/lib`. Intel's WSL driver ships a `libLLVM-9.so` that needs the Debian soname.
    Without it the Intel UMD fails to load and d3d12 can't create a device. Other vendors' drivers may
    need other Debian-named libraries: check with `ldd` on `/usr/lib/wsl/drivers/*/*.so`.
  - Result, checked on an HD 630: `eglinfo` reports `D3D12 (Intel(R) HD Graphics 630)`.
- **Kernel modules and device nodes**
  - `services.udev.enable` is needed because NixOS-WSL turns udev off, and dxgdrm's rule, which sets
    mode 0666, needs udev.
  - The user is in `render` and `video` too, in case the node appears before udev applies the rule,
    and in `input`: a udev rule gives `/dev/uinput` to that group, so weaselwayd can create its
    devices as the user.
  - `weaselway-prep` runs [libexec/prep-session.sh](libexec/prep-session.sh) once per boot, with
    `DXGDRM_KO` pointing at the store path for `uname -r`.
    - It loads dxgdrm by path; WSL's `/lib/modules` is an overlay that doesn't have it.
    - It loads `evdev` and `uinput` from WSL's own `/lib/modules`. Both are modules in the WSL kernel
      and nothing else loads them.
    - It takes `/tmp/.X11-unix` back from WSL's read-only bind, so the compositor can create its X
      socket there.
    - It mounts the `wslg` share. The unit fails with a pointer to `systemDistro=` if the share is
      missing.
  - The NixOS-WSL X0 bind mount (`tmp-.X11\x2dunix-X0.mount`) is masked for the same reason.
- **Session**
  - GNOME without GDM. [start-session.sh](start-session.sh) runs `gnome-session` (or
    `startplasma-wayland`) in a transient system unit, `weaselway-session.service`, with
    `PAMName=login` and `XDG_SEAT=seat0` on `tty1`. That makes it a logind session on the seat, and
    logind hands the compositor the KMS node and the input devices. This is why it needs `sudo`.
  - The unit starts with a clean environment. The script passes on `PATH` (without the Windows
    directories), `XDG_DATA_DIRS`, `LD_LIBRARY_PATH`, the d3d12 variables and the adapter.
  - It clears `WAYLAND_DISPLAY` and `DISPLAY` from the user manager first. A compositor that finds
    them there, left over from the previous session, tries to run nested.
  - `weaselwayd.service` is a unit of the user manager, wanted by `default.target`. It waits for a
    compositor and outlives it, so it is not tied to the session.
  - `/etc/environment.d/10-weaselway.conf` is the user manager's environment, and so weaselwayd's
    and the desktop services': `GALLIUM_DRIVER=d3d12`, `GSK_RENDERER=gl`,
    `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` and the vsock port. WebKit's sandbox doesn't bind
    `/dev/dxg`, so its web process can't create an EGL display and crashes; the file has the details.
- **WSL quirks**
  - **`wslg-session.service` is masked** (`systemd.user.units."wslg-session.service".enable = false`).
    WSL's user generator creates it. It symlinks `$XDG_RUNTIME_DIR/pulse/native`, `wayland-0` and
    `wayland-0.lock` into `/mnt/wslg/runtime-dir`, for the stock system distro's PulseAudio and Weston.
    It runs *after* `pipewire-pulse.socket` is listening and replaces the socket's directory entry. The
    socket still listens, but clients follow the symlink to nothing, and GNOME shows no audio devices.
  - **`PULSE_SERVER` is unset** in `environment.extraInit`. WSL sets it for every shell it starts,
    pointing at the stock PulseAudio.
  - **`wsl.interop.register = true`.** systemd mounts its own `binfmt_misc` at boot, after WSL
    registered `WSLInterop`, and the handler is gone. Running any `.exe` then fails with "Exec format
    error". That includes `start-viewer`.
  - **NetworkManager and wpa_supplicant are off.** GNOME enables them, wpa_supplicant fails in WSL,
    and that fails every `nixos-rebuild switch`. WSL manages the network itself.
- **Audio**
  - PipeWire with pulse and wireplumber; PulseAudio is off.
  - [pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf](pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf)
    goes in through `services.pipewire.configPackages` (it lands in `/etc/pipewire/pipewire.conf.d/`).
  - It defines a permanent "Remote Desktop Audio" null sink, and two `protocol-simple` servers on
    127.0.0.1:4711 (playback, S16LE stereo 44.1 kHz) and :4712 (mic, S16LE mono 44.1 kHz).
  - weaselwayd connects to both while a viewer with audio is attached
    ([weaselwayd/audio.c](weaselwayd/audio.c)).

## Image config ([nix/image/configuration.nix](nix/image/configuration.nix))

- User `nixos` (uid 1000; WSL only wires `/run/user/1000`).
- `weaselway.enable`, with `weaselway.adapter` and `weaselway.plasma.enable` optional.
- orca and speech-dispatcher are excluded; their voices are about 650 MB. The comment says how to
  restore them.
- Flakes on, no channels, git installed (the flake has git inputs).
- **sshd is off.** The line is commented out in `configuration.nix`; uncomment it to debug over SSH. No
  password ships in the image, so run `passwd` first. It's reachable on the LAN when WSL networking is
  mirrored.

## Building

```sh
sudo nix run .#tarballBuilder       # writes nixos.wsl
```

Or take the image from a CI run or a release, see "CI" below.

The image is x86_64. On an aarch64 machine, don't build under qemu-user/binfmt: on Apple silicon
(16K pages) it crashes loading `libavcodec`, which breaks mutter's g-ir-scanner, and gcc segfaults
partway through mesa. Build on an x86_64 machine (here `oliver@192.168.86.60`) as a remote store:

```sh
nix build --eval-store auto --store ssh-ng://oliver@192.168.86.60 --no-link --print-out-paths \
  .#nixosConfigurations.wsl.config.system.build.toplevel .#tarballBuilder
# on the builder (needs root, which sudo gives only with a password -- the user runs this):
cd /tmp && sudo /nix/store/<hash>-nixos-wsl-tarball-builder/bin/nixos-wsl-tarball-builder nixos.wsl
```

- To test unpushed commits of the other repos, add
  `--override-input mesa-src 'git+file:../mesa?ref=mesa-26.2.1-wsl'` (similarly `freerdp`, `dxgdrm`).
- weaselwayd alone builds with `make` in [weaselwayd/](weaselwayd), given `DXGDRM_INCLUDE` (a dxgdrm
  checkout) and FreeRDP 3, GLib, libpng, EGL, GLES, gbm and libdrm from pkg-config. `nix develop`
  has all of that.
  - `make check` tests the clipboard's format conversions.
  - [tests/clipboard-rdp.sh](weaselwayd/tests/clipboard-rdp.sh) copies and pastes text, HTML and an
    image in both directions without Windows or a GPU: xfreerdp on an Xvfb plays the client, a
    headless mutter the session, and `tests/rdp-harness` is weaselwayd's RDP server and clipboard
    without the screen. [tests/selection-mutter.sh](weaselwayd/tests/selection-mutter.sh) tries
    the mutter side alone. Neither runs in the build; they need mutter, D-Bus and an X server.
- Not on cache.nixos.org, so they take time: the dxgdrm kernel tree, mesa, mutter, and gnome-shell,
  which relinks against mutter. CI pushes them to weaselway.cachix.org; a builder that has it as a
  substituter (`extra-substituters`, key in [nix/module.nix](nix/module.nix)) gets them from there for
  any commit CI has built. The patched FreeRDP builds quickly, and so does the Windows viewer (the
  mingw gcc is cached on x86_64).
- The remote login shell is fish, so wrap remote scripts in `bash -s`.

**CI.** [build-image.yml](.github/workflows/build-image.yml) runs on pushes and PRs.
[release-image.yml](.github/workflows/release-image.yml) runs on `v*` tags and attaches
`nixos-weaselway-<tag>.wsl` to the release. Both use [.github/actions/build-image](.github/actions/build-image)
on the `ubuntu-26.04` runner. actionlint doesn't know that label yet; it exists.
- The image is about 1.4 GiB, under GitHub's 2 GiB release-asset limit. Watch the closure: the kernel
  tree, kgcc or orca's voices sneaking back in each cost hundreds of MB.
- Both push every path they build to weaselway.cachix.org (`cachix/cachix-action`, token in the
  `CACHIX_AUTH_TOKEN` secret, passed into the composite action as an input because composite actions
  can't read secrets). A run from scratch took about 45 minutes; with the cache warm, only what changed
  is compiled. Pull requests from forks get no secrets and only read.
- The module adds the same cache as a substituter, so `nixos-rebuild` on a WSL machine downloads those
  paths too. Installs from before that change get it with their first rebuild after updating; that one
  rebuild still compiles unless it's run with
  `--option extra-substituters https://weaselway.cachix.org --option extra-trusted-public-keys <key>`.
- Cachix's free tier has limited storage and evicts the least recently used paths. The kernel tree is
  the biggest item.

## Installing and running

```powershell
wsl --install --from-file nixos.wsl --name Gnome
```

1. Inside the distro:
   1. `install-system-image`
   2. Add the printed `systemDistro=` line to `.wslconfig`.
2. `wsl --shutdown` (PowerShell).
3. Back inside the distro: `start-session`, then `start-viewer`.

It has to be the first distro started after `wsl --shutdown`, because only that one gets
`/run/user/1000`.

`start-viewer` runs the exe from the store over `\\wsl.localhost`, and Windows loads `SDL3.dll` and
`SDL3_ttf.dll` from next to it. `WEASELWAY_VIEWER=/mnt/c/…/sdl-freerdp.exe start-viewer` uses another
build.

Change the system with `sudo -e /etc/nixos/configuration.nix` followed by
`sudo nixos-rebuild switch`. `sudo nix flake update --flake /etc/nixos` moves it to the newest
weaselway. `/etc/nixos/flake.lock` pins the commit the image was built from (see
"Flake layout").

## Debugging

**Putting a build onto a test machine without a full image round trip.**
- Only `root` is a trusted Nix user there, so `nix copy` as `nixos` is rejected. Copy to
  `ssh-ng://<host>?remote-program=sudo%20nix-daemon` instead.
- Then run `sudo nix-env -p /nix/var/nix/profiles/system --set <toplevel>` and
  `sudo <toplevel>/bin/switch-to-configuration switch`.
- **Only switch to a toplevel built from the same config as the machine's `/etc/nixos`.** The image
  config does not enable sshd, so a machine that turned it on in its own `/etc/nixos` loses it, and
  with it the access, when switched to a build of the repo's config. Diff them first.

**Checks that have been useful:**

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

`weaselwayd --verbose` logs a line for every frame: which buffer, how much of the screen, and how
long the readback took, and every step of a copy and paste. `G_MESSAGES_DEBUG=clipboard` (or `rdp`,
`audio`, `weaselwayd`) does the same for one part. To run it by hand, stop the unit first
(`systemctl --user stop weaselwayd`). `--tcp N` listens on 127.0.0.1 instead of the vsock, for a
client on the Linux side. `--no-clipboard` keeps the two clipboards apart.

For the clipboard, the log says whether mutter gave weaselwayd one ("sharing the session's
clipboard"), and `busctl --user tree org.gnome.Mutter.RemoteDesktop` shows the session object.
mutter closes the session when the screen is locked; weaselwayd asks for a new one every three
seconds until it gets it.

`start-viewer /sdl-show-stats:2` and `/sdl-show-damage` show the same from the client's side.

`WEASELWAY_SESSION_ENV="NAME=value …" start-session` adds variables to the session's unit, for
example `QT_LOGGING_RULES=kwin_*.debug=true`.

**Testing the audio bridge without the viewer.** `protocol-simple` accepts extra clients, so you can
record what weaselwayd receives. Play a tone into the sink and capture port 4711:

```sh
nix shell nixpkgs#sox -c sox -n -r 48000 -c 2 /tmp/tone.wav synth 8 sine 440 vol 0.3
(sleep 1; pw-play /tmp/tone.wav) & nix shell nixpkgs#socat -c timeout 5 socat -u TCP:127.0.0.1:4711 OPEN:/tmp/cap.raw,creat,trunc
# analyse /tmp/cap.raw as S16LE stereo 44.1 kHz: expect 440 Hz, peak ~9800, 5 s of data
```

## Things that were wrong and are fixed (don't reintroduce)

- **Codec fallback.** There is none on purpose: gfxredir or nothing. A client that cannot do it is
  disconnected with the reason in weaselwayd's log.
- **`winpr-makecert`.** It isn't on PATH on NixOS. weaselwayd makes a self-signed RSA-2048
  certificate with libcrypto, in memory. The certificate can't be dropped: FreeRDP's server refuses
  to initialize a peer without one, even for standard RDP security. That was tried.
- **A second reader of the channel manager.** weaselwayd's playback thread and its main loop both
  drain FreeRDP's virtual channel manager; the drain is serialized with a lock.
- **Alt+F4 closed the viewer** instead of the window in the session. SDL turns it into a close
  request on Windows unless told not to; the viewer now tells it.
- **Distorted audio.**
  - nixpkgs builds FreeRDP with FFmpeg. Its FFmpeg DSP backend mapped 16-bit PCM to
    `AV_CODEC_ID_PCM_U16LE`, which is unsigned. rdpsnd passes every packet through
    `freerdp_dsp_encode()`, even PCM to identical PCM, so every sample came out shifted by 32768 and
    speech was unintelligible.
  - The microphone (audin, decode direction) was affected the same way.
  - A 20-line C harness against `libfreerdp3` showed the shift, and byte-identical output with the
    fix.
  - Fixed in `weaselway-freerdp` (overlay patch), which weaselwayd links, and on `weaselway/freerdp`
    main.

## Open

- **Not ported from the RDP backend mutter used to carry:** the client's scale factor, and the
  error frame (a client is disconnected instead, with the reason in the log). There is no
  touchscreen device either, only the touchpad.
- **The clipboard** works with mutter only, and carries no files.
  - There is no compositor-neutral API. KWin has ext-data-control-v1, which would be a second
    implementation of [weaselwayd/selection.h](weaselwayd/selection.h); mutter does not have it.
  - A client that connects with text or an image on its clipboard replaces what the session had.
  - Tested against mutter 50.4 with xfreerdp as the client
    ([tests/clipboard-rdp.sh](weaselwayd/tests/clipboard-rdp.sh)).
- **Audio from browsers crackles** (Chromium, Firefox, GNOME Web on YouTube); mpv is fine.
  weaselwayd's log shows no dropped backlog or stalls while it happens. `pw-top` showed errors on
  the playback stream, and the graph running at 48 kHz against the sink's 44.1 kHz. Raising
  `clock.min-quantum` to 1024 did not help.
- **PipeWire leaves "Remote Desktop Microphone" nodes behind** after a viewer reconnects:
  protocol-simple keeps the closed socket's node.
- **The microphone** opens, but samples from a real one have not been checked.
- **Plasma locks the screen** after a while, and there is no password to unlock it with unless one
  was set.
- **KWin's patch on `master`** has only been compiled as the 6.6 and 6.7 backports. Rotated outputs
  and direct scanout are untested with it.
- **PipeWire runs without realtime scheduling.** rtkit is enabled and running, but `data-loop.0` stays
  `SCHED_OTHER`.
- **dxgdrm rules:** on one boot the render node was `0666`, as the rule intends. On another the user
  saw `root:render`; the groups cover it either way.
