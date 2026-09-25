# weaselway with Nix

This repo is shell/PowerShell glue that runs inside the target distro. The
Nix side lints it, and builds a NixOS-WSL image of the whole setup (see
"NixOS-WSL image" below). Linting:

```sh
nix develop -c shellcheck --shell=bash $(git ls-files '*.sh')   # all findings
nix flake check                                                  # errors only, fails the check
```

`--shell=bash` is needed because the sourced `ubuntu/resolute/_build.sh` has
no shebang. The flake check runs at `--severity=error`; the warnings are
still there and haven't been triaged.

## Building the components with Nix

Each component repo has its own `flake.nix` (nixpkgs `nixos-26.05`) and a
`WEASELWAY.md`. Nothing installs; these are compile checks.

| Repo | Command |
|---|---|
| mesa | `./weaselway-build.sh` |
| mutter | `./weaselway-build.sh` |
| wslg | `./weaselway-build.sh` (WSLGd + rdpapplist) |
| freerdp | `nix develop -c ./build-freerdp.sh` (Windows cross build) |
| dxgdrm | `nix develop -c make` (built with the WSL kernel's own gcc, see its WEASELWAY.md) |

The meson flags for mesa and mutter match [dev/build-mesa.sh](dev/build-mesa.sh)
and [dev/build-mutter.sh](dev/build-mutter.sh). If you change flags in one
place, change them in the other.

The Ubuntu packages ([ubuntu/resolute](ubuntu/resolute)) build the
`mesa-26.0.8-wsl` and `mutter 50.1-wslg` branches, not `main`.

## NixOS-WSL image

Instead of Ubuntu plus the `install-*.sh` scripts, the user distro can be a
NixOS-WSL image with everything already in it. [flake.nix](flake.nix) builds
one on top of [NixOS-WSL](https://github.com/nix-community/NixOS-WSL):

- [nix/overlay.nix](nix/overlay.nix): `weaselway-mesa` (nixpkgs' mesa built
  from `weaselway/mesa` `mesa-26.2.1-wsl`, d3d12/dzn/llvmpipe/softpipe/zink
  only), `mutter` (from `weaselway/mutter` `50.4-wslg`, with `-Drdp=enabled`),
  and `weaselway-scripts` (`start-gnome-shell`, `start-viewer`,
  `install-freerdp`, `install-system-image` on PATH).
- [nix/module.nix](nix/module.nix): `nixosModules.weaselway`, the NixOS version
  of what `install-units.sh` and `install-audio.sh` set up. The unit drop-in,
  `environment.d` file, prep script and PipeWire config are the same files
  from this repo. The dxgdrm module comes from the `dxgdrm` flake, built with
  the WSL kernel's own gcc so the CRCs match, and is loaded straight out of the
  store. Options: `weaselway.adapter` (like `install-units.sh --adapter`) and
  `weaselway.session` (default `gnome`; NixOS has no `ubuntu` session).
- `nixosConfigurations.wsl`: NixOS-WSL with the module on, user `nixos`.

Mesa goes in through `hardware.graphics.package` rather than replacing
`pkgs.mesa`, so only the drivers get rebuilt. mutter does replace `pkgs.mutter`,
so gnome-shell gets rebuilt against it.

### Building

```sh
sudo nix run .#tarballBuilder       # writes nixos.wsl
```

The inputs point at the GitHub branches. To build from local checkouts
(unpushed commits included):

```sh
sudo nix run .#tarballBuilder \
  --override-input dxgdrm path:../dxgdrm \
  --override-input mesa-src 'git+file:../mesa?ref=mesa-26.2.1-wsl' \
  --override-input mutter-src 'git+file:../mutter?ref=50.4-wslg'
```

The image is x86_64. On an aarch64 machine, use an x86_64 box as a remote
builder, not binfmt emulation: on Apple silicon (16K pages), qemu-user crashes
loading `libavcodec`, which breaks mutter's introspection step, and gcc
segfaults partway through mesa. Evaluate locally and build in the remote
store:

```sh
nix build --eval-store auto --store ssh-ng://<user>@<x86_64-host> \
  .#nixosConfigurations.wsl.config.system.build.toplevel .#tarballBuilder
# then, on the x86_64 host:
sudo /nix/store/<hash>-nixos-wsl-tarball-builder/bin/nixos-wsl-tarball-builder
```

Or add the machine to `nix.buildMachines` and build normally. Everything
except dxgdrm's kernel tree, mesa, mutter and whatever links mutter
(gnome-shell and friends) comes from the binary cache.

### Installing

From Windows:

```powershell
wsl --install --from-file nixos.wsl --name Gnome
wsl -d Gnome
```

The same first-distro rule as in the README applies: WSL only wires up
`/run/user/1000/` for the first distro started after `wsl --shutdown`.

The Windows side is still needed. Run these from inside the distro; both are
on PATH:

```sh
install-freerdp          # C:\Weaselway\sdl-freerdp.exe
install-system-image     # C:\Weaselway\system_x64-*.vhd, prints the .wslconfig line
```

Add the `systemDistro=` line to `.wslconfig` and `wsl --shutdown`, as in step 2
of the README.

### Running

```sh
start-gnome-shell        # gnome-session@gnome.target
start-viewer
```

The "Checking each step" list in the README still applies, except the module
path: here it is under `/nix/store/*-dxgdrm-all/lib/modules/$(uname -r)/`, and
`systemctl status weaselway-prep` names the one it tried.

To change the configuration, put the flake in `/etc/nixos`, import
`nixosModules.weaselway` next to NixOS-WSL, and `nixos-rebuild switch` as
usual.
