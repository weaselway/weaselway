#!/usr/bin/env bash

# Show the session on Windows. weaselwayd binds the vsock port, and this is
# the FreeRDP client on the Windows side that displays what it serves. It can
# be started before ww-start-session.sh: the window stays empty until a
# compositor is up.

set -eu -o pipefail

# The NixOS image ships the client in the store and points this at it.
VIEWER="${WEASELWAY_VIEWER:-/mnt/c/Weaselway/sdl-freerdp.exe}"

# The port weaselwayd listens on: environment.d gives it to the user manager,
# which is what weaselwayd's unit inherits.
PORT="$(systemctl --user show-environment | sed -n 's/^WEASELWAY_VSOCK_PORT=//p')"
if [ -z "${PORT}" ]; then
    echo "error: WEASELWAY_VSOCK_PORT is not in the user manager's environment -- is environment.d/10-weaselway.conf installed?" >&2
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
# client maps to read the frames weaselwayd writes there. WSL names it after the VM
# (WslCoreVm::InitializeGuest in microsoft/WSL), with the ID in upper case.
SHARED_MEMORY_PATH="WSL\\${VM_ID^^}\\wslg"

COMMAND=(
    "${VIEWER}" /u:dummy /d:dummy /p:dummy
    /v:vsock://"${VM_ID}":"${PORT}"
    /wslgsharedmemorypath:"${SHARED_MEMORY_PATH}"
    /cert:ignore
    /dynamic-resolution
    /w:1280
    /log-level:warn
    /multitouch
    /sdl-touchpad-gestures
    /audio-mode:redirect
    /microphone
    "$@"
)

# DEBUG=1 runs the viewer in the foreground, with its log on the terminal.
if [ "${DEBUG:-}" = 1 ]; then
    exec "${COMMAND[@]}"
fi

# Otherwise it runs on in the background, in a session of its own so that
# closing the terminal does not end it, with its log in a file.
LOG="${XDG_RUNTIME_DIR:-/tmp}/weaselway-viewer.log"
setsid "${COMMAND[@]}" > "${LOG}" 2>&1 < /dev/null &
VIEWER_PID=$!
disown

# A viewer that cannot connect or map the shared memory exits right away.
sleep 2
if ! kill -0 "${VIEWER_PID}" 2> /dev/null; then
    echo "error: the viewer exited; its log ($LOG):" >&2
    cat "${LOG}" >&2
    exit 1
fi

echo "The viewer is running. Its log: ${LOG}"
echo "To see the log on the terminal instead: DEBUG=1 ww-start-viewer"
