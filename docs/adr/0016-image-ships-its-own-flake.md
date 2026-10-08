# 0016. The image ships its configuration as a flake locked to its own commit

Status: Accepted

## Context

Users change the system by editing its NixOS configuration. Without a lock in
`/etc/nixos`, the first `nixos-rebuild` locked whatever weaselway `main` was by
then, and a one-line change could rebuild Mesa, mutter and gnome-shell.

## Decision

- The image ships `/etc/nixos` with a `flake.nix` that takes
  `github:weaselway/weaselway` as input, and the `configuration.nix` the image
  was built from (`nix/image-gnome/` or `nix/image-plasma/`). The hostname is
  `nixos`, so `nixos-rebuild` needs no `#name`.
- `nixosModules.image` (`nix/image-lock.nix`) writes `/etc/nixos/flake.lock` on
  activation if there is none (`505935b`). It pins weaselway to `self.rev` and
  `self.narHash`, with weaselway's own lock nested under it, the same file
  `nix flake lock` writes for that commit.
- The lock is written on activation, not put in the tarball:
  `wsl.tarball.configPath` goes through `lib.cleanSource`, where a generated
  directory would be an import-from-derivation.
- There are two image directories with identical `flake.nix` files, because
  `wsl.tarball.configPath` takes a whole directory. The desktop is selected in
  `configuration.nix`.

## Consequences

- A rebuild after a configuration change updates nothing. Updating is an
  explicit `nix flake update --flake /etc/nixos`.
- What the user rebuilds is what CI built and cached for that commit.
- A build from a dirty tree has no `self.rev` and ships without a lock.
- The lock depends on equal hashes for a git checkout and a GitHub tarball
  (see 0015).
