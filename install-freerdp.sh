#!/usr/bin/env bash

set -exu -o pipefail

VERSION=v1.0.1
URL=https://github.com/weaselway/freerdp/releases/download/${VERSION}/freerdp-${VERSION}.zip

mkdir -p /mnt/c/Weaselway

# Extract from a temporary copy so the zip itself does not end up in the
# Weaselway directory.
ZIP=$(mktemp --suffix=.zip)
trap 'rm -f ${ZIP}' EXIT

curl -L $URL -o ${ZIP}
unzip -o ${ZIP} -d /mnt/c/Weaselway
