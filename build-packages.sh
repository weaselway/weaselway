#!/usr/bin/env bash

set -eu -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# docker-env.sh mounts the current directory as /work inside the container, so
# the build has to be started from ubuntu/resolute and not from here.
cd "${SCRIPT_DIR}/ubuntu/resolute"
./build-packages-local.sh

cat <<EOF

The packages are in ubuntu/resolute/packages. Install them with:
  sudo apt install -y ${SCRIPT_DIR}/ubuntu/resolute/packages/*/*.deb

EOF
