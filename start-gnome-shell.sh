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
# Usage: start-gnome-shell.sh [session]
#
# `ls /usr/share/gnome-session/sessions/` lists what the distro offers -- ubuntu,
# gnome and gnome-login on Ubuntu 26.04. Nothing in the units is specific to any
# of them.

set -euo pipefail

SESSION="${1:-ubuntu}"
TARGET="gnome-session@${SESSION}.target"

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
