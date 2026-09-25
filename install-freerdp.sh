#!/usr/bin/env bash

set -exu -o pipefail

VERSION=v1.0.4
# Of the release zip. Update together with VERSION; empty skips the check
# (with a warning) until the release exists to take it from.
SHA256=
URL=https://github.com/weaselway/freerdp/releases/download/${VERSION}/freerdp-${VERSION}.zip

mkdir -p /mnt/c/Weaselway

# Extract from a temporary copy so the zip itself does not end up in the
# Weaselway directory.
ZIP=$(mktemp --suffix=.zip)
trap 'rm -f ${ZIP}' EXIT

curl -fL --retry 3 "$URL" -o "${ZIP}"

if [ -n "${SHA256}" ]; then
    echo "${SHA256}  ${ZIP}" | sha256sum -c -
else
    echo "warning: no SHA256 pinned for freerdp ${VERSION}, not verifying" >&2
fi
unzip -o ${ZIP} -d /mnt/c/Weaselway
