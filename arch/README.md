# Weaselway on Arch Linux and CachyOS

This guide installs weaselway on an Arch Linux or CachyOS WSL distro instead
of the NixOS image. You get a Plasma desktop on the virtual display, shown on
Windows by the same viewer. It is for people who want weaselway in a distro
they already know, and who can wait for Mesa to compile.

[install.sh](install.sh) does all of the work. It takes Plasma and PipeWire
(the audio server) from the distro, compiles the patched Mesa and weaselwayd,
builds FreeRDP and KWin with weaselway's patches, and downloads the dxgdrm
kernel module and the Windows viewer from GitHub releases. The steps below
explain what to run before and after it.

## Requirements

- Everything listed under [Requirements](../README.md#requirements) in the
  main README: an x86_64 Windows machine with WSL2, a GPU driver with WSL
  support, WSLg enabled and one of the supported WSL kernels.
- About 8 GB of disk space in the distro for the packages and the builds.
- About 30 minutes on a laptop with 8 CPU threads, most of it for pacman and
  the Mesa, FreeRDP and KWin compiles. `--without-kwin-patch` saves about
  10 minutes.

## 1. Install the distro

CachyOS publishes an official WSL image
([wiki](https://wiki.cachyos.org/virtualization/wsl),
[releases](https://github.com/CachyOS/cachyos-wsl/releases)):

```powershell
wsl --install --from-file cachyos-<version>.wsl --name CachyOS
```

Its first start asks a few questions, creates a user with `sudo` and switches
to CachyOS's optimized repositories. WSL always runs the Microsoft kernel, so
`linux-cachyos` and its schedulers are not used.

Arch Linux publishes an official WSL image too:

```powershell
wsl --install archlinux
```

That image starts as `root` and has neither `sudo` nor `git`. Create a user
and install them:

```sh
pacman -S sudo git
echo '%wheel ALL=(ALL:ALL) ALL' > /etc/sudoers.d/10-wheel
useradd -m -G wheel -s /bin/bash weasel
passwd weasel
printf '\n[user]\ndefault=weasel\n' >> /etc/wsl.conf
```

`sudoers.d/10-wheel` lets any `wheel` member use `sudo` with a password;
change the line to `%wheel ALL=(ALL:ALL) NOPASSWD: ALL` if you do not want the
prompt. Then run `wsl --terminate archlinux` and start the distro as that user.
systemd is on by default in both images; if you changed that, set
`systemd=true` under `[boot]` in `/etc/wsl.conf` and restart the distro.

## 2. Run the install script

Inside the distro:

```sh
git clone https://github.com/weaselway/weaselway.git
cd weaselway
./arch/install.sh
```

The script asks for your password for `sudo`. `--without-kwin-patch` leaves
the distro's KWin alone, see [KWin](#kwin). You can run it again at any time;
it rebuilds and reinstalls everything. The script caps compile jobs at 8
(`WEASELWAY_BUILD_JOBS` overrides it): a WSL VM with the default 8 GiB of RAM
otherwise swaps during the Mesa and KWin builds, which looks like a hung
build.

The script changes the system in these places:

- It installs `plasma-desktop` and its dependencies from the distro, without
  a display manager. `plasma-meta` is left out because it pulls Plasma's login
  manager.
- It builds FreeRDP with two patches and installs it over the repository
  package, see [FreeRDP](#freerdp).
- It installs Mesa to `/opt/weaselway/mesa` and puts that directory ahead of
  the distro's libraries with `/etc/ld.so.conf.d/00-weaselway-mesa.conf`. The
  distro's Mesa packages stay installed but are no longer used. Mesa is the
  weaselway fork, like the NixOS image uses: the stock Mesa has no d3d12
  driver that knows dxgdrm.
- It adds `libedit.so.2` for Intel's Windows driver, which Arch ships as
  `libedit.so.0`. Without it d3d12 falls back to llvmpipe on Intel GPUs.
- It installs weaselwayd and the `ww-*` scripts to `/usr/local/bin`, the
  dxgdrm module to `/opt/weaselway/dxgdrm` and the viewer to
  `/opt/weaselway/viewer`.
- It installs the same units, environment files and PipeWire drop-in as the
  NixOS module: `weaselway-prep.service` loads the module at boot, and the
  user unit `weaselwayd.service` runs only for your user.
- It adds your user to the groups `render`, `video` and `input`, and sets
  Plasma as the default session of `ww-start-session`.
- It masks WSL's `wslg-session.service` for all users. That unit replaces
  PipeWire's PulseAudio socket with a link to WSLg's PulseAudio, and the
  desktop would then show no audio devices.
- It enables OpenSSL's legacy provider for weaselwayd, see
  [FreeRDP](#freerdp). OpenSSL 4 in Arch keeps MD4 and RC4 in that provider,
  and FreeRDP initialises them even when the session uses TLS.
- With the default `--with-kwin-patch`, it builds KWin with the
  FB_DAMAGE_CLIPS backport, see [KWin](#kwin).

## 3. Restart the distro

The new groups, the module and the environment take effect after a restart.
In PowerShell:

```powershell
wsl --terminate CachyOS
```

Use `wsl --shutdown` instead to restart all WSL distros, which also reloads the
kernel modules.

## 4. Start the desktop

Start the distro from the Windows desktop, for example from Windows Terminal
or the Start menu. Do not start it over SSH or Remote Desktop: the viewer can
only show the session in the Windows session that started WSL.

Inside the distro:

```sh
ww-start-session
ww-start-viewer
```

`ww-start-session` starts Plasma on the virtual display. `ww-start-viewer`
opens it in a window on Windows. `ww-install-viewer-link` puts a shortcut on
the Windows desktop that does both. The main README describes these commands
under [Using it](../README.md#using-it).

To stop the session, run `ww-start-session stop`.

Plasma blanks the virtual display and locks the session after it has been idle
for a while. Moving the mouse in the viewer wakes it; if it does not (there is
nothing to move it over in an empty window yet), run `kscreen-doctor --dpms on`
and log in, or disable the screen locker and DPMS in Plasma's settings. GNOME
has no lock screen here, but Arch and CachyOS come with Plasma.

## FreeRDP

The distro's FreeRDP is replaced with a build of the same release and two
patches:

- [../nix/freerdp-dsp-ffmpeg-pcm-s16.patch](../nix/freerdp-dsp-ffmpeg-pcm-s16.patch)
  fixes 16-bit PCM in the FFmpeg DSP backend (WAVE_FORMAT_PCM is signed at 16
  bits). Without it audio and the microphone are garbled the same way as
  without the NixOS module's `weaselway-freerdp`.
- [freerdp-drdynvc-server-start.patch](freerdp-drdynvc-server-start.patch)
  restores `drdynvc_server_start()`. FreeRDP 3.30.0 opened the channel,
  created a stop event and a thread, and returned success; somewhere before
  3.33.0 the function became an unconditional `ERROR_INTERNAL_ERROR`. A
  server that uses the public drdynvc server API cannot activate dynamic
  channels then, so weaselwayd's gfxredir channel never starts and the client
  disconnects while activating, with `Activate for peer failed` in
  `journalctl --user -u weaselwayd`.

The patch applies to FreeRDP 3.33.x. When the package moves to a new release,
check whether the function works upstream first; if it does, drop the patch
and the `prepare()` line in the script.

## KWin

KWin reports the whole buffer as damaged in every frame unless it sets
FB_DAMAGE_CLIPS on the commit, so weaselwayd reads back 100% of the screen
each frame (`100% of the screen each on average` in the statistics line in
`journalctl --user -u weaselwayd -f`). The NixOS image patches KWin per
release; the script does the same for the release in Arch's repositories,
with the backports in [nix/](../nix):

- `kwin-6.6-fb-damage-clips.patch` for KWin 6.6,
- `kwin-6.7-fb-damage-clips.patch` for KWin 6.7,
- `kwin-6.8-fb-damage-clips.patch` for KWin 6.8.

Without the patch Plasma runs, but every frame is read back in full: the
statistics line says 100% of the screen per frame and the frame rate drops
accordingly. With it, only the changed regions are read: a handful of percent
while the desktop is idle.

The package gets a `pkgrel` one above the repository's, so a routine rebuild of
the repository package does not replace it. A new KWin version does, and then
the script stops and asks for a new backport; run it with
`--without-kwin-patch` until the patch is added to [nix/](../nix).

## Updating

Pull the repository and run the script again:

```sh
cd weaselway
git pull
./arch/install.sh
```

Run it again after `wsl --update` installs a new WSL kernel, too. The script
then downloads the module for that kernel, or stops with an error if dxgdrm
has no release for it yet.

## Choosing the GPU

On a machine with two GPUs, d3d12 renders on the first one Windows lists. On
the test laptop that was not the same GPU after every restart. To choose one,
put a part of its name into `/etc/environment.d/20-weaselway-adapter.conf`:

```sh
echo MESA_D3D12_DEFAULT_ADAPTER_NAME=nvidia | sudo tee /etc/environment.d/20-weaselway-adapter.conf
```

This takes effect after a restart of the distro. `ww-start-session --adapter
nvidia` chooses the GPU for one session only.

## Differences from the NixOS image

- Only Plasma is set up. Other compositors work through
  [custom sessions](../README.md#other-compositors) once you install them.
- KWin is patched by the install script instead of through the NixOS overlay.
- FreeRDP is built here instead of from a Nix package, see [FreeRDP](#freerdp).
- There is no Flatpak Mesa extension like the one `ubuntu/install.sh` builds;
  Flatpak apps bring their own Mesa and render on the CPU. Use native packages
  instead.
- Bluetooth modules rebuilt with rfkill and the `usbipd` auto-attach service are
  not ported yet. Custom compositors, USB devices with usbipd and the rest of
  the main README work the same.

## Troubleshooting

The checks in [Troubleshooting](../README.md#troubleshooting) and
[Debugging](../ARCHITECTURE.md#debugging) apply here too. These checks are
specific to this setup:

- `eglinfo -B -p surfaceless` should report a renderer such as
  `D3D12 (NVIDIA GeForce RTX 4070 SUPER)`. If it reports `llvmpipe`, the
  patched Mesa or the Windows GPU driver did not load.
  `ldconfig -p | grep libEGL_mesa` should list `/opt/weaselway/mesa/lib` first.
- `systemctl status weaselway-prep` reports when no module exists for the
  running kernel.
- The viewer connects and disconnects immediately, and the journal says
  `client activating` followed by `Activate for peer failed`: weaselwayd is
  linked against a FreeRDP without the drdynvc patch. Check
  `pacman -Q freerdp`: its `pkgrel` must differ from the repository package's
  (the script appends `.3` to the PKGBUILD's `pkgrel`).
- The statistics line in `journalctl --user -u weaselwayd -f` says `100% of
  the screen each on average`, also while the desktop is idle: KWin is the
  repository package without the FB_DAMAGE_CLIPS backport. Check
  `pacman -Q kwin` and rerun the script with `--with-kwin-patch`.
- `pkg-config --exists freerdp3 freerdp-server3 winpr3` fails: the patched
  FreeRDP did not install its headers. Rerun the script; the patched package
  installs the same files as the repository one.
