#!/usr/bin/env bash

# Root-side preparation for the GNOME session, run once per boot out of
# weaselway-prep.service. All of this used to sit inline in start-gnome-shell.sh
# behind a sudo; it moved here because the session itself is now a set of units
# under the user manager, which has no business calling sudo.

set -xeuo pipefail

# The d3d12 driver needs the render node that dxgdrm provides, and nothing loads
# the module at boot. Read /proc/modules directly rather than piping lsmod, so
# pipefail has nothing to trip over. The udevadm calls are what turn the module
# into /dev/dri/renderD128, which is what the shell drop-in asserts on.
#
# The module is loaded by path, from where install-kernel-module.sh put it, not by
# name: /lib/modules is an overlay WSL mounts itself and empties on every
# `wsl --shutdown`, so the directory `modprobe dxgdrm` would search is exactly
# the one that never has it. A path with a slash in it makes modprobe load that
# file instead. That skips modules.dep, which costs nothing here -- dxgdrm links
# only against DRM core, and CONFIG_DRM=y.
#
# The path carries the kernel release, so after a WSL kernel update the module
# built for the old one is not silently picked up and rejected -- it is just not
# there, and the message says what to do. Only a warning: the rest of this
# script has nothing to do with the module, and a missing render node already
# stops the session at org.gnome.Shell's AssertPathExists, right below this
# line in the journal.
#
# First, and regardless of the system distro below: the render node is needed
# by anything using the d3d12 driver, not only by the session.
# DXGDRM_KO in the environment overrides the path; the NixOS image points it
# into the store.
DXGDRM_KO="${DXGDRM_KO:-/usr/local/lib/weaselway/modules/$(uname -r)/dxgdrm.ko}"

if ! grep -q '^dxgdrm ' /proc/modules; then
    if [ -e "${DXGDRM_KO}" ]; then
        modprobe "${DXGDRM_KO}"
        udevadm trigger --subsystem-match=drm
        udevadm settle
    else
        echo "error: ${DXGDRM_KO} missing -- run install-kernel-module.sh" >&2
    fi
fi

# Take /tmp/.X11-unix back from WSL, so the socket mutter creates there (it
# binds the socket itself and hands Xwayland the fd with -listenfd) is not
# landing in something read-only or unwritable.
#
# WSL generates wslg.service for this path, ordered After=tmp.mount, whose whole
# body is:
#
#   mount -o bind,ro,X-mount.mkdir -t none /mnt/wslg/.X11-unix /tmp/.X11-unix
#
# Two things there matter. The mount is read-only, and X-mount.mkdir creates the
# mountpoint first -- as root, mode 0755. So undoing the mount is not enough:
# the directory it made stays behind, owned by root and writable by nobody else,
# and mutter running as the user cannot create its socket in it.
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
# later: Xwayland cannot bind its socket, and MUTTER_X11_MANDATORY=1 takes the
# whole session down with it. Nothing else checks this -- the shell drop-in
# asserts on the render node and the shared-memory mount, not on this path.
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
# hardcoded rather than passed in: the shell drop-in has to name it too, and a
# value in two unit files is easier to keep honest than one threaded through
# the environment.
#
# Last, and fatal: without it mutter can only send its error frame, and this
# unit failing is the other place that should say so.
SHARED_MEMORY_MOUNT_POINT=/mnt/wslg-shared-memory

if ! mountpoint -q "${SHARED_MEMORY_MOUNT_POINT}"; then
    mkdir -p "${SHARED_MEMORY_MOUNT_POINT}"
    if ! mount -t virtiofs -o dax wslg "${SHARED_MEMORY_MOUNT_POINT}"; then
        echo "error: cannot mount the WSLg shared-memory share -- is systemDistro= in .wslconfig set to the weaselway system image (README step 2), followed by wsl --shutdown?" >&2
        exit 1
    fi
    chmod 0777 "${SHARED_MEMORY_MOUNT_POINT}"
fi
