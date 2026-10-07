#!/usr/bin/env bash
# The ext-data-control-v1 side of the selection against a real KWin, without a
# desktop: a virtual KWin on a runtime directory of its own, selection-tool on
# one side and wl-copy/wl-paste on the other.
#
# Needs kwin_wayland, wl-clipboard, dbus-run-session and a built
# tests/selection-tool:
#   make tests/selection-tool && tests/selection-kwin.sh
set -euo pipefail

tool=$(dirname "$(readlink -f "$0")")/selection-tool

if [ -z "${SELECTION_TEST_INNER:-}" ]; then
    XDG_RUNTIME_DIR=$(mktemp -d)
    export XDG_RUNTIME_DIR
    trap 'rm -rf "$XDG_RUNTIME_DIR"' EXIT
    # With these set KWin would run nested, and wl-copy would talk to that.
    unset WAYLAND_DISPLAY DISPLAY
    SELECTION_TEST_INNER=1 dbus-run-session \
        ${DBUS_SESSION_CONF:+--config-file="$DBUS_SESSION_CONF"} -- "$0"
    exit
fi

log=$XDG_RUNTIME_DIR/log
pids=()
trap 'kill "${pids[@]}" 2>/dev/null || true' EXIT

fail() {
    echo "FAIL: $*" >&2
    tail -n 20 "$log".* >&2
    exit 1
}

# The tools start first: they are to find the compositor when it comes.
text="copied on the other side: äöü"
printf '%s' "$text" >"$log.text"
"$tool" offer "text/plain;charset=utf-8" "$log.text" 2>"$log.offer" &
offer=$!
pids+=("$offer")
"$tool" watch 2>"$log.watch" &
pids+=($!)

kwin_wayland --virtual --no-lockscreen --socket wayland-test --width 640 --height 480 \
    >"$log.kwin" 2>&1 &
pids+=($!)
export WAYLAND_DISPLAY=wayland-test

# Session -> selection-tool: wl-paste reads what selection-tool offers.
got=
for _ in $(seq 100); do
    got=$(timeout 2 wl-paste --no-newline --type "text/plain;charset=utf-8" 2>>"$log.paste") &&
        [ "$got" = "$text" ] && break
    sleep 0.1
done
[ "$got" = "$text" ] || fail "wl-paste read '$got', wanted '$text'"
grep -q '^requested: text/plain;charset=utf-8' "$log.offer" || fail "the owner was not asked"
# The owner is told that the clipboard is its own, and nothing else.
if grep -q '^owner: \[..*\]' "$log.offer"; then
    fail "the owner was told about its own offer"
fi
echo "PASS: session reads what we offer"

# selection-tool -> session: someone else copies; the owner loses it, the
# watcher and a reader see it.
other="the session's own: €"
printf '%s' "$other" | wl-copy --type "text/plain;charset=utf-8"
if ! got=$(timeout 10 "$tool" read "text/plain;charset=utf-8" 2>"$log.read"); then
    fail "nothing read"
fi
[ "$got" = "$other" ] || fail "read '$got', wanted '$other'"
grep -q '^owner: \[text/plain;charset=utf-8' "$log.offer" ||
    fail "the old owner did not see the new one"
echo "PASS: we read what the session offers"

# The compositor goes and comes back: the watcher finds it again.
kill "${pids[-1]}"
wait "${pids[-1]}" 2>/dev/null || true
kwin_wayland --virtual --no-lockscreen --socket wayland-test --width 640 --height 480 \
    >>"$log.kwin" 2>&1 &
pids+=($!)
printf '%s' "$text" | wl-copy --type text/plain 2>>"$log.paste" ||
    { sleep 1; printf '%s' "$text" | wl-copy --type text/plain; }
came_back() { sed -n '/lost the connection/,$p' "$log.watch" | grep -q '^owner: \[text/plain'; }
for _ in $(seq 100); do
    came_back && break
    sleep 0.1
done
came_back || fail "the watcher did not come back"
echo "PASS: a new compositor is found"
