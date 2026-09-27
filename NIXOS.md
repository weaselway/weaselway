# weaselway on NixOS-WSL

This is the reference for the NixOS side of weaselway. It records how things fit together, why they are
the way they are, what has been checked on a real machine, and how to debug. [WEASELWAY.md](WEASELWAY.md)
has the short user-facing version ("NixOS-WSL image"). Installing and updating the image is in the
[README](README.md). The Ubuntu setup is in [README-ubuntu.md](README-ubuntu.md).

## The pieces

A working session needs four things. On NixOS all of them come from the flake except the system
distro VHD.

| Piece | Where it comes from | Where it runs |
|---|---|---|
| mesa with the d3d12 changes | `weaselway/mesa` branch `mesa-26.2.1-wsl` | user distro (`/run/opengl-driver`) |
| mutter with the RDP backend | `weaselway/mutter` branch `50.4-wslg` | user distro (gnome-shell links it) |
| dxgdrm kernel module | `weaselway/dxgdrm` flake | user distro kernel (loaded from the store) |
| Windows viewer `sdl-freerdp.exe` | `weaselway/freerdp` flake, `packages.sdl-freerdp` | Windows, started from the store via interop |
| system distro VHD | `weaselway/wslg` releases (`install-system-image`) | WSL's system distro, `.wslconfig` `systemDistro=` |

**Why the system distro is still needed.** WSL only creates the `wslg` shared-memory device when GUI
apps are on and a `systemDistro` VHD is configured (`WslCoreVm.cpp`, `LXSS_ENABLE_GUI_APPS`). That is
virtiofs with DAX. Nothing in the guest can create it. mutter hands its frames to the client over
gfxredir, which lives in that shared memory. gfxredir is the point of the whole project, so there is
deliberately no codec fallback. Without the share, mutter sends a magenta error frame that names the
reason (`meta_rdp_peer_fail()` in mutter).

The weaselway system image is minimal. Its WSLGd only creates `/mnt/wslg/.X11-unix` and
`/mnt/wslg/runtime-dir`, which WSL's user-distro init expects, bind mounts `versions.txt`, and idles.
It publishes nothing.

**What the guest works out itself.** WSLGd used to publish these in `mutter-rdp.env`; that file is
gone.

| Value | Source |
|---|---|
| vsock port | fixed, `MUTTER_RDP_VSOCK_PORT=3389` in [environment.d/10-weaselway.conf](environment.d/10-weaselway.conf) |
| VM ID | `wslinfo --vm-id -n`. NixOS-WSL has no `/bin/wslinfo`, so scripts call `exec -a wslinfo /init --vm-id -n` |
| shared-memory NT path for the client | `WSL\<VM ID, upper case, no braces>\wslg` |
| virtiofs tag | always `wslg`. `prep-session.sh` mounts it at `/mnt/wslg-shared-memory` |

## Repos and branches

- **weaselway** (this repo): the flake, the NixOS module, the overlay, the image config, and the shell
  scripts shared with Ubuntu.
- **mutter**: `main` and `50.4-wslg` are the same tree (50.4). `50.1-wslg` is the same RDP backend on
  50.1, which the Ubuntu packages build. All three carry the dev shell (`flake.nix`,
  `weaselway-build.sh`). Commit changes to `main` and cherry-pick them onto both release branches. The
  NixOS image builds `50.4-wslg`.
- **mesa**: the NixOS image builds `mesa-26.2.1-wsl`, which matches nixpkgs' mesa release. Ubuntu
  builds `mesa-26.0.8-wsl`.
- **freerdp**: `main`. The flake has a Windows cross dev shell and `packages.sdl-freerdp`, which runs
  `build-freerdp.sh` with the SDL3, SDL3_ttf and OpenSSL archives pre-fetched by Nix (versions and hashes
  are read out of the script). It also carries the signed-PCM fix for the FFmpeg DSP (see "Audio").
- **dxgdrm**: the module is built with the WSL kernel's own toolchain, kernel.org crosstool gcc 13.2.0
  with binutils 2.41. The kernel has `CONFIG_MODVERSIONS`, so a module built with nixpkgs' gcc is
  rejected. The configure phase diffs the regenerated `.config` against the captured one. The `.ko` is
  stripped of references to the kernel tree, which would otherwise pull about 1.6 GiB into the image.
- **wslg**: the system image. Releases are `v1.0.79-N` and weaselway's `install-system-image.sh` pins
  one by SHA-256.
- **NixOS-WSL**: upstream, used unmodified as a flake input. The checkout in the parent directory is
  only for reading.

## Flake layout (this repo)

- `inputs`
  - `nixpkgs` (nixos-26.05), `nixos-wsl`, `dxgdrm`, `freerdp`
  - `mesa-src`, `mutter-src` (`flake = false`)
  - `mesa-src`, `mutter-src` and `freerdp` are fetched as `git+https://…?ref=…&shallow=1`, **not**
    `github:`. GitHub tarballs of repos with `eol=` rules in `.gitattributes` get different NAR hashes
    from different Nix versions, and CI then fails with "NAR hash mismatch". This has hit both mesa and
    freerdp.
- [nix/overlay.nix](nix/overlay.nix)
  - `weaselway-mesa`: nixpkgs mesa with `src` swapped and the driver list cut to
    d3d12/llvmpipe/softpipe/zink plus the `microsoft-experimental`/swrast Vulkan drivers. rusticl,
    teflon, intel-rt and the tools are off. It does **not** replace `pkgs.mesa`: it goes in through
    `hardware.graphics.package`, so nothing else rebuilds.
  - `mutter`: replaced outright so gnome-shell links it.
    - Built with `-Drdp=enabled`, plus `weaselway-freerdp` and `openssl` (for the in-memory session
      certificate).
    - gvdb is vendored in `postPatch`, because a git checkout has only the wrap.
  - `weaselway-freerdp`: nixpkgs freerdp plus
    [nix/freerdp-dsp-ffmpeg-pcm-s16.patch](nix/freerdp-dsp-ffmpeg-pcm-s16.patch). Only mutter links it.
  - `weaselway-viewer`: `freerdp.packages.<build system>.sdl-freerdp`. These are Windows binaries, so
    the build machine doesn't matter.
  - `weaselway-scripts`: `start-gnome-shell`, `start-viewer` and `install-system-image`, wrapped with
    `writeShellApplication`. The wrapper for `start-viewer` sets `WEASELWAY_VIEWER` to the exe in the
    store. `install-freerdp` is Ubuntu only.
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
- `packages.<sys>`: `weaselway-mesa`, `mutter`, `weaselway-scripts`, `weaselway-viewer`,
  `tarballBuilder` (`default`).

## What the module does, and why

Each line in [nix/module.nix](nix/module.nix) exists because something broke without it. Most were
found on a real WSL install.

- **Graphics**
  - `wsl.useWindowsDriver` puts `libd3d12`, `libdxcore` and `libd3d12core` into `/run/opengl-driver/lib`.
  - `LD_LIBRARY_PATH=/run/opengl-driver/lib` is set both in `environment.sessionVariables` and in
    `/etc/environment.d/05-weaselway-nixos.conf` (for the user manager, and so the session). mesa and
    the Windows drivers dlopen by bare name. Ubuntu finds them through
    `/etc/ld.so.conf.d/ld.wsl.conf`; NixOS has no equivalent. Without this, d3d12 fails silently and you
    get llvmpipe/kms_swrast. Stock nixpkgs mesa fails the same way.
  - `wslDriverCompat` puts `libedit.so.2` (a link to nixpkgs' `libedit.so.0`) into
    `/run/opengl-driver/lib`. Intel's WSL driver ships a `libLLVM-9.so` that needs the Debian soname.
    Without it the Intel UMD fails to load and d3d12 can't create a device. Other vendors' drivers may
    need other Debian-named libraries: check with `ldd` on `/usr/lib/wsl/drivers/*/*.so`.
  - Result, checked on an HD 630: `eglinfo` reports `D3D12 (Intel(R) HD Graphics 630)`.
- **Kernel module and device node**
  - `services.udev.enable` is needed because NixOS-WSL turns udev off, and dxgdrm's rule, which sets
    mode 0666, needs udev.
  - The user is in `render` and `video` too, in case the node appears before udev applies the rule.
  - `weaselway-prep` is the NixOS version of [units/weaselway-prep.service](units/weaselway-prep.service).
    It runs [libexec/prep-session.sh](libexec/prep-session.sh) with `DXGDRM_KO` pointing at the store
    path for `uname -r`.
    - It loads dxgdrm by path; WSL's `/lib/modules` is an overlay that doesn't have it.
    - It takes `/tmp/.X11-unix` back from WSL's read-only bind.
    - It mounts the `wslg` share. The unit fails with a pointer to `systemDistro=` if the share is
      missing.
  - The NixOS-WSL X0 bind mount (`tmp-.X11\x2dunix-X0.mount`) is masked, because mutter creates its own
    X socket there.
- **Session**
  - GNOME without GDM. `start-gnome-shell` starts `gnome-session@gnome.target` under the user manager.
  - The shell drop-in [units/org.gnome.Shell@.service.d/weaselway.conf](units/org.gnome.Shell@.service.d/weaselway.conf)
    is shipped through `systemd.packages`, with `/usr/bin/gnome-shell` replaced by the store path. It
    sets `MUTTER_RDP=1`, the headless virtual-monitor `ExecStart`, and
    `WSL2_SHARED_MEMORY_MOUNT_POINT`, and it unsets `PULSE_SERVER`.
  - `/etc/environment.d/10-weaselway.conf` is the same file Ubuntu installs: `XDG_SESSION_TYPE`,
    `GALLIUM_DRIVER=d3d12`, `GSK_RENDERER=gl` and the vsock port. systemd's environment.d generator reads
    `/etc/environment.d`.
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
  - mutter connects to both while a viewer with audio is attached (`meta-rdp-audio.c`).

## Image config ([nix/image/configuration.nix](nix/image/configuration.nix))

- User `nixos` (uid 1000; WSL only wires `/run/user/1000`).
- `weaselway.enable`, with `weaselway.adapter` optional.
- orca and speech-dispatcher are excluded; their voices are about 650 MB. The comment says how to
  restore them.
- Flakes on, no channels, git installed (the flake has git inputs).
- **sshd is off.** The line is commented out in `configuration.nix`; uncomment it to debug over SSH. No
  password ships in the image, so run `passwd` first. It's reachable on the LAN when WSL networking is
  mirrored.

## Building

The image is x86_64. The dev machine used so far is an aarch64 Apple M1. Don't build under
qemu-user/binfmt: with 16K pages it crashes loading `libavcodec`, which breaks mutter's
g-ir-scanner, and gcc segfaults partway through mesa. Build on the x86_64 NixOS builder
(`oliver@192.168.86.60`) as a remote store:

```sh
nix build --eval-store auto --store ssh-ng://oliver@192.168.86.60 --no-link --print-out-paths \
  .#nixosConfigurations.wsl.config.system.build.toplevel .#tarballBuilder
# on the builder (needs root, which sudo gives only with a password -- the user runs this):
cd /tmp && sudo /nix/store/<hash>-nixos-wsl-tarball-builder/bin/nixos-wsl-tarball-builder nixos.wsl
```

- To test unpushed fork commits, add `--override-input mutter-src 'git+file:../mutter?ref=50.4-wslg'`
  (similarly `mesa-src`, `freerdp`, `dxgdrm`).
- Not in the binary cache, so they take time: the dxgdrm kernel tree, mesa, mutter, and gnome-shell,
  which relinks against mutter. The patched FreeRDP builds quickly, and so does the Windows viewer (the
  mingw gcc is cached on x86_64).
- The remote login shell is fish, so wrap remote scripts in `bash -s`.

**CI.** [build-image.yml](.github/workflows/build-image.yml) runs on pushes and PRs.
[release-image.yml](.github/workflows/release-image.yml) runs on `v*` tags and attaches
`nixos-weaselway-<tag>.wsl` to the release. Both use [.github/actions/build-image](.github/actions/build-image)
on the `ubuntu-26.04` runner. actionlint doesn't know that label yet; it exists.
- The image is about 1.4 GiB, under GitHub's 2 GiB release-asset limit. Watch the closure: the kernel
  tree, kgcc or orca's voices sneaking back in each cost hundreds of MB.
- Nothing is cached between runs; each run takes about 45 minutes.

## Installing and running

```powershell
wsl --install --from-file nixos.wsl --name Gnome
```

1. Inside the distro:
   1. `passwd`
   2. `install-system-image`
   3. Add the printed `systemDistro=` line to `.wslconfig`.
2. `wsl --shutdown` (PowerShell).
3. Back inside the distro: `start-gnome-shell`, then `start-viewer`.

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

A test install has been reachable as `nixos@192.168.86.144` over SSH. The password is whatever the
user set with `passwd`; ask them. `sshpass` is in nixpkgs. After a re-import the host key changes, so
run `ssh-keygen -R <ip>` first.

**Putting a build onto the test machine without a full image round trip.**
- Only `root` is a trusted Nix user there, so `nix copy` as `nixos` is rejected.
- What works:
  1. List the missing paths (`nix-store --check-validity --print-invalid` over the closure).
  2. Stream them with `nix-store --export` on the builder into `sudo nix-store --import` on the machine.
  3. Run `sudo nix-env -p /nix/var/nix/profiles/system --set <toplevel>` and
     `sudo <toplevel>/bin/switch-to-configuration switch`.
- **Only switch to a toplevel built from the same config as the machine's `/etc/nixos`.** Once, a build
  from the repo config was switched onto a machine whose `/etc/nixos` had sshd and the repo's didn't.
  sshd was removed and access was lost. The image config no longer enables sshd, so a machine that
  turned it on in its own `/etc/nixos` loses it the same way when switched to a repo build. Diff them
  first.

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
journalctl --user -u 'org.gnome.Shell@*' -b     # mutter: rdp: …, rdp: audio: …
```

**Testing the audio bridge without the viewer.** `protocol-simple` accepts extra clients, so you can
record what mutter receives. Play a tone into the sink and capture port 4711:

```sh
nix shell nixpkgs#sox -c sox -n -r 48000 -c 2 /tmp/tone.wav synth 8 sine 440 vol 0.3
(sleep 1; pw-play /tmp/tone.wav) & nix shell nixpkgs#socat -c timeout 5 socat -u TCP:127.0.0.1:4711 OPEN:/tmp/cap.raw,creat,trunc
# analyse /tmp/cap.raw as S16LE stereo 44.1 kHz: expect 440 Hz, peak ~9800, 5 s of data
```

**Testing mutter's RDP server locally, with no WSL at all.**
1. Build with the mutter dev shell (`./weaselway-build.sh`).
2. Run `_build/nix/src/mutter` headless:
   - `MUTTER_RDP=1 MUTTER_RDP_DEBUG_TCP=1 MUTTER_RDP_PORT=13389`;
   - point `WSL2_SHARED_MEMORY_MOUNT_POINT` at a non-mount, which forces the error frame, or at
     `/dev/shm`, which passes the mount check but no client will confirm gfxredir;
   - a compiled schema dir in `GSETTINGS_SCHEMA_DIR`;
   - a `dbus-run-session` with an explicit `--config-file`;
   - `--mutter-plugin` pointing at `_build/nix/src/compositor/plugins/libdefault.so`.
3. Connect `xfreerdp /v:127.0.0.1:13389 /cert:ignore` under `xvfb-run` and grab the screen with `xwd`.

This verified both error-frame paths and the in-memory TLS certificate.

## Things that were wrong and are fixed (don't reintroduce)

- **Codec fallback in mutter.** Removed on purpose: gfxredir or the magenta error frame, nothing in
  between. A client that never confirms gfxredir caps gets the error frame after 5 s; before, it got a
  silent black screen.
- **`winpr-makecert`.** mutter used to spawn it to make the TLS certificate, and it isn't on PATH on
  NixOS. mutter now makes a self-signed RSA-2048 certificate with libcrypto, in memory. The certificate
  can't be dropped: FreeRDP's server refuses to initialize a peer without one, even for standard RDP
  security. That was tried.
- **Distorted audio (NixOS only).**
  - nixpkgs builds FreeRDP with FFmpeg. Its FFmpeg DSP backend mapped 16-bit PCM to
    `AV_CODEC_ID_PCM_U16LE`, which is unsigned. rdpsnd passes every packet through
    `freerdp_dsp_encode()`, even PCM to identical PCM, so every sample came out shifted by 32768 and
    speech was unintelligible.
  - The microphone (audin, decode direction) was affected the same way.
  - A 20-line C harness against `libfreerdp3` showed the shift, and byte-identical output with the
    fix.
  - Fixed in `weaselway-freerdp` (overlay patch) and on `weaselway/freerdp` main.
  - Ubuntu's `freerdp3` is built without FFmpeg ("Ubuntu can't have ffmpeg in main") and uses FreeRDP's
    own PCM path, so it was never affected. Debian's is affected.

## Open / not yet verified on real WSL

- **The PCM fix**, end to end: it is built, but hasn't been heard through the viewer yet.
- **PipeWire runs without realtime scheduling.** rtkit is enabled and running, but `data-loop.0` stays
  `SCHED_OTHER`.
- **PipeWire delivers audio to mutter in quantum-sized bursts**, while mutter sends 20 ms packets.
  protocol-simple has no chunk size of its own: each graph cycle's output is written to the TCP
  socket as-is, and mutter cuts the byte stream into 882-frame packets, so bursts only matter if the
  client buffers too little. Nothing in our config sets the quantum. The stock default is 1024
  frames (~23 ms at 44.1 kHz), and the stock `pipewire.conf` raises the minimum to 1024 inside a VM.
  It grows up to the 2048-frame max (~43 ms at 48 kHz) when a client asks for high latency; that is
  what was observed here. If playback stutters, check `pw-top` while audio plays. A smaller quantum
  needs `default.clock.min-quantum` lowered as well as `node.latency` on the capture stream,
  because the VM rule clamps anything below 1024.
- **Running the viewer from `\\wsl.localhost`** hasn't been confirmed yet.
- **WSL may not like the minimal WSLGd (v1.0.79-4)** idling as the system distro's only process. The
  test machine was still on an older system image (`49a4cc0`).
- **Whether `sdl-freerdp.exe` accepts the derived `/wslgsharedmemorypath:`** and mutter binds vsock
  3389 unprivileged still hasn't been seen with the minimal image. It worked with the older image and
  the new scripts.
- **dxgdrm rules:** on one boot the render node was `0666`, as the rule intends. On another the user
  saw `root:render`; the groups cover it either way.
- **Ubuntu's mutter packages need a rebuild** from `50.1-wslg` (`weasel5`) to get the error frame and
  the in-memory certificate. `winpr-utils` stays in the Ubuntu install list until then.
