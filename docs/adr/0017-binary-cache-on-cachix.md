# 0017. CI and installed systems share a binary cache on Cachix

Status: Accepted

## Context

Several packages are not on cache.nixos.org: the kernel tree dxgdrm is built
against, Mesa, mutter, gnome-shell (relinked against mutter), KWin and
weaselwayd. CI rebuilt them on every run, which took about 45 minutes, and a
user's `nixos-rebuild` compiled them too.

## Decision

- Both workflows push every path they build to weaselway.cachix.org with
  `cachix/cachix-action` (`fc4cc75`). The token is the `CACHIX_AUTH_TOKEN`
  secret, passed to the composite action as an input, because composite
  actions cannot read secrets.
- The module configures the same cache as a substituter, with its key, so
  `nixos-rebuild` on an installed system downloads these paths.
- Pull requests from forks get no secrets and only read from the cache.

## Consequences

- With a warm cache CI compiles only the changed packages, and users download
  what CI built for their commit.
- A configuration change that alters one of these packages still compiles it
  locally, which takes time and a few GB of disk space.
- Installed systems trust a third-party cache, and whoever holds the token can
  push to it.
- The free tier has limited storage and evicts the least recently used paths
  first. The kernel tree is the largest item.
- An installation that predates the setting compiles everything on its first
  rebuild unless the substituter is passed on the command line.
