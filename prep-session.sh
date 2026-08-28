#!/usr/bin/env bash

# Root-side preparation for the GNOME session, run once per boot out of
# weaselway-prep.service. All of this used to sit inline in start-gnome-shell.sh
# behind a sudo; it moved here because the session itself is now a set of units
# under the user manager, which has no business calling sudo.

set -xeuo pipefail

# WSL bind mounts the system distro's /mnt/wslg/.X11-unix here, but the Xwayland
# mutter spawns needs to own the directory. Take the mount away if it is there.
# Only a try: a busy mount is not a reason to abort the boot.
if [ -L /tmp/.X11-unix ]; then
    rm -f /tmp/.X11-unix
fi

if mountpoint -q /tmp/.X11-unix; then
    umount /tmp/.X11-unix || true
fi

# The d3d12 driver needs the render node that dxgdrm provides, and nothing loads
# the module at boot. Read /proc/modules directly rather than piping lsmod, so
# pipefail has nothing to trip over. The udevadm calls are what turn the module
# into /dev/dri/renderD128, which is what the shell drop-in asserts on.
if ! grep -q '^dxgdrm ' /proc/modules; then
    modprobe dxgdrm
    udevadm trigger --subsystem-match=drm
    udevadm settle
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
