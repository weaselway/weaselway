# Weaselway

A GPU-accelerated GNOME session on WSL2, shipped as a NixOS-WSL image.

mutter runs headless inside the distro and serves the session over RDP on a
vsock. mesa's `d3d12` driver renders on the GPU Windows exposes. A patched
FreeRDP client on the Windows side shows the result. Everything is in the
image: the patched mesa and mutter, the `dxgdrm` kernel module, the PipeWire
audio bridge, and the Windows viewer itself.

The one piece outside the image is a minimal WSLg **system distro**
([weaselway/wslg](https://github.com/weaselway/wslg)). WSL only creates the
shared memory mutter hands its frames over on when a system distro is
configured. Without it, the viewer shows a magenta error frame instead of the
desktop.

The older Ubuntu setup (patched packages plus the `install-*.sh` scripts) is
in [README-ubuntu.md](README-ubuntu.md). How the NixOS side works, and how to
debug it, is in [NIXOS.md](NIXOS.md).

## Installation

### 1. Get the image

Download `nixos-weaselway-<version>.wsl` from the
[releases](https://github.com/weaselway/weaselway/releases). Every CI run also
uploads one as the artifact `nixos-wsl-<commit>`. To build it yourself, see
"Building" in [NIXOS.md](NIXOS.md). The image is x86_64 only.

### 2. Import it

In PowerShell:

```powershell
wsl --install --from-file nixos-weaselway-<version>.wsl --name Gnome
wsl -d Gnome
```

This opens a shell as the user `nixos`. There is no password yet. Set one
first, because `sudo` needs it:

```sh
passwd
```

### 3. Install the system distro

Inside the distro:

```sh
install-system-image
```

This downloads the system image to `C:\Weaselway\system_x64-<version>.vhd` and
prints the line to add to `%USERPROFILE%\.wslconfig`:

```ini
[wsl2]
systemDistro=C:\\Weaselway\\system_x64-<version>.vhd
```

If `.wslconfig` already has a `[wsl2]` section, put the line under that one.
Then restart WSL from PowerShell:

```powershell
wsl --shutdown
```

### 4. Start the session

```powershell
wsl -d Gnome
```

```sh
start-gnome-shell
start-viewer
```

`start-gnome-shell` starts GNOME under the user's systemd manager and returns
once it is up. `start-viewer` opens the session in a window on Windows. It runs
the `sdl-freerdp.exe` inside the image, so nothing needs installing on the
Windows side.

**This has to be the first distro started after `wsl --shutdown`.** WSL only
sets up `/run/user/1000` for the first distro it starts. If another distro got
there first, run `wsl --shutdown` and start this one first.

To stop the session:

```sh
systemctl --user start gnome-session-shutdown.target
```

## Using the viewer

`start-viewer` passes any extra arguments on to `sdl-freerdp.exe`, after its
own. So FreeRDP options can be added or overridden per run. The keyboard layout
is `/kbd:layout:German` unless you pass another:

```sh
start-viewer /kbd:layout:"United States - English"
start-viewer /kbd:layout:0x409        # the same layout, by its Windows ID
```

The names are FreeRDP's layout names, not country names (`German`, `French`,
`Swiss German`, `United Kingdom`, ...). An unknown name makes the viewer exit
with "Could not identify keyboard layout".

Two options help with debugging:

```sh
start-viewer /sdl-show-stats:2      # frame and bandwidth counter overlay, text at 2x
start-viewer /sdl-show-damage       # tint the regions updated in each frame
```

The value after `/sdl-show-stats` is the text scale and can be left out. The
options combine.

The viewer also enables touch input, touchpad swipes of 3+ fingers (overview
and workspace switching), audio playback and the microphone. To use a different
build of the viewer, point `WEASELWAY_VIEWER` at it:

```sh
WEASELWAY_VIEWER=/mnt/c/Weaselway/sdl-freerdp.exe start-viewer
```

## Configuration

The system is configured by the NixOS flake in `/etc/nixos`, which the image
ships:

- `flake.nix` pulls in weaselway (`github:weaselway/weaselway`), and through it
  the matching nixpkgs and NixOS-WSL.
- `configuration.nix` is the system itself, the same file the image was built
  from ([nix/image/configuration.nix](nix/image/configuration.nix)).

To change something, edit `configuration.nix` and rebuild:

```sh
sudo -e /etc/nixos/configuration.nix
sudo nixos-rebuild switch
```

The hostname is `nixos`, so `nixos-rebuild` picks `nixosConfigurations.nixos`
from that flake without a `#name`.

These are the weaselway options:

| Option | Default | What it does |
|---|---|---|
| `weaselway.enable` | `false` (the image sets `true`) | The whole session: mesa, mutter, dxgdrm, audio, the scripts. |
| `weaselway.adapter` | `null` | GPU to render on, matched against a substring of its name (`"nvidia"`, `"Intel"`). Null takes the first adapter Windows lists. |
| `weaselway.session` | `"gnome"` | gnome-session that `start-gnome-shell` starts when given none. |

`start-gnome-shell --adapter <name>` overrides the adapter for a single session
without rebuilding.

Everything else is plain NixOS: add packages to `environment.systemPackages`,
set `time.timeZone`, and so on. Some things the image sets that you may want to
change:

- **sshd is off.** Uncomment `services.openssh.enable` to SSH in, and set a
  password with `passwd` or add a key first. It is reachable from the LAN when
  WSL networking is mirrored.
- **The screen reader is left out** to keep the image small (orca's voices are
  about 650 MB). Delete the two lines that say so to get it back.

## Updating

`/etc/nixos` has no `flake.lock` until the first rebuild. The first
`nixos-rebuild` pins whatever weaselway is current at that moment. After that,
updating means moving the lock forward and rebuilding:

```sh
sudo nix flake update --flake /etc/nixos
sudo nixos-rebuild switch
```

This updates weaselway, and with it nixpkgs, NixOS-WSL, mesa, mutter, the
kernel module and the viewer, all to versions that were tested together.

**Rebuilds compile locally.** The binary cache doesn't have the patched mesa,
mutter, gnome-shell (which links mutter) or dxgdrm's kernel tree. An update
that touches them takes a while and needs a few GB of disk. Everything else
comes from the cache.

After a rebuild, restart the session so it runs the new mutter and mesa: stop
it as above, then `start-gnome-shell` again. If the kernel module changed,
`wsl --shutdown` is simpler.

If an update breaks something, go back to the previous generation:

```sh
sudo nixos-rebuild switch --rollback
```

The system distro VHD lives on the Windows side, so a rebuild doesn't replace
it. `install-system-image` pins a version, and an update can move that pin. Run
`install-system-image` again after updating: if it downloads a new image,
change the `systemDistro=` line to the one it prints and run `wsl --shutdown`.

## When something doesn't work

- **Magenta screen in the viewer:** the shared memory is missing. Check that
  `systemDistro=` is in `.wslconfig`, that you ran `wsl --shutdown` afterwards,
  and that this distro was the first one started.
  `systemctl status weaselway-prep` reports why.
- **Everything renders in software (llvmpipe):** the Windows GPU driver didn't
  load. The graphics part of "What the module does" in [NIXOS.md](NIXOS.md)
  lists the known causes.
- **No audio devices in GNOME:** `wpctl status` should list "Remote Desktop
  Audio", and `/run/user/1000/pulse/native` should be a socket, not a symlink.

"Debugging" in [NIXOS.md](NIXOS.md) has the full list of checks.
