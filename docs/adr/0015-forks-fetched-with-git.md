# 0015. Forks with eol rules are fetched with git, not as GitHub tarballs

Status: Accepted

## Context

Mesa's and FreeRDP's `.gitattributes` have `eol=` rules. Nix 2.34 and 2.35
disagree on whether a GitHub tarball has them applied, so the locked NAR hash
of a `github:` input failed in CI with "NAR hash mismatch" (`683046a`,
`3aeacec`).

## Decision

- `freerdp` is a `git+https://…?ref=…&shallow=1` input.
- The mesa fork is fetched with `fetchFromGitHub` in `nix/overlay.nix`, one
  rev and hash per branch. That is a fixed-output derivation that unpacks
  GitHub's tarball itself, so the Nix version does not change the result.
  The tarball differs from a checkout only in the line endings of `*.csv`
  files, which the build does not read.
- weaselway itself has no `.gitattributes`.

## Consequences

- A mesa update is a new rev and hash in `nix/overlay.nix`, not
  `nix flake update`.

- The same tree, and the same store paths, with every Nix version.
- The image's `/etc/nixos/flake.lock` pins `github:weaselway/weaselway` and
  relies on the `narHash` of a git checkout being equal to that of GitHub's
  tarball (see 0016). Adding `eol=` or `export-ignore` rules to this
  repository would break that.
