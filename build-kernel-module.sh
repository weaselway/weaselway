#!/bin/bash

set -euo pipefail

if ! [ -d repo-dxgdrm/.git ] ; then
    git clone https://github.com/weaselway/dxgdrm.git repo-dxgdrm
fi

git -C repo-dxgdrm pull origin main

ubuntu/resolute/docker-env.sh -v /lib/modules:/lib/modules -- -exc '
    cd repo-dxgdrm
    ./build-kernel-headers.sh
    make install
'

echo "You can now load the kernel module:"
echo "  sudo modprobe dxgdrm"
echo "  sudo udevadm trigger --subsystem-match=drm"
echo "  sudo udevadm settle"
