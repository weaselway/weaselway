#!/usr/bin/env bash

# Install the audio half of the session: PipeWire, and the config that gives it
# the two ends of the RDP audio bridge.
#
# There is no PulseAudio in this setup. The system distro used to run one with a
# pair of custom RDP modules, and mutter spoke a bespoke socket protocol to
# them; it now connects to two stock protocol-simple servers over TCP instead.
# pipewire-pulse is still installed, because that is what libpulse clients --
# gnome-shell's own volume control among them -- talk to.
#
# Kept out of install-units.sh deliberately: that script is file installation
# and systemctl only, with no apt and no network, and gets re-run whenever the
# GPU choice changes.
#
# Usage: install-audio.sh

set -xeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Present already on any ubuntu-desktop install; named explicitly so a minimal
# user distro gets them too. wireplumber is not optional -- without a session
# manager nothing picks a default device or links the streams up.
sudo apt install -y pipewire pipewire-pulse wireplumber

install -D -m 0644 \
    "${SCRIPT_DIR}/pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf" \
    "${HOME}/.config/pipewire/pipewire.conf.d/10-weaselway-rdp-audio.conf"

# context.objects and context.modules are read when the daemon starts, so
# nothing above is live yet. Restart rather than reload: PipeWire has no
# reload, and the sockets mean clients reconnect on their own.
# Nothing to enable: the packages ship their own enablement
# (/etc/systemd/user/default.target.wants/, via `systemctl --global enable` in
# the postinst), and it hangs off the *user manager's* default.target rather
# than the GNOME session -- so audio is up before gnome-session starts and does
# not go down with it. wireplumber has no symlink of its own because it is
# WantedBy=pipewire.service and gets pulled in.
#
# The reload is for the case where apt above installed those units just now: a
# manager that was already running has not seen them yet.
systemctl --user daemon-reload

# Sockets as well as services, and sockets first. The .socket units own the
# actual filesystem nodes (notably $XDG_RUNTIME_DIR/pulse/native, which every
# libpulse client connects to); the services only inherit the fds. Restarting
# services alone can leave pipewire-pulse holding a listening socket whose
# directory entry is gone -- the kernel still reports it as LISTEN, but
# clients get "Connection refused" and it looks like pipewire-pulse died.
# Restarting the socket units too makes the node get recreated regardless.
systemctl --user stop \
    pipewire.socket pipewire-pulse.socket \
    pipewire.service pipewire-pulse.service wireplumber.service

systemctl --user start pipewire.socket pipewire-pulse.socket
systemctl --user start pipewire.service wireplumber.service pipewire-pulse.service

set +x

# Wait for readiness rather than sleeping a fixed amount. Startup here is not
# quick or predictable: stopping pipewire-pulse can take ~30s when rtkit is
# unreachable and its DBus call has to time out (harmless, but it pushes the
# restart well past any sleep worth hardcoding).
echo
printf "waiting for pipewire"
for _ in $(seq 60); do
    if wpctl status > /dev/null 2>&1 &&
       [ -S "${XDG_RUNTIME_DIR}/pulse/native" ] &&
       { ! command -v pactl > /dev/null 2>&1 || pactl info > /dev/null 2>&1; }; then
        break
    fi
    printf "."
    sleep 0.5
done
echo

# wpctl for the device list: it ships with wireplumber, which we require
# anyway, whereas pactl comes from pulseaudio-utils and may not be installed.
# (The pipewire-pulse check further down does use pactl, but only if present.)
#
# "Remote Desktop Audio" must be present and starred (default) *now*, with no
# RDP viewer attached and nothing connected to either port. That is the whole
# point of fronting the playback server with a null sink. If it is missing, the
# config did not load: check `journalctl --user -u pipewire -n 50`.
#
# "Remote Desktop Microphone" is expected to be absent here. It is the
# per-client stream itself, so it only exists while mutter is connected.
echo
echo "== Audio devices (no client connected) =="
wpctl status | sed -n '/^Audio/,/^Video/p'
echo "== Bridge ports =="
ss -ltn '( sport = :4711 or sport = :4712 )' || true

# Check the PulseAudio compatibility socket explicitly rather than trusting
# that the unit is "active". The failure mode above is silent: the service
# stays running and enabled while every libpulse client -- gnome-shell's
# volume control included -- gets "Connection refused".
echo
echo "== pipewire-pulse =="
if [ ! -S "${XDG_RUNTIME_DIR}/pulse/native" ]; then
    echo "ERROR: ${XDG_RUNTIME_DIR}/pulse/native is missing." >&2
    echo "       libpulse clients cannot connect. Try:" >&2
    echo "         systemctl --user restart pipewire-pulse.socket pipewire-pulse.service" >&2
    exit 1
fi
if command -v pactl > /dev/null 2>&1; then
    if ! pactl info > /dev/null 2>&1; then
        echo "ERROR: the socket exists but pactl cannot connect." >&2
        echo "       Check: journalctl --user -u pipewire-pulse -n 50" >&2
        echo "       and that PULSE_SERVER is unset (install-units.sh clears it)." >&2
        exit 1
    fi
    pactl info | grep -E "^(Server Name|Default Sink|Default Source):"
else
    echo "socket present at ${XDG_RUNTIME_DIR}/pulse/native"
    echo "(install pulseaudio-utils for a full pactl check)"
fi
