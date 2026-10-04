# Weaselway

A GPU-accelerated GNOME desktop on WSL2, shipped as a NixOS-WSL image.

The compositor is the stock one. The `dxgdrm` kernel module gives it a virtual
display to drive, the same way it would drive a monitor, and mesa's `d3d12`
driver renders on the GPU Windows exposes. A small daemon, `weaselwayd`, picks
up each frame and hands it to a FreeRDP client on the Windows side through
shared memory; the client's keyboard, mouse, touchpad and audio come back the
same way. Everything is in the image: the patched mesa, the kernel module,
weaselwayd, the PipeWire audio bridge, and the Windows viewer itself.

Because the compositor needs nothing special, Plasma works the same way. It is
not in the image, but one option turns it on (see "Configuration").

The one piece outside the image is a minimal WSLg **system distro**
([weaselway/wslg](https://github.com/weaselway/wslg)). WSL only creates the
shared memory the frames are handed over on when a system distro is
configured. Without it, the viewer cannot show the desktop.

How the pieces work, and how to debug them, is in [NIXOS.md](NIXOS.md).

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

This opens a shell as the user `nixos`. There is no password, and `sudo` does
not ask for one.

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
start-session
start-viewer
```

`start-session` starts GNOME on the virtual display and returns once it is up.
`start-viewer` opens the session in a window on Windows. It runs the
`sdl-freerdp.exe` inside the image, so nothing needs installing on the Windows
side. Closing the window leaves the session running; `start-viewer` shows it
again.

**This has to be the first distro started after `wsl --shutdown`.** WSL only
sets up `/run/user/1000` for the first distro it starts. If another distro got
there first, run `wsl --shutdown` and start this one first.

To stop the session:

```sh
start-session stop
```

To get there with a double click, put a shortcut on the Windows desktop:

```sh
install-viewer-link
```

The shortcut starts the distro if it is not running, the session if there is
none, and the viewer. `install-viewer-link <name>` gives it another name than
"Weaselway". It opens a minimized console window next to the viewer, which
closes with it.

`start-session` takes the session to start: `gnome` (the default),
`gnome-shell` for the bare shell without the desktop's services, and `plasma`
or `kwin` if Plasma is enabled.

## Using the viewer

`start-viewer` passes any extra arguments on to `sdl-freerdp.exe`, after its
own. So FreeRDP options can be added or overridden per run.

The keyboard arrives in the session as an ordinary keyboard, so its layout is
set in the desktop's own settings (GNOME: Settings, Keyboard, Input Sources),
not on the viewer.

Two options help with debugging:

```sh
start-viewer /sdl-show-stats:2      # frame and bandwidth counter overlay, text at 2x
start-viewer /sdl-show-damage       # tint the regions updated in each frame
```

The value after `/sdl-show-stats` is the text scale and can be left out. The
options combine.

The viewer also forwards touchpad gestures of 3+ fingers (the session sees a
touchpad of its own, so the desktop's swipes and pinches work), audio playback
and the microphone. To use a different
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
| `weaselway.enable` | `false` (the image sets `true`) | The whole session: mesa, dxgdrm, weaselwayd, audio, the scripts. |
| `weaselway.adapter` | `null` | GPU to render on, matched against a substring of its name (`"nvidia"`, `"Intel"`). Null takes the first adapter Windows lists. |
| `weaselway.session` | `"gnome"` | Session that `start-session` starts when given none. |
| `weaselway.plasma.enable` | `false` | Installs Plasma next to GNOME, for `start-session plasma`. |

`start-session --adapter <name>` overrides the adapter for a single session
without rebuilding.

Plasma's KWin carries a patch, so enabling it downloads KWin and what links it
from weaselway.cachix.org, or compiles them if CI has not built that
combination.

Everything else is plain NixOS: add packages to `environment.systemPackages`,
set `time.timeZone`, and so on. Some things the image sets that you may want to
change:

- **sshd is off.** Uncomment `services.openssh.enable` to SSH in, and set a
  password with `passwd` or add a key first. It is reachable from the LAN when
  WSL networking is mirrored.
- **The screen reader is left out** to keep the image small (orca's voices are
  about 650 MB). Delete the two lines that say so to get it back.

## Updating

`/etc/nixos/flake.lock` pins weaselway to the commit the image was built from,
so rebuilding after a config change doesn't pull in anything new. Updating means
moving the lock forward and rebuilding:

```sh
sudo nix flake update --flake /etc/nixos
sudo nixos-rebuild switch
```

This updates weaselway, and with it nixpkgs, NixOS-WSL, mesa, the kernel
module, weaselwayd and the viewer, all to versions that were tested together.

The patched mesa, mutter (nixpkgs' with one fix), gnome-shell (which links
mutter), weaselwayd and dxgdrm's kernel tree aren't on cache.nixos.org. CI pushes them to
[weaselway.cachix.org](https://weaselway.cachix.org), and the image already
has that cache configured, so an update downloads them. A config change that
alters one of them (a different mesa, say) still compiles it locally, which
takes a while and needs a few GB of disk.

After a rebuild, restart the session so it runs the new mesa: stop it as
above, then `start-session` again. If the kernel module changed,
`wsl --shutdown` is simpler.

If an update breaks something, go back to the previous generation:

```sh
sudo nixos-rebuild switch --rollback
```

The system distro VHD lives on the Windows side, so a rebuild doesn't replace
it. `install-system-image` pins a version, and an update can move that pin. Run
`install-system-image` again after updating: if it downloads a new image,
change the `systemDistro=` line to the one it prints and run `wsl --shutdown`.

## What doesn't work yet

- **Files cannot be copied between Windows and the session.** Text, formatted
  text and images can.
- **On Plasma the clipboard is not shared with Windows.** On GNOME it is.
- **The scale factor of the Windows display is not passed on.** Set the scale
  in the desktop's display settings.
- **Audio from browsers can crackle.** Other players are fine.
- **Touchscreens are not forwarded.** Touchpad gestures are.

## When something doesn't work

- **The viewer's window stays empty, or it exits right away:**
  - `journalctl --user -u weaselwayd` says what weaselwayd saw. "waiting for
    the compositor's first commit" means no session is running: run
    `start-session`.
  - If the shared memory is missing, weaselwayd does not start. Check that
    `systemDistro=` is in `.wslconfig`, that you ran `wsl --shutdown`
    afterwards, and that this distro was the first one started.
    `systemctl status weaselway-prep` reports why.
- **`start-session` fails:** `journalctl -u weaselway-session` has the
  compositor's output.
- **Everything renders in software (llvmpipe):** the Windows GPU driver didn't
  load. The graphics part of "What the module does" in [NIXOS.md](NIXOS.md)
  lists the known causes.
- **No audio devices in the desktop:** `wpctl status` should list "Remote
  Desktop Audio", and `/run/user/1000/pulse/native` should be a socket, not a
  symlink.

"Debugging" in [NIXOS.md](NIXOS.md) has the full list of checks.
