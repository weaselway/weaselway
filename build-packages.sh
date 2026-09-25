#!/usr/bin/env bash

set -eu -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# docker-env.sh mounts the current directory as /work inside the container, so
# the build has to be started from ubuntu/resolute and not from here.
cd "${SCRIPT_DIR}/ubuntu/resolute"
./build-packages-local.sh

cat <<EOF

The packages are in ubuntu/resolute/packages. Install them, and hold them so
archive updates don't replace them, with:
  sudo apt install -y --allow-downgrades ${SCRIPT_DIR}/ubuntu/resolute/packages/*/*.deb
  for deb in ${SCRIPT_DIR}/ubuntu/resolute/packages/*/*.deb; do dpkg-deb -f "\$deb" Package; done | sort -u | xargs sudo apt-mark hold

EOF
