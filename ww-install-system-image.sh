#!/usr/bin/env bash

set -exu -o pipefail

VERSION=v1.0.79-4
# Of the .vhd.gz release asset. Update together with VERSION.
SHA256=6cdeed59cec8752d474560b64637c2e26266966bf109f9e839efc0213300ce92
URL=https://github.com/weaselway/wslg/releases/download/${VERSION}/system_x64-${VERSION}.vhd.gz

DIR=/mnt/c/Weaselway
VHD="${DIR}/system_x64-${VERSION}.vhd"

mkdir -p "${DIR}"

# Re-runnable: gzip refuses to overwrite an existing .vhd, and there is no
# point downloading it again. Unpack into a temporary name and rename at the
# end, so an interrupted run never leaves a truncated image behind that the
# check above would take for a finished one.
if [ -e "${VHD}" ]; then
    echo "${VHD} already present"
else
    GZ="$(mktemp "${DIR}/.system_x64-${VERSION}.XXXXXX.vhd.gz")"
    trap 'rm -f "${GZ}" "${GZ%.gz}"' EXIT

    curl -fL --retry 3 "${URL}" -o "${GZ}"
    echo "${SHA256}  ${GZ}" | sha256sum -c -
    gzip -df "${GZ}"
    mv "${GZ%.gz}" "${VHD}"
fi

cat <<EOF

Ensure that in your Windows home directory the
file .wslconfig contains the following lines
(if it already has a [wsl2] section, put the
systemDistro line under that one)

[wsl2]
systemDistro=C:\\\\Weaselway\\\\system_x64-${VERSION}.vhd

Then run 'wsl --shutdown' for it to take effect.

EOF
