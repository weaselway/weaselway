#!/usr/bin/env bash
# selection.c against a real mutter, without a desktop: a headless mutter on a
# bus of its own, one selection-tool that owns the clipboard and one that
# reads it. Between them that is every call and signal weaselwayd uses.
#
# Needs mutter, dbus-run-session and a built tests/selection-tool:
#   make tests/selection-tool && tests/selection-mutter.sh
set -euo pipefail

tool=$(dirname "$(readlink -f "$0")")/selection-tool

if [ -z "${SELECTION_TEST_INNER:-}" ]; then
    XDG_RUNTIME_DIR=$(mktemp -d)
    export XDG_RUNTIME_DIR
    trap 'rm -rf "$XDG_RUNTIME_DIR"' EXIT
    # Where /etc/dbus-1/session.conf is not the bus's configuration (NixOS),
    # DBUS_SESSION_CONF says which file is.
    SELECTION_TEST_INNER=1 dbus-run-session \
        ${DBUS_SESSION_CONF:+--config-file="$DBUS_SESSION_CONF"} -- "$0"
    exit
fi

text="copied on the other side: äöü"
log=$XDG_RUNTIME_DIR/log

mutter --headless --wayland --no-x11 --virtual-monitor 640x480 >"$log.mutter" 2>&1 &
mutter=$!
printf '%s' "$text" >"$log.text"
"$tool" offer "text/plain;charset=utf-8" "$log.text" 2>"$log.offer" &
offer=$!
trap 'kill $offer $mutter 2>/dev/null || true' EXIT

# Reads as soon as the clipboard has an owner, which is once mutter is up and
# the other tool's offer has gone through.
if ! got=$(timeout 30 "$tool" read "text/plain;charset=utf-8" 2>"$log.read"); then
    echo "FAIL: nothing read" >&2
    tail -n 20 "$log".* >&2
    exit 1
fi

if [ "$got" != "$text" ]; then
    echo "FAIL: read '$got', wanted '$text'" >&2
    exit 1
fi
# The owner is told that the clipboard is its own, and nothing else.
if grep -q '^owner: \[..*\]' "$log.offer"; then
    echo "FAIL: the owner was told about its own offer" >&2
    cat "$log.offer" >&2
    exit 1
fi
grep -q '^requested: text/plain;charset=utf-8' "$log.offer"
echo "PASS: '$got'"
