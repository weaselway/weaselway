# Patched mesa / mutter for Ubuntu 26.04 (resolute)

Rebuilds Ubuntu's own `mesa` and `mutter` source packages with the WSL patches
from the [weaselway](https://github.com/weaselway) forks on top, and drops the
resulting `.deb`s into `packages/`.

Nothing is built against a hand-rolled tree: the Ubuntu source package is the
base, and the fork's commits are exported as patches and pushed onto it with
quilt. That keeps the Debian packaging, dependencies and maintainer scripts
intact, so the result installs over the distro package cleanly.

## Quick start

```sh
./build-packages-local.sh
sudo apt install -y ./packages/*/*.deb
```

`build-packages-local.sh` runs `build-mesa.sh` and `build-mutter.sh` inside the
container from `docker-env.sh`, so the only thing needed on the host is docker.
Expect it to take a while.

## docker-env.sh

Builds the image described by the `Dockerfile` next to it and runs a command in
it. The image is Ubuntu 26.04 with `deb-src` enabled (needed for `apt source`
and `apt build-dep`).

```sh
./docker-env.sh                  # interactive shell in the build environment
./docker-env.sh ./build-mesa.sh  # run one thing in it
./docker-env.sh -c 'set -ex; …'  # the entrypoint is "env bash", so this is its argv
```

Two details worth knowing:

- The image contains a `builder` account created with the calling user's UID and
  GID, and the container runs as that UID. Build artifacts written to the mounted
  directory therefore end up owned by you and not by root. That is also why the
  image tag carries the UID -- a different user on the same machine gets their
  own image.
- This directory is mounted at `/work` and is the working directory, so every
  path below is relative to the checkout and the results survive the container
  exiting.

`sudo` inside the container is passwordless, which is what lets the build
scripts install their own build dependencies.

## What a build does

`build-mesa.sh` and `build-mutter.sh` are just variable blocks; the work is in
`_build.sh`, which they source for its `build-packages` function. In order:

1. Refuses to run unless `/etc/lsb-release` says `resolute` -- the packaging is
   pinned to one Ubuntu release.
2. Clones `https://github.com/weaselway/$REPO.git` into `$PACKAGE/repo` and
   switches to `$BRANCH`. The clone is guarded by a `repo/ok` marker, so it is
   only ever done once; delete `$PACKAGE/repo` to start over.
3. Exports `$UPSTREAMTAG..HEAD` as a patch series into `$PACKAGE/patches` with
   `git format-patch`. The fork's branch being a linear series of commits on top
   of the upstream release tag is what makes this work.
4. Unpacks the Ubuntu source package with `apt source $PACKAGE`.
5. Applies `debian-$PACKAGE.patch` from this directory if that file exists, for
   changes to the Debian packaging itself rather than to the source. Only
   `debian-mutter.patch` exists today; a package that needs no packaging changes
   simply has no such file.
6. Imports the exported patches into `debian/patches` with `quilt import` and
   applies them with `quilt push -a`.
7. Adds a changelog entry and builds (see below).
8. Moves everything produced into `packages/$PACKAGE/`, which is wiped at the
   start of each run.

The per-package variables:

| Variable | Meaning |
| --- | --- |
| `REPO` | repository name under `github.com/weaselway` |
| `PACKAGE` | Ubuntu **source** package name, used for `apt source` and `apt build-dep`, and as the work and output directory name |
| `PACKAGE_SOURCE` | directory `apt source` unpacks to, i.e. `<package>-<upstream version>` |
| `BRANCH` | branch to build in the fork |
| `UPSTREAMTAG` | tag the branch is based on; the patch series is `$UPSTREAMTAG..HEAD` |
| `EXTRA_DEPENDENCIES` | extra apt packages on top of `build-dep` |
| `SOURCEONLY` | see below; defaults to `false` and is overridable from the environment |

Adding a package means copying one of the two scripts, changing those variables,
and adding it to `build-packages-local.sh`. Note that `PACKAGE_SOURCE`,
`UPSTREAMTAG` and `BRANCH` all encode the version, so bumping a package means
editing three lines.

`mesa/`, `mutter/` and `packages/` are gitignored -- they are entirely
reproducible from a run.

## Source-only builds, for the PPA

The default is a local binary build: `dch --local "+weasel0"` for the version
suffix, then `dpkg-buildpackage -us -uc -b`. Unsigned, binaries only, ready to
`apt install`.

Setting `SOURCEONLY=true` switches to a **signed source package** instead --
`dch --distribution resolute --release` followed by `dpkg-buildpackage -S -sa -d`
-- which is the form Launchpad accepts for a PPA upload:

```sh
SOURCEONLY=true ./build-mesa.sh
```

Because it has to sign, this one needs a GPG key present, and it is signing with
a specific key id (`4A65FFE4EEFE2E93`, hardcoded in both `_build.sh` and
`setup-gpg.sh`). To make that key available:

1. Copy the secret key into `./gnupg` in this directory.
2. Run `./setup-gpg.sh` **inside the build environment**
   (`./docker-env.sh` first, or `./docker-env.sh ./setup-gpg.sh`). It symlinks
   `./gnupg` to `~/.gnupg` in the container, writes a `gpg-agent.conf` using
   `pinentry-curses`, and then signs a throwaway message twice so the agent
   caches the passphrase for the hour that follows. That is why it is two test
   signatures and not one -- the second confirms the cache actually took, so the
   real build will not stop for a prompt halfway through.
3. Run the build with `SOURCEONLY=true` in the same container session.

The passphrase cache is `default-cache-ttl 3600`, so a long build may still
outlive it; re-run `setup-gpg.sh` if signing starts prompting again.

`./gnupg` is **not** covered by `.gitignore`. Keep it out of any commit.
