#!/usr/bin/env bash

# Start the GNOME session. Everything this script used to do itself now lives in
# units, installed by install-units.sh:
#
#   - weaselway-prep.service loads dxgdrm and mounts the shared-memory share
#   - ~/.config/environment.d/10-weaselway.conf gives the user manager the
#     driver and session variables, XDG_SESSION_TYPE among them
#   - a drop-in on org.gnome.Shell@.service supplies the headless RDP arguments
#     and reads the vsock port out of /mnt/wslg/mutter-rdp.env
#
# The dbus-run-session wrapper that used to be here is gone with them: the user
# manager already owns a session bus, and the session has to share it rather
# than get one of its own.
#
# Usage: start-gnome-shell.sh [--adapter NAME] [session]
#
# `ls /usr/share/gnome-session/sessions/` lists what the distro offers -- ubuntu,
# gnome and gnome-login on Ubuntu 26.04. Nothing in the units is specific to any
# of them.
#
# --adapter pins the GPU d3d12 renders on for this session only, overriding
# whatever `install-units.sh --adapter` settled on; an empty name pins nothing.

set -euo pipefail

SESSION=""
ADAPTER=""
ADAPTER_SET=0

while [ $# -gt 0 ]; do
    case "$1" in
        --adapter)
            [ $# -ge 2 ] || { echo "error: --adapter needs a value" >&2; exit 1; }
            ADAPTER="$2"
            ADAPTER_SET=1
            shift 2
            ;;
        --adapter=*)
            ADAPTER="${1#--adapter=}"
            ADAPTER_SET=1
            shift
            ;;
        -*)
            echo "error: unknown option '$1'" >&2
            exit 1
            ;;
        *)
            SESSION="$1"
            shift
            ;;
    esac
done

SESSION="${SESSION:-ubuntu}"
TARGET="gnome-session@${SESSION}.target"

# The manager's environment outlives a session, so an --adapter from a previous
# run is still in it. Put the configured value back when none is given, rather
# than inheriting the last one silently.
ADAPTER_CONF="${HOME}/.config/environment.d/20-weaselway-adapter.conf"
if [ "${ADAPTER_SET}" -eq 0 ] && [ -f "${ADAPTER_CONF}" ]; then
    ADAPTER="$(sed -n 's/^MESA_D3D12_DEFAULT_ADAPTER_NAME=//p' "${ADAPTER_CONF}")"
fi

if [ -n "${ADAPTER}" ]; then
    systemctl --user set-environment "MESA_D3D12_DEFAULT_ADAPTER_NAME=${ADAPTER}"
else
    systemctl --user unset-environment MESA_D3D12_DEFAULT_ADAPTER_NAME
fi

# The shell's instance name is the gnome-shell *mode*, which is not the session
# name: ubuntu.session wants org.gnome.Shell@ubuntu.service, but gnome.session
# wants @user and gnome-login.session wants @gdm. Ask the target which one it
# requires instead of guessing -- `show` loads the unit, so this works before it
# has ever been started.
SHELL_UNIT="$(systemctl --user show "${TARGET}" --property=Requires --value \
    | tr ' ' '\n' | grep -m1 '^org\.gnome\.Shell@' || true)"

# A previous failed attempt leaves the target and the shell in failed state, and
# systemd refuses to start a failed unit again without this. Errors are dropped
# rather than reported: on a first run of the day neither unit is loaded yet,
# and "Unit not loaded" is the expected answer, not a problem.
# Unquoted so an undiscovered shell unit expands to no argument at all.
# shellcheck disable=SC2086
systemctl --user reset-failed "${TARGET}" ${SHELL_UNIT} 2>/dev/null || true

# A failed attempt also takes the session bus down with it
# (gnome-session@.target has OnFailure=gnome-session-shutdown.target, which
# stops dbus.service), so bring it back before trying again.
systemctl --user start dbus.service

systemctl --user start "${TARGET}"

# The target reports success as soon as its components are up, which tells us
# nothing about whether the shell is actually serving. Show where it got to.
systemctl --user --no-pager status "${SHELL_UNIT:-${TARGET}}"
