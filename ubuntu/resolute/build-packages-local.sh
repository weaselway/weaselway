#!/bin/bash

set -ex

./docker-env.sh -- -c '
    set -ex
    ./build-mesa.sh
    ./build-mutter.sh
'

echo "Use apt to install packages now:"
echo "  apt install -y ./packages/*/*.deb"
