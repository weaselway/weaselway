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
#
# Usage: install-units.sh [--adapter NAME]
#
# --adapter pins the GPU d3d12 renders on, for machines where the first adapter
# Windows lists is not the one you want. It is matched against a substring of
# the adapter description, so "intel" or "nvidia" does. Without it, no adapter
# is pinned and d3d12 picks; passing an empty name clears a previous choice.

set -euo pipefail

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
        *)
            echo "error: unknown argument '$1'" >&2
            exit 1
            ;;
    esac
done

set -x

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Read after 10-weaselway.conf -- environment.d files are applied in name order,
# so this one wins on the variable it sets. Kept separate from the shipped conf
# so that file stays a verbatim copy of what is in the checkout.
ADAPTER_CONF="${HOME}/.config/environment.d/20-weaselway-adapter.conf"

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

# Only touched when --adapter was given: a plain re-run should not silently drop
# a choice made earlier.
if [ "${ADAPTER_SET}" -eq 1 ]; then
    if [ -n "${ADAPTER}" ]; then
        mkdir -p "$(dirname "${ADAPTER_CONF}")"
        printf 'MESA_D3D12_DEFAULT_ADAPTER_NAME=%s\n' "${ADAPTER}" > "${ADAPTER_CONF}"
    else
        rm -f "${ADAPTER_CONF}"
    fi
fi

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
    $(grep -hvE '^[[:space:]]*(#|$)' \
        "${SCRIPT_DIR}/environment.d/10-weaselway.conf" \
        $([ -f "${ADAPTER_CONF}" ] && echo "${ADAPTER_CONF}"))

# An adapter cleared just now is still in the manager's environment from before.
if [ ! -f "${ADAPTER_CONF}" ]; then
    systemctl --user unset-environment MESA_D3D12_DEFAULT_ADAPTER_NAME
fi

# WSL injects this, pointing at the PulseAudio the stock WSLGd ran; ours runs
# none, and libpulse clients only find pipewire-pulse when it is unset. The
# shell drop-in carries UnsetEnvironment=PULSE_SERVER so this survives a
# restart -- this line is for the manager that is already running, and so for
# anything started in this session without going through the shell unit.
systemctl --user unset-environment PULSE_SERVER
