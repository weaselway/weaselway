#!/usr/bin/env bash

# Install the units that make up the session. Three pieces, in two scopes:
#
#   - a system unit doing the root-side preparation (kernel module, mounts)
#   - environment for the user manager, which every session service inherits
#   - a drop-in turning the distro's stock gnome-shell unit into our headless
#     RDP one
#
# After this, `systemctl --user start gnome-session@ubuntu.target` is the whole
# of starting a session.

set -xeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# A unit file of the same name under ~/.config/systemd/user shadows the distro's
# entirely -- the drop-in would then be layered onto that copy instead, and the
# ExecStart= we are replacing would not be the one the distro ships. Refuse
# rather than install into a setup where the result is not what the file says.
SHADOWED="${HOME}/.config/systemd/user/org.gnome.Shell@.service"
if [ -e "${SHADOWED}" ]; then
    echo "error: ${SHADOWED} shadows the distro unit; remove it first" >&2
    exit 1
fi

# The prep script runs as root from a system unit, so it is installed out of the
# checkout rather than referenced in it -- a unit with an ExecStart= pointing
# into a home directory breaks the moment the checkout moves. It lives under
# libexec/ for the same reason it is installed under /usr/local/lib: it is the
# unit's to run, not yours.
sudo install -D -m 0755 \
    "${SCRIPT_DIR}/libexec/prep-session.sh" \
    /usr/local/lib/weaselway/prep-session.sh

sudo install -D -m 0644 \
    "${SCRIPT_DIR}/units/weaselway-prep.service" \
    /etc/systemd/system/weaselway-prep.service

install -D -m 0644 \
    "${SCRIPT_DIR}/environment.d/10-weaselway.conf" \
    "${HOME}/.config/environment.d/10-weaselway.conf"

install -D -m 0644 \
    "${SCRIPT_DIR}/units/org.gnome.Shell@.service.d/weaselway.conf" \
    "${HOME}/.config/systemd/user/org.gnome.Shell@.service.d/weaselway.conf"

# The drop-in used to be installed under the @ubuntu instance. Left behind it
# would still apply, and instance drop-ins are read after template-wide ones --
# so the stale copy would quietly win, for that one session only.
LEGACY="${HOME}/.config/systemd/user/org.gnome.Shell@ubuntu.service.d"
rm -f "${LEGACY}/weaselway.conf"
rmdir "${LEGACY}" 2>/dev/null || true

sudo systemctl daemon-reload
sudo systemctl enable --now weaselway-prep.service

systemctl --user daemon-reload

# environment.d is read only when the user manager starts, so nothing we just
# installed is live yet. Push the same values in by hand, so a session can be
# started without logging out first; the file is what makes them survive the
# next `wsl --shutdown`. Unquoted on purpose -- the conf is KEY=VALUE lines,
# which is exactly set-environment's argument form.
# shellcheck disable=SC2046
systemctl --user set-environment \
    $(grep -vE '^[[:space:]]*(#|$)' "${SCRIPT_DIR}/environment.d/10-weaselway.conf")
