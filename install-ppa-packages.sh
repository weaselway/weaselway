#!/usr/bin/env bash

set -exu -o pipefail

# Alternative to building mesa and mutter locally -- pulls the same patched
# packages from a PPA instead. The pin file gives that PPA priority over
# anything else that also ships a `mutter` or `mesa`, including a later distro
# update, so the patched build does not get silently reverted from underneath
# the session.

sudo tee /etc/apt/preferences.d/weaselway > /dev/null <<'EOF'
Package: *
Pin: release o=LP-PPA-oliver-bestmann-weaselway
Pin-Priority: 1001
EOF

sudo add-apt-repository -y ppa:oliver-bestmann/weaselway
sudo apt update
sudo apt upgrade -y

# The pin only helps if the PPA actually served the packages. Check, rather
# than find out from a session running the stock mutter.
for pkg in mutter libegl-mesa0; do
    version="$(dpkg-query -W -f='${Version}' "${pkg}")"
    case "${version}" in
        *weasel*) ;;
        *)
            echo "error: ${pkg} ${version} is not the weaselway build from the PPA" >&2
            exit 1
            ;;
    esac
done
