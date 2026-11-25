# Weaselway on Ubuntu, Arch Linux and CachyOS

This guide installs weaselway into an Ubuntu 26.04, Arch Linux or CachyOS WSL
distro instead of the NixOS image. You get GNOME, Plasma or Hyprland on the
virtual display, shown on Windows by the same viewer. It is for people who
want weaselway in a distro they already know, and who can wait for Mesa and
the compositor to compile.

[install.py](install.py) does all of the work. It takes the desktop and
PipeWire (the audio server) from the distro and rebuilds the compositor
package with weaselway's patches. It compiles the patched Mesa and weaselwayd,
and downloads the dxgdrm kernel module and the Windows viewer from GitHub
releases. The steps below explain what to run before and after it.

## Requirements

- Everything listed under [Requirements](../README.md#requirements) in the
  main README: an x86_64 Windows machine with WSL2, a GPU driver with WSL
  support, WSLg enabled and one of the supported WSL kernels.
- About 8 GB of disk space in the distro for the packages and the builds, and
  about 4 GB more with `--flatpak`.
- About 30 to 60 minutes on a laptop with 8 CPU threads, most of it for the
  package manager, Mesa and the compositor. `--flatpak` adds about 15 minutes.

## 1. Install the distro

**Ubuntu**, in PowerShell:

```powershell
wsl --install Ubuntu-26.04
```

Ubuntu asks for a user name and a password on the first start. The password
is the one for `sudo`.

**Arch Linux**, in PowerShell:

```powershell
wsl --install archlinux
```

That image starts as `root` and has neither `sudo`, `git` nor Python. Set up
the package keys, install them, and create a user:

```sh
pacman-key --init
pacman-key --populate archlinux
pacman -Syu sudo git python
echo '%wheel ALL=(ALL:ALL) ALL' > /etc/sudoers.d/10-wheel
useradd -m -G wheel -s /bin/bash weasel
passwd weasel
printf '\n[user]\ndefault=weasel\n' >> /etc/wsl.conf
```

Then run `wsl --terminate archlinux` and start the distro again as that user.

**CachyOS** publishes a WSL image
([wiki](https://wiki.cachyos.org/virtualization/wsl),
[releases](https://github.com/CachyOS/cachyos-wsl/releases)):

```powershell
wsl --install --from-file cachyos-<version>.wsl --name CachyOS
```

Its first start creates a user with `sudo`. WSL always runs the Microsoft
kernel, so `linux-cachyos` and its schedulers are not used.

systemd is on by default in all three images. If you changed that, set
`systemd=true` under `[boot]` in `/etc/wsl.conf` and run `wsl --shutdown`.

## 2. Run the install script

Inside the distro:

```sh
git clone https://github.com/weaselway/weaselway.git
cd weaselway
./distro/install.py
```

The script asks which desktop to install, whether to build Mesa for Flatpak
apps and whether to set up Bluetooth. The options answer the questions on the
command line instead:

- `--desktop gnome|kde|hyprland` picks the desktop.
- `--flatpak` builds Mesa for Flatpak apps, see [Flatpak apps](#flatpak-apps).
  `--no-flatpak` skips it.
- `--bluetooth` sets up Bluetooth adapters attached with usbipd, see
  [Bluetooth](#bluetooth). `--no-bluetooth` skips it.
- `--jobs N` sets the number of parallel compile jobs. The default is one per
  CPU thread, at most 8: a WSL VM with the default 8 GiB of RAM swaps with
  more during the Mesa and KWin builds, which looks like a hung build.
- `--no-leave-idle-to-windows` lets the desktop lock and turn off the screen
  when idle, see [Desktops](#desktops).
- `--quick-build` compiles without optimization. It is for CI: the builds
  finish sooner, and the desktop runs slower.

The script asks for your password for `sudo`. You can run it again at any
time, for example with another desktop; it rebuilds and reinstalls what
changed.

The script changes the system in these places:

- It installs the desktop from the distro without a display manager, since
  `ww-start-session` creates the login session itself. On Ubuntu it blocks
  `gdm3`, `sddm`, NetworkManager, ModemManager, wpa_supplicant, CUPS, BlueZ
  (unless `--bluetooth`), Plymouth and snapd in
  `/etc/apt/preferences.d/weaselway`. The desktops recommend them, but in WSL
  a display manager would take the virtual display, and NetworkManager would
  interfere with the network WSL sets up. On Arch it installs the desktop's
  packages one by one, since `plasma-meta` and the `gnome` group pull a
  display manager.
- On Ubuntu it removes snapd and all installed snaps. Snaps bring their own
  Mesa, which cannot use the GPU through dxgdrm. Ubuntu's `chromium` and
  `firefox` packages only install snaps. With `--flatpak`, install both from
  Flathub. Otherwise use another source of `.deb`s, such as Mozilla's apt
  repository for Firefox. It also enables source packages (`deb-src`) in
  `/etc/apt/sources.list.d/ubuntu.sources` for the build dependencies.
- It rebuilds the compositor package with weaselway's patches, see
  [Patched packages](#patched-packages). On Arch it also rebuilds FreeRDP.
- It installs Mesa to `/opt/weaselway/mesa` and puts that directory ahead of
  the distro's libraries with `/etc/ld.so.conf.d/00-weaselway-mesa.conf`. The
  distro's Mesa packages stay installed but are no longer used.
- On Arch it adds `libedit.so.2` for Intel's Windows driver, which Arch ships
  as `libedit.so.0`. Without it d3d12 falls back to llvmpipe (Mesa's CPU
  renderer) on Intel GPUs.
- It installs weaselwayd and the `ww-*` scripts to `/usr/local/bin`, the
  dxgdrm module to `/opt/weaselway/dxgdrm` and the viewer to
  `/opt/weaselway/viewer`.
- It adds the system unit `weaselway-prep.service`, which loads the module at
  boot, and the user unit `weaselwayd.service`, which runs only for your user.
- It adds your user to the groups `render`, `video` and `input`, and makes the
  chosen desktop the default session of `ww-start-session`.
- It masks WSL's `wslg-session.service` for all users. That unit replaces
  PipeWire's PulseAudio socket with a link to WSLg's PulseAudio, and the
  desktop would then show no audio devices.
- It installs UPower, which shows the laptop's battery that Hyper-V passes
  into WSL. `/etc/UPower/UPower.conf.d/weaselway.conf` stops UPower from
  shutting the distro down when the battery is critical, and
  `/etc/systemd/sleep.conf.d/weaselway.conf` stops the desktop from
  suspending it when idle. Windows handles both for the whole machine.
- It masks `tpm-udev.path` where it exists. WSL has no TPM (the security
  chip), and the unit fails at every boot.

## 3. Restart WSL

The new groups, the module and the environment take effect after a restart.
In PowerShell:

```powershell
wsl --shutdown
```

## 4. Start the desktop

Start the distro from the Windows desktop, for example from Windows Terminal
or the Start menu. Do not start it over SSH or Remote Desktop: the viewer can
only show the session in the Windows session that started WSL.

Inside the distro:

```sh
ww-start-session
ww-start-viewer
```

`ww-start-session` starts the desktop on the virtual display.
`ww-start-viewer` opens it in a window on Windows. `ww-install-viewer-link`
puts a shortcut on the Windows desktop that does both. The main README
describes these commands under [Using it](../README.md#using-it). To stop the
session, run `ww-start-session stop`.

## Desktops

The script leaves locking and turning off the screen when idle to Windows.
It installs defaults that the desktop's settings can still change, and
`--no-leave-idle-to-windows` removes them again:

**GNOME** has no lock screen here: gnome-shell only locks when GDM runs, and
the script keeps GDM out. Lock Windows instead. The script also keeps GNOME
from going idle, with `idle-delay` 0 in
`/usr/share/glib-2.0/schemas/90_weaselway.gschema.override`.

**Plasma** neither dims, turns off nor locks the screen when idle, set in
`/etc/xdg/powerdevilrc` and `/etc/xdg/kscreenlockerrc`. The script keeps
these files if you wrote them yourself. With `--no-leave-idle-to-windows`,
Plasma blanks the virtual display and locks the session after it has been
idle for a while. Moving the mouse in the viewer wakes it. If it does not,
run `kscreen-doctor --dpms on` and log in.

**Hyprland** starts through `ww-start-session custom`, which runs
`/usr/local/bin/custom-weaselway-session`. The script writes that file unless
you already have one of your own. It points Hyprland at dxgdrm's card, since
`card0` belongs to WSL's vgem (a virtual graphics device that cannot render).
Hyprland's default configuration opens kitty with Super+Q.

## Patched packages

The NixOS image patches the compositors, and the script does the same for the
distro's packages, with the patches from [nix/](../nix):

- mutter (GNOME): `mutter-stage-relayout.patch` resizes the overview with the
  viewer's window, and `mutter-kms-fd0.patch` keeps GNOME from hanging when
  WSL's vgem device comes back. From mutter 50.4 on,
  `mutter-viewport-in-fb.patch` keeps windows from leaving trails on Intel
  HD/UHD 620/630 GPUs when the desktop has several monitors.
- KWin (Plasma): the FB_DAMAGE_CLIPS backport for the KWin release. Without it
  KWin reports the whole screen as changed in every frame, and weaselwayd
  reads back all of it (`100% of the screen each` in the statistics line in
  `journalctl --user -u weaselwayd -f`).
- aquamarine and Hyprland: the backports that let Hyprland follow the size of
  the viewer's window.
- FreeRDP, on Arch only: `freerdp-dsp-ffmpeg-pcm-s16.patch`. Arch's FreeRDP
  uses FFmpeg for audio, which treats 16-bit PCM as unsigned, and audio and
  the microphone are garbled without the patch. Ubuntu's FreeRDP is built
  without FFmpeg.

On Ubuntu the script downloads the source package with `apt-get source`,
applies the patches and builds the packages with `dpkg-buildpackage`; the
version gets a `+weaselway1` suffix. On Arch it builds the package from Arch's
PKGBUILD at the version in the repositories, applies the patches after Arch's
own `prepare()`, and adds `.1` to the `pkgrel`. Either way the patched
package sorts above the distro's, and a normal upgrade keeps it until the
distro ships a newer version. After such an update, run the script again to
rebuild the package.

The patches are made for specific releases. When the distro moves to a
release that has no backport yet, the script stops with an error that names
the package and the version.

## Flatpak apps

Flatpak apps run on a runtime with its own Mesa, which has no d3d12 driver,
and in a sandbox without `/dev/dxg` and WSL's libraries. With `--flatpak`,
the script builds weaselway's Mesa against the freedesktop runtimes `25.08`
and `26.08`, which most current apps are based on. It installs the result as
the local GL extension `org.freedesktop.Platform.GL.host`, which Flatpak
prefers over the runtime's Mesa. At boot, `weaselway-prep.service` creates
`/dev/dri/dxg`, a copy of `/dev/dxg` that apps with GPU access see, and binds
`/usr/lib/wsl` into the extension.

Chromium and Firefox from Flathub are on `25.08` and render on the GPU:

```sh
flatpak install flathub org.chromium.Chromium org.mozilla.firefox
```

Apps on older runtimes, GNOME 48 and earlier for example, keep their
runtime's Mesa, which renders on the CPU. Without LLVM in the SDK the
extension has softpipe instead of llvmpipe as the CPU fallback, which is
slow.

## Bluetooth

WSL's kernel has no rfkill (the kernel's switch for radios), and the desktops
hide a Bluetooth adapter without it. With `--bluetooth` the script installs
BlueZ and downloads Bluetooth modules rebuilt with rfkill, plus the firmware
Realtek adapters need, from the latest weaselway release.
`weaselway-bluetooth.service` loads them in place of WSL's own at boot, and a
udev rule hands the firmware to the kernel.

Attach the adapter with [usbipd](../README.md#usb-devices-and-bluetooth)
after the distro has started. If it was attached earlier, the stock modules
are loaded already: `systemctl status weaselway-bluetooth` says so, and
`wsl --shutdown` followed by attaching after the start fixes it.

## Updating

Pull the repository and run the script again:

```sh
cd weaselway
git pull
./distro/install.py
```

Run it again after `wsl --update` installs a new WSL kernel, too. The script
then downloads the module for that kernel, or stops with an error if dxgdrm
has no release for it yet.

## Choosing the GPU

On a machine with two GPUs, d3d12 renders on the first one Windows lists. On
the test laptop that was not the same GPU after every restart. To choose one,
put a part of its name into
`/etc/environment.d/20-weaselway-adapter.conf`:

```sh
echo MESA_D3D12_DEFAULT_ADAPTER_NAME=nvidia | sudo tee /etc/environment.d/20-weaselway-adapter.conf
```

This takes effect after `wsl --shutdown`. `ww-start-session --adapter nvidia`
chooses the GPU for one session only.

## Differences from the NixOS image

- The distro's packages are patched by the script, so a distro update can
  replace a patched package until you run the script again.
- usbipd auto-attach (`weaselway.usbip.devices`) is not set up. Attach
  devices by hand with `usbipd attach --wsl`.
- Other compositors work through
  [custom sessions](../README.md#other-compositors) once you install them.

## Troubleshooting

The checks in [Troubleshooting](../README.md#troubleshooting) and
[Debugging](../ARCHITECTURE.md#debugging) apply here too. These checks are
specific to this setup:

- `eglinfo -B -p surfaceless` should report a renderer such as
  `D3D12 (NVIDIA GeForce 940MX)`. If it reports `llvmpipe`, the patched Mesa
  or the Windows GPU driver did not load. `ldconfig -p | grep libEGL_mesa`
  should list `/opt/weaselway/mesa/lib` first.
- `systemctl status weaselway-prep` reports when no module exists for the
  running kernel.
- The patched packages have `+weaselway1` (Ubuntu) or a `pkgrel` ending in
  `.1` (Arch) in their version: check with `dpkg -l | grep weaselway` or
  `pacman -Q mutter kwin hyprland aquamarine freerdp`. Without it, a distro
  update replaced the package; run the script again.
- In a Flatpak app on a `25.08` or `26.08` runtime,
  `flatpak run --command=eglinfo <app> -B -p x11` should report a D3D12
  renderer, if the app ships `eglinfo`. If it reports `softpipe`, check that
  `/dev/dri/dxg` exists and that `systemctl status weaselway-prep` shows no
  warnings.
