#!/usr/bin/env bash

# Connect to the running session. start-gnome-shell.sh must be up first: it is
# mutter that binds the vsock port, and this is the FreeRDP client on the
# Windows side that displays what it serves.

set -eu -o pipefail

# The port mutter was started with: environment.d gives it to the user manager,
# which is what the shell unit inherits.
PORT="$(systemctl --user show-environment | sed -n 's/^MUTTER_RDP_VSOCK_PORT=//p')"
if [ -z "${PORT}" ]; then
    echo "error: MUTTER_RDP_VSOCK_PORT is not in the user manager's environment -- is environment.d/10-weaselway.conf installed?" >&2
    exit 1
fi

# The Hyper-V VM the client connects to. wslinfo is WSL's own /init under
# another name; NixOS-WSL does not link it into /bin, so fall back to calling
# /init by that name directly.
if command -v wslinfo > /dev/null; then
    VM_ID="$(wslinfo --vm-id -n)"
else
    VM_ID="$(exec -a wslinfo /init --vm-id -n)"
fi

# The NT object directory behind the "wslg" shared-memory share, which the
# client maps to read the frames mutter writes there. WSL names it after the VM
# (WslCoreVm::InitializeGuest in microsoft/WSL), with the ID in upper case.
SHARED_MEMORY_PATH="WSL\\${VM_ID^^}\\wslg"

/mnt/c/Weaselway/sdl-freerdp.exe /u:dummy /d:dummy /p:dummy \
    /v:vsock://"${VM_ID}":"${PORT}" \
    /wslgsharedmemorypath:"${SHARED_MEMORY_PATH}" \
    /cert:ignore \
    /dynamic-resolution \
    /w:1280 \
    /kbd:layout:German \
    /log-level:warn \
    /multitouch \
    /sdl-touchpad-gestures \
    /audio-mode:redirect \
    /microphone \
    "$@"
