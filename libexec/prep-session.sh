#!/usr/bin/env bash

# Root-side preparation for the session, run once per boot out of
# weaselway-prep.service.

set -xeuo pipefail

# dxgdrm provides the DRM nodes: the render node d3d12 clients need, and the
# KMS node with the virtual display the compositor drives. Nothing loads the
# module at boot. Read /proc/modules directly rather than piping lsmod, so
# pipefail has nothing to trip over. The udevadm calls are what turn the module
# into /dev/dri/card0 and renderD128 with their permissions.
#
# The module is loaded by path, not by name: /lib/modules is an overlay WSL
# mounts itself, and the module lives in the Nix store. A path with a slash in
# it makes modprobe load that file. That skips modules.dep, which costs
# nothing here -- dxgdrm links only against DRM core, and CONFIG_DRM=y.
#
# The path carries the kernel release, so after a WSL kernel update the module
# built for the old one is not silently picked up and rejected -- it is just not
# there, and the message says so. Only a warning: the rest of this script has
# nothing to do with the module, and ww-start-session.sh refuses to start without
# the KMS node.
#
# First, and regardless of the system distro below: the render node is needed
# by anything using the d3d12 driver, not only by the session.
: "${DXGDRM_KO:?set by weaselway-prep.service}"

if ! grep -q '^dxgdrm ' /proc/modules; then
    if [ -e "${DXGDRM_KO}" ]; then
        modprobe "${DXGDRM_KO}"
        udevadm trigger --subsystem-match=drm
        udevadm settle
    else
        echo "error: ${DXGDRM_KO} missing -- the dxgdrm flake has no module for this WSL kernel yet" >&2
    fi
fi

# Input reaches the compositor as ordinary evdev devices that weaselwayd
# creates through uinput. Both are modules in the WSL kernel
# (CONFIG_INPUT_EVDEV=m, CONFIG_INPUT_UINPUT=m), and nothing loads them. They
# come from WSL's own /lib/modules, by path for the same reason as dxgdrm above
# and because NixOS' modprobe does not search there. Neither depends on another
# module. A warning only: the session works without them, just without input.
for MODULE in evdev uinput; do
    if grep -q "^${MODULE} " /proc/modules; then
        continue
    fi
    MODULE_KO="$(find "/lib/modules/$(uname -r)" "/usr/lib/modules/$(uname -r)" \
        -name "${MODULE}.ko*" 2>/dev/null | head -n 1 || true)"
    if [ -n "${MODULE_KO}" ]; then
        modprobe "${MODULE_KO}" || echo "warning: loading ${MODULE_KO} failed" >&2
    else
        echo "warning: no ${MODULE}.ko under /lib/modules/$(uname -r) -- no input from the viewer" >&2
    fi
done

# Take /tmp/.X11-unix back from WSL, so the socket the compositor creates there
# (it binds the socket itself and hands Xwayland the fd) is not landing in
# something read-only or unwritable.
#
# WSL generates wslg.service for this path, ordered After=tmp.mount, whose whole
# body is:
#
#   mount -o bind,ro,X-mount.mkdir -t none /mnt/wslg/.X11-unix /tmp/.X11-unix
#
# Two things there matter. The mount is read-only, and X-mount.mkdir creates the
# mountpoint first -- as root, mode 0755. So undoing the mount is not enough:
# the directory it made stays behind, owned by root and writable by nobody else,
# and a compositor running as the user cannot create its socket in it.
#
# Hence rm -rf rather than umount alone, and a fresh 1777 directory after it --
# sticky and world-writable, which is what /usr/lib/tmpfiles.d/x11.conf asks for
# on any normal desktop and what makes the owner question moot.
#
# The unit is ordered After=wslg.service so this runs once WSL has had its turn;
# wslg.service carries ConditionPathExists=!/tmp/.X11-unix/X0 and so does not
# come back and redo it afterwards.
if [ -L /tmp/.X11-unix ]; then
    rm -f /tmp/.X11-unix
fi

# Stacked mounts are possible here, and each umount pops one. Bounded rather
# than `while true` so a mount that will not go away fails the unit below
# instead of spinning.
for _ in 1 2 3 4 5; do
    mountpoint -q /tmp/.X11-unix || break
    umount /tmp/.X11-unix || break
done

rm -rf /tmp/.X11-unix
mkdir -m 1777 /tmp/.X11-unix

# Prove the takeover worked, because the failure is otherwise silent until much
# later: Xwayland cannot bind its socket, and X11 applications do not start.
# Nothing else checks this.
#
# Note this cannot be a `touch` probe: that runs as root, which can write into a
# root-owned 0755 directory perfectly well, and so passes in exactly the broken
# case it is meant to catch. Check the state itself instead.
#
# Nor can it be `mountpoint`: that reads /proc/self/mountinfo, and WSL's bind is
# still listed there long after it stopped being reachable. tmp.mount covers
# /tmp with a fresh tmpfs *after* wslg.service mounted onto the old one, which
# leaves the bind shadowed -- present in the table, mounted over, affecting
# nothing. `mountpoint` calls that a mountpoint and `umount` calls it "not
# mounted", and only the latter is telling the truth.
#
# Comparing device numbers asks the question that actually matters: if our
# directory is on the same filesystem as /tmp, then nothing is mounted over it.
if [ "$(stat -c %d /tmp/.X11-unix)" != "$(stat -c %d /tmp)" ]; then
    echo "error: something is still mounted over /tmp/.X11-unix" >&2
    exit 1
fi

MODE="$(stat -c %a /tmp/.X11-unix)"
if [ "${MODE}" != "1777" ]; then
    echo "error: /tmp/.X11-unix has mode ${MODE}, expected 1777" >&2
    exit 1
fi

# The shared-memory share gfxredir allocates its buffers on. WSL creates it,
# as the virtiofs tag "wslg", only when GUI apps are on *and* a system distro is
# configured -- which is why the weaselway system image has to be in
# .wslconfig at all. Nothing in the system distro mounts it any more; the share
# is VM-wide, and this is the only place that uses it. The mount point is
# weaselwayd's default (--shm).
#
# Last, and fatal: without it weaselwayd has nowhere to put the frames, and
# this unit failing is the place that says why.
SHARED_MEMORY_MOUNT_POINT=/mnt/wslg-shared-memory

if ! mountpoint -q "${SHARED_MEMORY_MOUNT_POINT}"; then
    mkdir -p "${SHARED_MEMORY_MOUNT_POINT}"
    if ! mount -t virtiofs -o dax wslg "${SHARED_MEMORY_MOUNT_POINT}"; then
        echo "error: cannot mount the WSLg shared-memory share -- is systemDistro= in .wslconfig set to the weaselway system image (see the README), followed by wsl --shutdown?" >&2
        exit 1
    fi
    chmod 0777 "${SHARED_MEMORY_MOUNT_POINT}"
fi
