# Weaselway on Ubuntu 26.04

This guide installs weaselway on an Ubuntu 26.04 WSL distro instead of the
NixOS image. You get the stock Ubuntu GNOME session on the virtual display,
shown on Windows by the same viewer. It is for people who want weaselway in a
distro they already know, and who can wait for Mesa to compile.

[install.sh](install.sh) does all of the work. It takes GNOME and PipeWire
(the audio server) from Ubuntu, compiles the patched Mesa and weaselwayd, and
downloads the dxgdrm kernel module and the Windows viewer from GitHub
releases. The steps below explain what to run before and after it.

## Requirements

- Everything listed under [Requirements](../README.md#requirements) in the
  main README: an x86_64 Windows machine with WSL2, a GPU driver with WSL
  support, WSLg enabled and one of the supported WSL kernels.
- About 5 GB of disk space in the distro for the packages and the Mesa build.
- About 15 minutes on a laptop with 8 CPU threads, most of it for apt and the
  Mesa compile.

## 1. Install Ubuntu

In PowerShell:

```powershell
wsl --install Ubuntu-26.04
```

Ubuntu asks for a user name and a password on the first start. The password
is the one for `sudo` and for the GNOME lock screen.

systemd is on by default in Ubuntu's WSL image. If you changed that, set
`systemd=true` under `[boot]` in `/etc/wsl.conf` and run `wsl --shutdown`.

## 2. Run the install script

Inside the distro:

```sh
git clone https://github.com/weaselway/weaselway.git
cd weaselway
./ubuntu/install.sh
```

The script asks for your password for `sudo`. You can run it again at any
time; it rebuilds and reinstalls everything.

The script changes the system in these places:

- It enables source packages (`deb-src`) in
  `/etc/apt/sources.list.d/ubuntu.sources`, so that apt can install Mesa's
  build dependencies.
- It blocks `gdm3`, NetworkManager, ModemManager, wpa_supplicant, CUPS,
  BlueZ, Plymouth and snapd in `/etc/apt/preferences.d/weaselway`. GNOME
  recommends them, but in WSL a display manager would take the virtual
  display, and NetworkManager would interfere with the network WSL sets up.
- It installs Mesa to `/opt/weaselway/mesa` and puts that directory ahead of
  Ubuntu's libraries with `/etc/ld.so.conf.d/00-weaselway-mesa.conf`. Ubuntu's
  Mesa packages stay installed but are no longer used.
- It installs weaselwayd and the `ww-*` scripts to `/usr/local/bin`, the
  dxgdrm module to `/opt/weaselway/dxgdrm` and the viewer to
  `/opt/weaselway/viewer`.
- It adds the system unit `weaselway-prep.service`, which loads the module at
  boot, and the user unit `weaselwayd.service`.
- It adds your user to the groups `render`, `video` and `input`.
- It masks WSL's `wslg-session.service` for all users. That unit replaces
  PipeWire's PulseAudio socket with a link to WSLg's PulseAudio, and the
  desktop would then show no audio devices.
- It masks `tpm-udev.path`, which GNOME's dependencies install. WSL has no
  TPM (the security chip), and the unit fails at every boot.

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

`ww-start-session` starts GNOME on the virtual display. `ww-start-viewer`
opens it in a window on Windows. `ww-install-viewer-link` puts a shortcut on
the Windows desktop that does both. The main README describes these commands
under [Installation](../README.md#3-start-the-session) and
[The viewer](../README.md#the-viewer).

To stop the session, run `ww-start-session stop`.

## Updating

Pull the repository and run the script again:

```sh
cd weaselway
git pull
./ubuntu/install.sh
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

- mutter is Ubuntu's, without weaselway's patch. When you resize the viewer's
  window, the GNOME overview keeps its old size until the next time it opens.
- Only GNOME is set up. Other compositors work through
  [custom sessions](../README.md#other-compositors) once you install them.

## Troubleshooting

The checks in [Troubleshooting](../README.md#troubleshooting) and
[Debugging](../ARCHITECTURE.md#debugging) apply here too. Two checks are
specific to this setup:

- `eglinfo -B -p surfaceless` should report a renderer such as
  `D3D12 (NVIDIA GeForce 940MX)`. If it reports `llvmpipe`, the patched Mesa
  or the Windows GPU driver did not load. `ldconfig -p | grep libEGL_mesa`
  should list `/opt/weaselway/mesa/lib` first.
- `systemctl status weaselway-prep` reports when no module exists for the
  running kernel.
