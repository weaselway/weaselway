#!/usr/bin/env bash

# Root-side preparation for the GNOME session, run once per boot out of
# weaselway-prep.service. All of this used to sit inline in start-gnome-shell.sh
# behind a sudo; it moved here because the session itself is now a set of units
# under the user manager, which has no business calling sudo.

set -xeuo pipefail

# WSL bind mounts the system distro's /mnt/wslg/.X11-unix here, but the Xwayland
# mutter spawns needs to own the directory. Take the mount away if it is there.
# The umount is only a try -- what matters is the state it leaves behind, which
# is checked below rather than inferred from an exit status.
if [ -L /tmp/.X11-unix ]; then
    rm -f /tmp/.X11-unix
fi

if mountpoint -q /tmp/.X11-unix; then
    umount /tmp/.X11-unix || true
fi

# Prove the takeover worked. A busy umount leaves WSL's mount in place, and the
# next sign of it is Xwayland failing to bind its socket much later, with
# MUTTER_X11_MANDATORY=1 taking the whole session down. Nothing else checks
# this -- the shell drop-in asserts on the render node and the shared-memory
# mount, not on /tmp/.X11-unix -- so failing the unit here is the only way it
# gets said out loud.
#
# A missing directory is fine and left alone: the X server creates it.
if [ -d /tmp/.X11-unix ]; then
    PROBE="/tmp/.X11-unix/.weaselway-writable.$$"
    if ! touch "${PROBE}"; then
        echo "error: /tmp/.X11-unix not writable -- WSL mount still there" >&2
        exit 1
    fi
    rm -f "${PROBE}"
fi

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
# there, and the message says what to do. Only a warning: the shared-memory
# mount below has nothing to do with the module, and a missing render node
# already stops the session at org.gnome.Shell's AssertPathExists, right below
# this line in the journal.
DXGDRM_KO="/usr/local/lib/weaselway/modules/$(uname -r)/dxgdrm.ko"

if ! grep -q '^dxgdrm ' /proc/modules; then
    if [ -e "${DXGDRM_KO}" ]; then
        modprobe "${DXGDRM_KO}"
        udevadm trigger --subsystem-match=drm
        udevadm settle
    else
        echo "error: ${DXGDRM_KO} missing -- run install-kernel-module.sh" >&2
    fi
fi

# WSLGd, in the system distro, picked the vsock port and published the transport
# details here. We only want the shared-memory tag; the port is read by the shell
# unit itself, straight out of the same file via EnvironmentFile=.
source /mnt/wslg/mutter-rdp.env

# WSLGd mounts the shared-memory DAX share only in the system-distro mount
# namespace (/mnt/shared_memory), which is invisible from here. If a virtiofs tag
# was published, mount the same VM-wide share ourselves at a user-distro path.
# The mount point is hardcoded rather than passed in: the shell drop-in has to
# name it too, and a value in two unit files is easier to keep honest than one
# threaded through the environment.
SHARED_MEMORY_MOUNT_POINT=/mnt/wslg-shared-memory

if [ -n "${WSLG_SHARED_MEMORY_VIRTIO_TAG:-}" ]; then
    if ! mountpoint -q "${SHARED_MEMORY_MOUNT_POINT}"; then
        mkdir -p "${SHARED_MEMORY_MOUNT_POINT}"
        mount -t virtiofs -o dax "${WSLG_SHARED_MEMORY_VIRTIO_TAG}" "${SHARED_MEMORY_MOUNT_POINT}"
        chmod 0777 "${SHARED_MEMORY_MOUNT_POINT}"
    fi
fi
