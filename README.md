# Weaselway

Weaselway runs a GPU-accelerated Linux desktop on WSL2 and shows it in a
window on Windows. It is distributed as a NixOS-WSL image.

The compositor runs unmodified. The `dxgdrm` kernel module provides a virtual
display that it drives like a monitor, and Mesa's `d3d12` driver renders on the
GPU that Windows exposes to WSL. The `weaselwayd` daemon reads each finished
frame and passes it through shared memory to a FreeRDP-based viewer on Windows.
Keyboard, mouse, touchpad and audio travel over the same connection.

Any Wayland compositor with a KMS backend can run this way. The image includes
GNOME, Plasma is a single option away, and sway, Weston, Hyprland and others
start through a session script you provide. See [Sessions](#sessions).

The image contains the patched Mesa, the kernel module, weaselwayd, the audio
configuration and the Windows viewer. The only separate download is a minimal
WSLg system distro ([weaselway/wslg](https://github.com/weaselway/wslg)): WSL
creates the shared memory region used for the frames only when a system distro
is configured.

[NIXOS.md](NIXOS.md) describes the architecture, the build and how to debug it.

## Installation

The image is x86_64 only.

### 1. Get the image

Download `nixos-weaselway-<version>.wsl` from the
[releases](https://github.com/weaselway/weaselway/releases) page. Each CI run
also uploads an image as the artifact `nixos-wsl-<commit>`. To build one
yourself, see [Building](NIXOS.md#building).

### 2. Import it

In PowerShell:

```powershell
wsl --install --from-file nixos-weaselway-<version>.wsl --name Weaselway
wsl -d Weaselway
```

The shell runs as the user `nixos`. The account has no password, and `sudo`
does not ask for one.

### 3. Install the system distro

Inside the distro:

```sh
ww-install-system-image
```

This downloads the system image to `C:\Weaselway\system_x64-<version>.vhd` and
prints the setting to add to `%USERPROFILE%\.wslconfig`:

```ini
[wsl2]
systemDistro=C:\\Weaselway\\system_x64-<version>.vhd
```

If the file already has a `[wsl2]` section, add the line there. Then restart
WSL from PowerShell:

```powershell
wsl --shutdown
```

### 4. Start the session

```powershell
wsl -d Weaselway
```

```sh
ww-start-session
ww-start-viewer
```

`ww-start-session` starts the desktop on the virtual display and returns when
it is running. `ww-start-viewer` opens it in a window on Windows. The viewer,
`sdl-freerdp.exe`, runs from inside the image, so nothing is installed on the
Windows side. Closing the window does not end the session; run
`ww-start-viewer` again to reconnect.

> [!IMPORTANT]
> Weaselway must be the first distro started after `wsl --shutdown`. WSL sets
> up `/run/user/1000` only for the first distro it starts. If another distro
> was started first, run `wsl --shutdown` and start Weaselway first.

To stop the session:

```sh
ww-start-session stop
```

### Desktop shortcut

```sh
ww-install-viewer-link
```

This puts a shortcut named "Weaselway" on the Windows desktop;
`ww-install-viewer-link <name>` uses another name. The shortcut starts the
distro and the session if they are not running, then opens the viewer. A
minimized console window stays open next to the viewer and closes with it.

## Sessions

`ww-start-session [session]` accepts the following sessions. Without an
argument it starts the one set in `weaselway.session`, which is `gnome` unless
you change it.

| Session | What it starts |
|---|---|
| `gnome` | The GNOME desktop, as a display manager would start it. |
| `plasma` | The Plasma desktop. Requires `weaselway.plasma.enable`. |
| `custom` | The compositor of your choice, through `custom-weaselway-session`. |
| `gnome-shell` | GNOME Shell alone, for debugging. |
| `kwin` | KWin with a terminal, for debugging. Requires `weaselway.plasma.enable`. |

The two debugging sessions leave out the services that `gnome-session` and
Plasma start (settings daemon, keyring, portals and so on). Much of the
desktop does not work in them.

### Other compositors

`ww-start-session custom` runs the executable `custom-weaselway-session` from
your `PATH`. It runs in the same logind session on the seat as the built-in
sessions, so the compositor it starts gets the display and the input devices.
The script sets whatever environment the compositor needs and ends by
executing it:

```nix
environment.systemPackages = [
  pkgs.sway
  (pkgs.writeShellScriptBin "custom-weaselway-session" ''
    export XDG_CURRENT_DESKTOP=sway
    exec sway "$@"
  '')
];
```

[examples/custom-weaselway-session](examples/custom-weaselway-session) does
the same for sway without a rebuild. Its `nix-shell` shebang fetches sway when
the session starts.

To make a custom session the default, set `weaselway.session = "custom"`.

## The viewer

`ww-start-viewer` appends its arguments to the `sdl-freerdp.exe` command line,
so any FreeRDP option can be added or overridden for one run.

The session sees an ordinary keyboard. Set the layout in the desktop's own
keyboard settings, not in the viewer.

The viewer forwards touchpad gestures with three or more fingers, audio
playback and the microphone. The session sees a touchpad of its own, so the
desktop's swipe and pinch gestures work.

Two options help when investigating display problems. They can be combined.

```sh
ww-start-viewer /sdl-show-stats:2   # frame and bandwidth counters, text at 2x
ww-start-viewer /sdl-show-damage    # tint the regions updated in each frame
```

The value after `/sdl-show-stats` is the text scale and is optional.

To run a different build of the viewer, set `WEASELWAY_VIEWER`:

```sh
WEASELWAY_VIEWER=/mnt/c/Weaselway/sdl-freerdp.exe ww-start-viewer
```

## Configuration

The image ships its own NixOS configuration as a flake in `/etc/nixos`:

- `flake.nix` takes weaselway (`github:weaselway/weaselway`) as an input, and
  with it the matching nixpkgs and NixOS-WSL.
- `configuration.nix` is the system configuration, the same file the image
  was built from ([nix/image/configuration.nix](nix/image/configuration.nix)).

Edit `configuration.nix` and rebuild to change the system:

```sh
sudo -e /etc/nixos/configuration.nix
sudo nixos-rebuild switch
```

The hostname is `nixos`, so `nixos-rebuild` selects
`nixosConfigurations.nixos` without a `#name` argument.

Weaselway adds these options:

| Option | Default | Description |
|---|---|---|
| `weaselway.enable` | `false` (`true` in the image) | Enables everything: Mesa, dxgdrm, weaselwayd, audio and the scripts. |
| `weaselway.adapter` | `null` | The GPU to render on, as a substring of its name (`"nvidia"`, `"Intel"`). `null` selects the first adapter Windows lists. |
| `weaselway.session` | `"gnome"` | The session `ww-start-session` starts when none is given. |
| `weaselway.plasma.enable` | `false` | Installs Plasma for `ww-start-session plasma`. |

`ww-start-session --adapter <name>` overrides the adapter for one session
without a rebuild.

Weaselway patches KWin. Enabling Plasma therefore downloads KWin and its
dependents from weaselway.cachix.org, or compiles them if CI has not built
that combination.

Everything else is standard NixOS: add packages to
`environment.systemPackages`, set `time.timeZone` and so on. The image
deviates from the NixOS defaults in two places:

- sshd is disabled. To log in over SSH, uncomment `services.openssh.enable`,
  and set a password with `passwd` or add a key first. With mirrored WSL
  networking the distro is reachable from the LAN.
- The screen reader is not installed, because Orca's voices add about 650 MB.
  Delete the two lines marked in `configuration.nix` to install it.

## Updating

`/etc/nixos/flake.lock` pins weaselway to the commit the image was built from.
A rebuild after a configuration change therefore does not update anything. To
update, move the lock forward and rebuild:

```sh
sudo nix flake update --flake /etc/nixos
sudo nixos-rebuild switch
```

This updates weaselway together with nixpkgs, NixOS-WSL, Mesa, the kernel
module, weaselwayd and the viewer, to versions that were built and tested
together.

Some packages are not on cache.nixos.org: the patched Mesa and mutter,
gnome-shell (which links mutter), weaselwayd and the kernel tree dxgdrm is
built against. CI pushes them to
[weaselway.cachix.org](https://weaselway.cachix.org), and the image is
configured to use that cache. A configuration change that alters one of these
packages compiles it locally, which takes time and a few GB of disk space.

Restart the session after a rebuild so that it uses the new Mesa:
`ww-start-session stop`, then `ww-start-session`. If the kernel module
changed, run `wsl --shutdown` instead.

To undo an update, switch back to the previous generation:

```sh
sudo nixos-rebuild switch --rollback
```

The system distro VHD is stored on the Windows side and is not replaced by a
rebuild. Run `ww-install-system-image` again after an update. If it downloads
a new image, change the `systemDistro=` line to the one it prints and run
`wsl --shutdown`.

## Limitations

- The clipboard is shared with Windows only in GNOME sessions. Text, formatted
  text and images can be copied; files cannot.
- The scale factor of the Windows display is not passed on. Set the scale in
  the desktop's display settings.
- Audio from web browsers can crackle. Other players are not affected.
- Touchscreens are not forwarded. Touchpad gestures are.
- Compositors other than GNOME and Plasma have seen little testing. If one
  does not start on the virtual display, please open an issue.

## Troubleshooting

### The viewer window stays empty or closes immediately

Check `journalctl --user -u weaselwayd`. The message "waiting for the
compositor's first commit" means that no session is running; start one with
`ww-start-session`.

weaselwayd does not start without the shared memory region, and
`systemctl status weaselway-prep` reports why it is missing. Check that
`.wslconfig` contains the `systemDistro=` line, that you ran `wsl --shutdown`
afterwards, and that Weaselway was the first distro started.

### `ww-start-session` fails

`journalctl -u weaselway-session` contains the compositor's output.

### Everything renders in software (llvmpipe)

The Windows GPU driver did not load. The known causes are listed under
[Graphics](NIXOS.md#graphics) in NIXOS.md.

### The desktop shows no audio devices

`wpctl status` should list "Remote Desktop Audio", and
`/run/user/1000/pulse/native` should be a socket, not a symbolic link.

[Debugging](NIXOS.md#debugging) in NIXOS.md lists further checks.
