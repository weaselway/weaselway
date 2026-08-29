#!/bin/bash

set -euo pipefail

# Builds dxgdrm.ko against the running WSL kernel and puts it somewhere it will
# still be after a reboot, together with the udev rules that go with it.
#
# The obvious home for a module, /lib/modules/$(uname -r)/extra, is not one
# here: WSL mounts /usr/lib/modules/$(uname -r) itself as an overlay whose
# upper layer lives in WSL's own mount namespace, so anything installed there
# is discarded at the next `wsl --shutdown` -- and `modprobe dxgdrm` by name
# searches exactly that directory. prep-session.sh loads the module by path
# instead, out of the directory below. See repo-dxgdrm/BUILD-NOTES.md.

MODULE_DIR="/usr/local/lib/weaselway/modules/$(uname -r)"

if ! [ -d repo-dxgdrm/.git ] ; then
    git clone https://github.com/weaselway/dxgdrm.git repo-dxgdrm
fi

git -C repo-dxgdrm pull origin main

# The build needs nothing from the host but its kernel version, which the
# container shares, and writes only into the bind-mounted repo.
ubuntu/resolute/docker-env.sh -exc '
    cd repo-dxgdrm
    ./build-kernel-headers.sh
    make all
'

sudo install -D -m 0644 repo-dxgdrm/dxgdrm.ko "${MODULE_DIR}/dxgdrm.ko"

# The path is keyed on `uname -r` because a module only loads into the exact
# kernel release it was built against. After a WSL kernel update the new path
# simply does not exist yet, which prep-session.sh reports as "rebuild it" --
# rather than the old .ko being found and rejected with a vermagic error.

# Unlike the module, the rules are a one-time install: /etc/udev/rules.d is on
# the distro's own disk. Reload so that the `udevadm trigger` following a
# module load is evaluated against them and not against the rules udev started
# with.
sudo install -D -m 0644 \
    repo-dxgdrm/99-dxgdrm.rules \
    /etc/udev/rules.d/99-dxgdrm.rules

sudo udevadm control --reload
