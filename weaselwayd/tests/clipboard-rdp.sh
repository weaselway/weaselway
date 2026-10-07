#!/usr/bin/env bash
# The clipboard from one end to the other, without Windows and without a GPU:
#
#   xclip <-> Xvfb <-> xfreerdp == RDP ==> rdp-harness <-> mutter <-> selection-tool
#
# xfreerdp on an X server of its own plays the Windows side, a headless mutter
# is the session. With CLIPBOARD_TEST_COMPOSITOR=kwin a virtual KWin is.
#
# Text, HTML and an image are copied on each side and pasted on the other.
#
# Needs mutter or kwin_wayland, dbus-run-session, Xvfb, xclip, xfreerdp, and the
# two programs:
#   make tests/rdp-harness tests/selection-tool && tests/clipboard-rdp.sh
set -euo pipefail

here=$(dirname "$(readlink -f "$0")")

if [ -z "${CLIPBOARD_TEST_INNER:-}" ]; then
    XDG_RUNTIME_DIR=$(mktemp -d)
    export XDG_RUNTIME_DIR
    trap 'rm -rf "$XDG_RUNTIME_DIR"' EXIT
    # With this set KWin would run nested.
    unset WAYLAND_DISPLAY
    CLIPBOARD_TEST_INNER=1 dbus-run-session \
        ${DBUS_SESSION_CONF:+--config-file="$DBUS_SESSION_CONF"} -- "$0"
    exit
fi

port=${CLIPBOARD_TEST_PORT:-33890}
display=:${CLIPBOARD_TEST_DISPLAY:-97}
log=$XDG_RUNTIME_DIR/log
pids=()
trap 'kill "${pids[@]}" 2>/dev/null || true' EXIT

fail() {
    echo "FAIL: $*" >&2
    tail -n 40 "$log".* >&2
    exit 1
}

# Until a command succeeds, for a few seconds.
retry() {
    for _ in $(seq 40); do
        "$@" 2>>"$log.retry" && return 0
        sleep 0.1
    done
    fail "$*"
}

case ${CLIPBOARD_TEST_COMPOSITOR:-mutter} in
    mutter)
        mutter --headless --wayland --no-x11 --virtual-monitor 640x480 >"$log.compositor" 2>&1 &
        ;;
    kwin)
        kwin_wayland --virtual --no-lockscreen --width 640 --height 480 >"$log.compositor" 2>&1 &
        ;;
    *)
        echo "CLIPBOARD_TEST_COMPOSITOR is mutter or kwin" >&2
        exit 2
        ;;
esac
pids+=($!)
Xvfb "$display" -screen 0 800x600x24 >"$log.xvfb" 2>&1 &
pids+=($!)
"$here/rdp-harness" "$port" >"$log.harness" 2>&1 &
pids+=($!)

harness_says() { grep -q "$1" "$log.harness"; }
retry harness_says "sharing the session's clipboard"

# A client without gfxredir is dropped after five seconds, so every case gets
# a connection of its own.
client=
connect() {
    local before
    if [ -n "$client" ]; then
        kill "$client" 2>/dev/null || true
        wait "$client" 2>/dev/null || true
    fi
    before=$(grep -c "clipboard channel ready" "$log.harness" || true)
    DISPLAY=$display xfreerdp /v:127.0.0.1:"$port" /u:test /p:test /cert:ignore /size:800x600 \
        +clipboard /sec:tls >>"$log.client" 2>&1 &
    client=$!
    pids+=("$client")
    for _ in $(seq 100); do
        [ "$(grep -c "clipboard channel ready" "$log.harness" || true)" -gt "$before" ] && return 0
        sleep 0.1
    done
    fail "no client with a clipboard channel"
}

# The two clipboards: xclip is the client's, selection-tool the session's.
client_copy() { DISPLAY=$display xclip -selection clipboard -t "$1" -i "$2"; }
client_paste() { DISPLAY=$display timeout 3 xclip -selection clipboard -t "$1" -o >"$2"; }
session_paste() { timeout 3 "$here/selection-tool" read "$1" >"$2"; }
owner=
session_copy() {
    if [ -n "$owner" ]; then
        kill "$owner" 2>/dev/null || true
        wait "$owner" 2>/dev/null || true
    fi
    "$here/selection-tool" offer "$1" "$2" 2>>"$log.offer" &
    owner=$!
    pids+=("$owner")
}

same() { "$1" "$2" "$out" && cmp -s "$out" "$3"; }
contains() { "$1" "$2" "$out" && grep -qF "$3" "$out"; }
is_png() { "$1" "$2" "$out" && [ "$(head -c 8 "$out" | od -An -tx1 | tr -d ' \n')" = 89504e470d0a1a0a ]; }

out=$XDG_RUNTIME_DIR/out
text=$XDG_RUNTIME_DIR/text
html=$XDG_RUNTIME_DIR/html
png=$XDG_RUNTIME_DIR/png
bmp=$XDG_RUNTIME_DIR/bmp
printf 'two lines, and not only ASCII: äöü €\nthe second' >"$text"
printf '<b>bold</b> and <i>äöü</i>' >"$html"
# Three pixels by two, and the 24 bit bitmap that is made of it.
base64 -d >"$png" <<'PNG'
iVBORw0KGgoAAAANSUhEUgAAAAMAAAACCAIAAAASFvFNAAAAFklEQVR4nGP4z8DAAMMM////5xKRAwA8PAY3BUge8wAAAABJRU5ErkJggg==
PNG
base64 -d >"$bmp" <<'BMP'
Qk1OAAAAAAAAADYAAAAoAAAAAwAAAAIAAAABABgAAAAAABgAAADEDgAAxA4AAAAAAAAAAAAAAAAA////HhQKAAAAAAD/AP8A/wAAAAAA
BMP

connect
client_copy UTF8_STRING "$text"
retry same session_paste "text/plain;charset=utf-8" "$text"
echo "PASS: text, client -> session"

connect
client_copy text/html "$html"
retry contains session_paste text/html "$(cat "$html")"
echo "PASS: HTML, client -> session"

connect
client_copy image/bmp "$bmp"
retry same session_paste image/bmp "$bmp"
retry is_png session_paste image/png
echo "PASS: image, client -> session"

session_copy "text/plain;charset=utf-8" "$text"
connect
retry same client_paste UTF8_STRING "$text"
echo "PASS: text, session -> client"

session_copy text/html "$html"
connect
retry contains client_paste text/html "$(cat "$html")"
echo "PASS: HTML, session -> client"

session_copy image/png "$png"
connect
retry same client_paste image/bmp "$bmp"
echo "PASS: image, session -> client"
