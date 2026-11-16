# 0035. One Python installer for other distros, patching the distro's packages

Status: Accepted

## Context

`ubuntu/install.sh` set up GNOME on Ubuntu, and `arch/install.sh` (#10) set up
Plasma on Arch Linux and CachyOS. About two thirds of the two Bash scripts
were the same: the Mesa build, weaselwayd, the releases, the units and the
environment. Each supported one desktop, Ubuntu's mutter was unpatched, and
features such as the Flatpak GL extension existed on one distro only.

## Decision

- `distro/install.py` replaces both scripts. It is one file, standard library
  only, so it runs with the distro's `python3` and nothing else. It is typed
  and checked with `mypy --strict`; the version logic has doctests.
- A `Distro` class per package manager (`Ubuntu`, `Arch`) installs packages
  and rebuilds a package with patches. The rest is shared.
- Every desktop (GNOME, Plasma, Hyprland) and every option (Flatpak,
  Bluetooth) works on every distro. Options not given on the command line
  are asked for; without a terminal the script stops instead.
- The compositor packages get the patches from `nix/` that the overlay
  applies, picked by upstream version in `BACKPORTS`. A version without a
  backport stops the script. Ubuntu builds the source package with a
  `+weaselway1` version, Arch builds its PKGBUILD with `.1` appended to
  `pkgrel` and patches the tree between `makepkg --nobuild` and
  `--noextract`, so Arch's own `prepare()` stays untouched. makepkg has no
  directory for user patches.
- FreeRDP is rebuilt only where it links FFmpeg (Arch), for the PCM fix.
  The drdynvc patch from #10 is gone: weaselwayd has not called
  `drdynvc_server_start` since `187b0ea`.
- The Bluetooth modules need wsl-kernel's toolchain and prepared trees, which
  a distro does not have. Releases attach them per kernel
  (`wsl-bluetooth-<kernel>.tar.gz`, built from `nix/bluetooth-modules.nix`),
  and the script downloads them from the latest release.
- `--quick-build` compiles without optimization for CI.

## Consequences

- A distro update of a patched package replaces it with the unpatched one
  until the script runs again.
- New compositor releases need a backport in `nix/` and an entry in
  `BACKPORTS`, as in the overlay.
- `--bluetooth` only works with a release that has the tarballs, the first
  one after this change.
- Releases carry the Bluetooth tarballs besides the two images (0027).
