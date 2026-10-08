# 0015. Forks with eol rules are fetched with git, not as GitHub tarballs

Status: Accepted

## Context

Mesa's and FreeRDP's `.gitattributes` have `eol=` rules. Nix 2.34 and 2.35
disagree on whether a GitHub tarball has them applied, so the locked NAR hash
of a `github:` input failed in CI with "NAR hash mismatch" (`683046a`,
`3aeacec`).

## Decision

- `mesa-src` and `freerdp` are `git+https://…?ref=…&shallow=1` inputs.
- weaselway itself has no `.gitattributes`.

## Consequences

- The same tree, and the same store paths, with every Nix version.
- The image's `/etc/nixos/flake.lock` pins `github:weaselway/weaselway` and
  relies on the `narHash` of a git checkout being equal to that of GitHub's
  tarball (see 0016). Adding `eol=` or `export-ignore` rules to this
  repository would break that.
