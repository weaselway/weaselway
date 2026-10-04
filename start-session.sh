#!/usr/bin/env bash

# Start a desktop on dxgdrm's virtual display.
#
# The compositor is an ordinary one on its native backend: it takes the dxgdrm
# KMS node and the input devices from logind, page-flips like on real
# hardware, and knows nothing about Windows. weaselwayd picks the frames up
# and serves them to the viewer (start-viewer).
#
# The native backend wants what a display manager normally provides: a logind
# session on seat0 that owns the devices. So the session runs in a transient
# system unit with a PAM session, not under the user manager, and starting it
# takes sudo.
#
# Usage: start-session.sh [--adapter NAME] [SESSION] [-- arguments]
#        start-session.sh stop
#
#   gnome        the GNOME desktop (gnome-session)
#   gnome-shell  the bare shell, without the session's services
#   plasma       the Plasma desktop, if the system has it
#   kwin         bare KWin with a terminal in it
#
# Without SESSION it is $WEASELWAY_DEFAULT_SESSION, or gnome.

set -euo pipefail

UNIT=weaselway-session

usage() {
    cat >&2 <<USAGE
usage: start-session [--adapter NAME] [gnome|gnome-shell|plasma|kwin] [-- arguments]
       start-session stop
USAGE
    exit 1
}

if [ "${1:-}" = "--adapter" ]; then
    [ $# -ge 2 ] || usage
    # Matched by d3d12 against a substring of the adapter description.
    export MESA_D3D12_DEFAULT_ADAPTER_NAME="$2"
    shift 2
fi

SESSION="${WEASELWAY_DEFAULT_SESSION:-gnome}"
if [ $# -gt 0 ] && [ "$1" != "--" ]; then
    SESSION="$1"
    shift
fi
[ "${1:-}" = "--" ] && shift

if [ "${SESSION}" = "stop" ]; then
    exec sudo systemctl stop "${UNIT}.service"
fi

SESSION_USER="$(id -un)"
SESSION_UID="$(id -u)"

case "${SESSION}" in
    gnome)
        # The whole desktop, as a display manager would start it. The shell
        # and the session's services run as units of the user manager; the
        # shell finds the logind session through XDG_SESSION_ID, which
        # gnome-session hands on.
        COMMAND=(gnome-session "$@")
        DESKTOP=GNOME
        ;;
    gnome-shell)
        # --display-server rather than letting it guess: WSL puts
        # WAYLAND_DISPLAY and DISPLAY into every shell, and with those set
        # mutter would try to run nested.
        COMMAND=(gnome-shell --wayland --display-server "$@")
        DESKTOP=GNOME
        ;;
    plasma)
        # As above: KWin and the shell run as units of the user manager.
        COMMAND=(startplasma-wayland "$@")
        DESKTOP=KDE
        ;;
    kwin)
        # Enough to see it render and take input.
        COMMAND=(kwin_wayland --drm --xwayland "$@" konsole)
        DESKTOP=KDE
        ;;
    *)
        usage
        ;;
esac

if systemctl --quiet is-active "${UNIT}.service"; then
    echo "error: a session is running already; stop it with: start-session stop" >&2
    exit 1
fi

if [ ! -e /dev/dri/card0 ]; then
    echo "error: no /dev/dri/card0 -- did weaselway-prep load dxgdrm? (systemctl status weaselway-prep)" >&2
    exit 1
fi

# A compositor leaves its sockets' names in the user manager's environment when
# it goes. The next one is started by the user manager and would take them to
# mean that it is to run nested, in a compositor that is no longer there.
systemctl --user unset-environment WAYLAND_DISPLAY DISPLAY GNOME_SETUP_DISPLAY \
    XDG_CURRENT_DESKTOP 2>/dev/null || true

# The unit starts with a clean environment, so resolve the command here, where
# PATH is the user's.
if ! command -v "${COMMAND[0]}" > /dev/null; then
    echo "error: ${COMMAND[0]} is not installed" >&2
    if [ "${DESKTOP}" = "KDE" ]; then
        echo "Plasma is off by default; set weaselway.plasma.enable = true in /etc/nixos/configuration.nix and rebuild." >&2
    fi
    exit 1
fi
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
# Extra variables for the session, space separated, for debugging:
#   WEASELWAY_SESSION_ENV="QT_LOGGING_RULES=kwin_*.debug=true" start-session kwin
for ASSIGNMENT in ${WEASELWAY_SESSION_ENV:-}; do
    PROPERTIES+=(--property="Environment=${ASSIGNMENT}")
done
if [ -n "${LD_LIBRARY_PATH:-}" ]; then
    PROPERTIES+=(--property="Environment=LD_LIBRARY_PATH=${LD_LIBRARY_PATH}")
fi
if [ -n "${MESA_D3D12_DEFAULT_ADAPTER_NAME:-}" ]; then
    PROPERTIES+=(--property="Environment=MESA_D3D12_DEFAULT_ADAPTER_NAME=${MESA_D3D12_DEFAULT_ADAPTER_NAME}")
fi

# logind wants a VT number for a session on a seat that has VTs, and refuses
# one on a seat that has none. The WSL kernel has them (CONFIG_VT=y); a custom
# kernel may not.
if [ -e /dev/tty0 ] && [ -e /dev/tty1 ]; then
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
fi

# A previous run that failed leaves the unit behind in failed state.
sudo systemctl reset-failed "${UNIT}.service" 2>/dev/null || true

sudo systemd-run --unit="${UNIT}" --collect \
    --quiet --description="Weaselway session (${SESSION})" \
    "${PROPERTIES[@]}" \
    -- "${COMMAND[@]}"

# weaselwayd is enabled for the user manager; this is for the case that it was
# stopped by hand.
systemctl --user start weaselwayd.service || true

sleep 3
if ! systemctl --quiet is-active "${UNIT}.service"; then
    systemctl --no-pager status "${UNIT}.service" || true
    exit 1
fi

cat <<MSG
The ${SESSION} session is up. Show it on Windows with: start-viewer

Follow the session:   journalctl -fu ${UNIT}
Follow weaselwayd:    journalctl --user -fu weaselwayd
Stop:                 start-session stop
MSG
