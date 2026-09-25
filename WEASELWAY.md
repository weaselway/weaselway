# weaselway with Nix

This repo is shell/PowerShell glue that runs inside the target distro, so
there is nothing to compile. The Nix side is only linting:

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
| dxgdrm | `nix develop -c make` (x86_64 cross build; not loadable, see its WEASELWAY.md) |

The meson flags for mesa and mutter match [dev/build-mesa.sh](dev/build-mesa.sh)
and [dev/build-mutter.sh](dev/build-mutter.sh). If you change flags in one
place, change them in the other.

The Ubuntu packages ([ubuntu/resolute](ubuntu/resolute)) build the
`mesa-26.0.8-wsl` and `mutter 50.1-wslg` branches, not `main`.
