#!/usr/bin/env bash

# kms-wsl spike: start an unmodified compositor on dxgdrm's virtual display.
#
# This is the other way to run a session, next to start-gnome-shell.sh. There,
# mutter runs headless and serves RDP itself. Here, the compositor uses its
# normal native backend: it takes the dxgdrm KMS node and the input devices
# from logind, page-flips like on real hardware, and knows nothing about
# Windows. weaselway-presenter is what picks the frames up.
#
# The native backend wants what a display manager normally provides: a logind
# session on seat0 that owns the devices. So the compositor runs in a transient
# system unit with a PAM session, not under the user manager.
#
# Usage: start-kms-session.sh [gnome|kwin|stop] [-- compositor arguments]
#
# Do not run this next to start-gnome-shell.sh: both shells want the same
# session bus names and the same Wayland socket.

set -euo pipefail

UNIT=weaselway-kms
SESSION="${1:-gnome}"
[ $# -gt 0 ] && shift
[ "${1:-}" = "--" ] && shift

if [ "${SESSION}" = "stop" ]; then
    exec sudo systemctl stop "${UNIT}.service"
fi

SESSION_USER="$(id -un)"
SESSION_UID="$(id -u)"

case "${SESSION}" in
    gnome)
        # --display-server rather than letting it guess: WSL puts
        # WAYLAND_DISPLAY and DISPLAY into every shell, and with those set
        # mutter would try to run nested.
        #
        # WEASELWAY_KMS_GNOME_SHELL picks the gnome-shell to run. The NixOS
        # image points it at nixpkgs' own build, linked against unpatched
        # mutter, because that is the claim under test.
        COMMAND=("${WEASELWAY_KMS_GNOME_SHELL:-gnome-shell}" --wayland --display-server "$@")
        DESKTOP=GNOME
        ;;
    kwin)
        # A bare compositor with a terminal in it; enough to see it render
        # and take input.
        COMMAND=(kwin_wayland --drm --xwayland "$@" konsole)
        DESKTOP=KDE
        ;;
    *)
        echo "usage: $0 [gnome|kwin|stop] [-- compositor arguments]" >&2
        exit 1
        ;;
esac

if [ ! -e /dev/dri/card0 ]; then
    echo "error: no /dev/dri/card0 -- did weaselway-prep load dxgdrm? (systemctl status weaselway-prep)" >&2
    exit 1
fi

# The unit starts with a clean environment, so resolve the compositor here,
# where PATH is the user's.
COMMAND[0]="$(command -v "${COMMAND[0]}")"

# The session gets the user's PATH without the Windows directories WSL appends
# to it: those have spaces in them, which Environment= splits on, and nothing
# in the session should be starting Windows programs by accident anyway.
SESSION_PATH="$(printf '%s' "${PATH}" | tr ':' '\n' | grep -v '^/mnt/' | paste -sd:)"

PROPERTIES=(
    --property="User=${SESSION_USER}"
    --property=PAMName=login
    --property=Environment=XDG_SESSION_TYPE=wayland
    --property=Environment=XDG_SESSION_CLASS=user
    --property=Environment=XDG_SEAT=seat0
    --property="Environment=XDG_CURRENT_DESKTOP=${DESKTOP}"
    # The user manager's bus, which pam_systemd starts with the session, so
    # that services started from the session land on the same one.
    --property="Environment=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/${SESSION_UID}/bus"
    --property="Environment=PATH=${SESSION_PATH}"
    # The d3d12 driver's own needs, as in environment.d/10-weaselway.conf.
    --property=Environment=GALLIUM_DRIVER=d3d12
    --property=Environment=GSK_RENDERER=gl
    --property=Environment=WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
)

# Where icon and cursor themes, schemas and the like are found. A login shell
# gets this from the profile; the unit has no profile. Without it KWin finds
# no cursor theme and shows no pointer at all.
if [ -n "${XDG_DATA_DIRS:-}" ]; then
    PROPERTIES+=(--property="Environment=XDG_DATA_DIRS=${XDG_DATA_DIRS}")
fi
if [ "${SESSION}" = "kwin" ]; then
    # Plasma's own theme is not installed; GNOME's is.
    PROPERTIES+=(--property=Environment=XCURSOR_THEME=Adwaita)
fi
if [ -n "${LD_LIBRARY_PATH:-}" ]; then
    PROPERTIES+=(--property="Environment=LD_LIBRARY_PATH=${LD_LIBRARY_PATH}")
fi
if [ -n "${MESA_D3D12_DEFAULT_ADAPTER_NAME:-}" ]; then
    PROPERTIES+=(--property="Environment=MESA_D3D12_DEFAULT_ADAPTER_NAME=${MESA_D3D12_DEFAULT_ADAPTER_NAME}")
fi

# logind wants a VT number for a session on a seat that has VTs, and refuses
# one on a seat that has none. Whether the WSL kernel's CONFIG_VT=y results in
# usable VTs is one of the things this spike finds out, so handle both.
if [ -e /dev/tty0 ] && [ -e /dev/tty1 ]; then
    echo "seat0 has VTs: running the session on tty1"
    PROPERTIES+=(
        --property=TTYPath=/dev/tty1
        --property=TTYReset=yes
        --property=TTYVHangup=yes
        --property=StandardInput=tty
        --property=StandardOutput=journal
        --property=StandardError=journal
        --property=UtmpIdentifier=tty1
        --property=UtmpMode=user
        --property=Environment=XDG_VTNR=1
    )
else
    echo "seat0 has no VTs: running the session without one"
fi

# A previous run that failed leaves the unit behind in failed state.
sudo systemctl reset-failed "${UNIT}.service" 2>/dev/null || true

sudo systemd-run --unit="${UNIT}" --collect \
    --description="${DESKTOP} on the dxgdrm KMS display (kms-wsl spike)" \
    "${PROPERTIES[@]}" \
    -- "${COMMAND[@]}"

sleep 3
systemctl --no-pager status "${UNIT}.service" || true

cat <<MSG

Follow the compositor:   journalctl -fu ${UNIT}
Session and seat:        loginctl; loginctl seat-status seat0
Read the frames back:    weaselway-presenter      (JPEGs in /tmp/weaselway-frames)
Stop:                    start-kms-session stop
MSG
