#!/usr/bin/env bash

# Install Firefox and Chromium as real .debs, from the xtradeb PPA.
#
# Ubuntu ships both as snaps -- the packages of those names in the archive are
# transitional and pull in the snap. That does not work here: a browser snap
# bundles its own mesa, which is not the patched one, so it either fails to
# accelerate or breaks outright. xtradeb builds them as ordinary packages
# against the system libraries, and so against our mesa.
#
# The pin gives that PPA priority over the archive for everything it carries,
# which is what keeps `apt upgrade` from putting the transitional packages back.

set -xeuo pipefail

sudo tee /etc/apt/preferences.d/weaselway-xtradeb > /dev/null <<'EOF'
Package: *
Pin: release o=LP-PPA-xtradeb-apps
Pin-Priority: 1001
EOF

sudo add-apt-repository -y ppa:xtradeb/apps
sudo apt update
sudo apt install -y firefox chromium

# Which build actually landed. Both should name xtradeb rather than the archive,
# and neither should be the transitional package.
apt policy firefox chromium
