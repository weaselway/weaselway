#!/usr/bin/env bash

# Root-side preparation for the session, run once per boot out of
# weaselway-prep.service.

set -xeuo pipefail

# dxgdrm provides the render node d3d12 clients need and the KMS node the
# compositor drives; nothing loads it at boot. It lives in the Nix store, not in
# WSL's /lib/modules overlay, so modprobe gets that as its root. On kernels
# built without DRM core the package brings it along as modules, and its
# modules.dep loads them first. The path carries the kernel release, so after
# a WSL kernel update the module is simply missing. Not fatal here:
# ww-start-session.sh refuses to start without the KMS node.
#
# /proc/modules rather than `lsmod | grep`, which trips pipefail.
: "${DXGDRM_ROOT:?set by weaselway-prep.service}"

if ! grep -q '^dxgdrm ' /proc/modules; then
    if [ -e "${DXGDRM_ROOT}/lib/modules/$(uname -r)/extra/dxgdrm.ko" ]; then
        modprobe -d "${DXGDRM_ROOT}" dxgdrm
        udevadm trigger --subsystem-match=drm
        udevadm settle
    else
        echo "error: no dxgdrm.ko for $(uname -r) in ${DXGDRM_ROOT} -- the dxgdrm flake has no module for this WSL kernel yet" >&2
    fi
fi

# weaselwayd creates the input devices through uinput and the compositor reads
# them as evdev. Both are modules in the WSL kernel and nothing loads them.
# By path from WSL's own /lib/modules, which NixOS' modprobe does not search.
# Not fatal: the session works without them, just without input.
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

# Take /tmp/.X11-unix back from WSL, so the compositor can create its X socket
# there. WSL's wslg.service bind-mounts /mnt/wslg/.X11-unix over it read-only,
# and X-mount.mkdir creates the mountpoint as root, mode 0755 -- so umount
# alone leaves a directory the user cannot write to. Hence rm -rf and a fresh
# 1777 directory. The unit is ordered After=wslg.service, so WSL has had its
# turn by now.
if [ -L /tmp/.X11-unix ]; then
    rm -f /tmp/.X11-unix
fi

# Mounts can be stacked, and each umount pops one. Bounded so a mount that
# will not go away fails the check below instead of spinning.
for _ in 1 2 3 4 5; do
    mountpoint -q /tmp/.X11-unix || break
    umount /tmp/.X11-unix || break
done

rm -rf /tmp/.X11-unix
mkdir -m 1777 /tmp/.X11-unix

# Verify the takeover, because the failure is otherwise silent until X11
# applications do not start. Not a `touch` probe: root can write into a
# root-owned 0755 directory. Not `mountpoint`: tmp.mount covers /tmp after
# wslg.service mounted onto the old one, and the shadowed bind is still listed
# in mountinfo. Same device as /tmp means nothing is mounted over it.
if [ "$(stat -c %d /tmp/.X11-unix)" != "$(stat -c %d /tmp)" ]; then
    echo "error: something is still mounted over /tmp/.X11-unix" >&2
    exit 1
fi

MODE="$(stat -c %a /tmp/.X11-unix)"
if [ "${MODE}" != "1777" ]; then
    echo "error: /tmp/.X11-unix has mode ${MODE}, expected 1777" >&2
    exit 1
fi

# The share gfxredir allocates its buffers on. WSL exposes it as the virtiofs
# tag "wslg" only when GUI apps are on, which they are unless .wslconfig says
# guiApplications=false. Fatal: without it weaselwayd has nowhere to put the
# frames.
#
# The first wsl.exe of the other elevation (a desktop shortcut after an admin
# terminal, or the reverse) makes WSL unmount every virtiofs mount it finds and
# ask the host to remount it as a drive; for this share that times out after
# 30 seconds and leaves it unmounted. WSL leaves mounts below its own
# virtiofs-mounts directory alone (2.9 and later), and a private mount does not
# lose its copy in systemd's namespace when WSL unmounts it in a new one
# (earlier versions). weaselwayd's default (--shm) is a link to it.
SHARED_MEMORY_MOUNT_POINT=/run/wsl/virtiofs-mounts/weaselway-wslg
SHARED_MEMORY_LINK=/mnt/wslg-shared-memory

if ! mountpoint -q "${SHARED_MEMORY_MOUNT_POINT}"; then
    mkdir -p "${SHARED_MEMORY_MOUNT_POINT}"
    if ! mount -t virtiofs -o dax wslg "${SHARED_MEMORY_MOUNT_POINT}"; then
        echo "error: cannot mount the WSLg shared-memory share -- is guiApplications=false set in .wslconfig? Remove it and run wsl --shutdown." >&2
        exit 1
    fi
    mount --make-private "${SHARED_MEMORY_MOUNT_POINT}"
    chmod 0777 "${SHARED_MEMORY_MOUNT_POINT}"
fi

# Where earlier versions mounted it: an empty directory now.
if [ -d "${SHARED_MEMORY_LINK}" ] && [ ! -L "${SHARED_MEMORY_LINK}" ]; then
    umount "${SHARED_MEMORY_LINK}" 2>/dev/null || true
    rmdir "${SHARED_MEMORY_LINK}"
fi
ln -sfnT "${SHARED_MEMORY_MOUNT_POINT}" "${SHARED_MEMORY_LINK}"
