# 0027. Releases are immutable and carry both images

Status: Accepted

## Context

Users import a release's image and pin `/etc/nixos` to its commit (see 0016).
A release whose assets change after publication, or that has only one of the
two images, cannot be trusted. Tags like `v1.1.0-rc5` were published as full
releases, and the newest of them became what `releases/latest` pointed at
(`2d07d84`).

## Decision

- `release-image.yml` runs on `v*` tags. It builds the two images in separate
  jobs and creates the release in a third, so a release is never published
  with one image.
- Each image gets a `.sha256` file next to it.
- `gh release create` fails if a release for the tag already exists, and the
  workflow fails with it rather than replacing a published asset.
- A tag with a suffix is published as a pre-release.
- The images of a build run are kept as artifacts for one day.

## Consequences

- Fixing a release means a new tag.
- A failed release job (for example an image over 2 GiB, see 0019) leaves the
  tag without a release, and the tag has to be replaced by a new one.
