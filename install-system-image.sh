#!/usr/bin/env bash

set -exu -o pipefail

VERSION=v1.0.79-2
URL=https://github.com/weaselway/wslg/releases/download/${VERSION}/system_x64-${VERSION}.vhd.gz

mkdir -p /mnt/c/Weaselway

curl -L $URL -o /mnt/c/Weaselway/system_x64-${VERSION}.vhd.gz
gzip -d /mnt/c/Weaselway/system_x64-${VERSION}.vhd.gz

cat <<EOF

Ensure that in your Windows home directory the
file .wslconfig contains the following lines
(if it already has a [wsl2] section, put the
systemDistro line under that one)

[wsl2]
systemDistro=C:\\\\Weaselway\\\\system_x64-${VERSION}.vhd

Then run 'wsl --shutdown' for it to take effect.

EOF
