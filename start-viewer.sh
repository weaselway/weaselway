#!/usr/bin/env bash

# Connect to the running session. start-gnome-shell.sh must be up first: it is
# mutter that binds the vsock port, and this is the FreeRDP client on the
# Windows side that displays what it serves.

set -eu -o pipefail

source /mnt/wslg/mutter-rdp.env

/mnt/c/Weaselway/sdl-freerdp.exe /u:dummy /d:dummy /p:dummy \
    /v:vsock://$WSLG_VM_ID:$MUTTER_RDP_VSOCK_PORT \
    /wslgsharedmemorypath:"$WSLG_SHARED_MEMORY_OB_DIRECTORY" \
    /cert:ignore \
    /dynamic-resolution \
    /w:1280 \
    /kbd:layout:German \
    /log-level:warn
