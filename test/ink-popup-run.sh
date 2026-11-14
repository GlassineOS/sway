#!/bin/sh
# The container half of ink-popup-test.sh (Alpine 3.24, as root). Output
# 800x600 at scale 2: layout 400x300, every layout px = 4 panel px.
# The cases:
#  1  a window (tiled, all of the output) with an ink area on its left half
#  2  an input-method popup with an ink area under the text field's cursor:
#     its own rect (S 1, a brush, APP input-method), cut out of the window's
#  3  the pointer (the pen as sway's emulated pointer) on the popup: enter,
#     press, drag, release reach the popup; the text field keeps its focus
#  4  a second popup above it, with an ink object but no areas: emits nothing,
#     cuts the first
#  5  the cursor moves: the popup's rect moves with it
#  6  unmap / map / destroy: its rect goes and comes back, the window gets its
#     region back
#  7  a layer surface (bottom edge) with an ink area: APP = its namespace,
#     cuts the window
#  8  a popup for a text field on the layer surface (flipped above the
#     cursor, no room below); a click on it keeps the layer's focus
set -eu
apk add -q build-base meson ninja pkgconf basu-dev cairo-dev eudev-dev gdk-pixbuf-dev json-c-dev libevdev-dev \
    libinput-dev libxkbcommon-dev linux-pam-dev pango-dev pcre2-dev wayland-dev wayland-protocols curl \
    librsvg-dev glib-dev libxres-dev linux-headers libdrm-dev
cp -r /s /tmp/t
cd /tmp/t
rm -rf build subprojects out-arm64 build-test
WLR=$(sh wlroots/setup.sh /tmp/t)
if [ -e /sway-bin ]; then
    SWAY=/sway-bin
else
    # shellcheck disable=SC2086
    meson setup -Dsd-bus-provider=basu -Dman-pages=disabled -Dswaybar=false -Dswaynag=false $WLR build >/dev/null
    ninja -C build sway/sway >/tmp/ninja.log 2>&1 || { tail -30 /tmp/ninja.log; exit 1; }
    grep -i "warning" /tmp/ninja.log | grep -i glassine || true
    SWAY=/tmp/t/build/sway/sway
fi

G=/tmp/gen
mkdir -p $G
WP=/usr/share/wayland-protocols
for x in $WP/stable/xdg-shell/xdg-shell.xml $WP/unstable/text-input/text-input-unstable-v3.xml \
        subprojects/wlroots/protocol/input-method-unstable-v2.xml \
        subprojects/wlroots/protocol/wlr-virtual-pointer-unstable-v1.xml \
        protocols/wlr-layer-shell-unstable-v1.xml protocols/glassine-ink-v1.xml; do
    n=$(basename "$x" .xml)
    wayland-scanner client-header "$x" $G/$n-client-protocol.h
    wayland-scanner private-code "$x" $G/$n-protocol.c
done
gcc -std=c11 -O2 -Wall -Werror -I$G -o /tmp/client test/ink-popup-client.c $G/*-protocol.c -lwayland-client

export XDG_RUNTIME_DIR=/tmp/rt
mkdir -p -m 700 $XDG_RUNTIME_DIR
F=/run/glassine-epdd-desktop.ctl
rm -f $F /out/*.log
mkfifo $F
cat 0<>$F >/out/ink-fifo.log &      # read-write: the FIFO always has a reader
printf "output HEADLESS-1 mode 800x600 scale 2\ndefault_border none\n" >/tmp/sway.conf
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 $SWAY -c /tmp/sway.conf -d >/out/sway.log 2>&1 &
SW=$!
i=0
while [ ! -S $XDG_RUNTIME_DIR/wayland-1 ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
mkfifo /tmp/cmd
WAYLAND_DISPLAY=wayland-1 /tmp/client </tmp/cmd >/out/client.log 2>&1 &
CL=$!
exec 3>/tmp/cmd

FAIL=0
finish() {
    exec 3>&-
    kill $CL $SW 2>/dev/null || true
    chown -R "$HOST_UID" /out
    if [ $FAIL -eq 0 ]; then echo "ink-popup: all cases passed"; else echo "ink-popup: FAILED"; fi
    exit $FAIL
}

# wait (5 s at most) until the client printed a line matching $1
wait_log() {
    i=0
    while ! grep -q -- "$1" /out/client.log; do
        i=$((i + 1))
        if [ $i -gt 50 ]; then echo "FAIL: no '$1' from the client"; FAIL=1; tail -5 /out/client.log; finish; fi
        sleep 0.1
    done
}
send() {
    echo "$1" >&3
    wait_log "^ok $1\$"
}
# the last complete ink block sway sent
block() {
    awk '/^ink begin$/ { b = ""; inb = 1; next } /^ink end$/ { if (inb) last = b; inb = 0; next }
        inb { b = b $0 "\n" } END { printf "%s", last }' /out/ink-fifo.log
}
# panel px of APP's rects in the last block
area() {
    block | awk -v app="$1" '$1 == "ink" && $2 == "r" { if ($9 == app) a += $7 * $8; next }
        $1 == "ink" && $2 != "brush" { if ($7 == app) a += $5 * $6 } END { print a + 0 }'
}
# check: APP's area becomes N within 3 s (sway sends after a frame)
expect() {
    i=0
    while [ "$(area "$1")" != "$2" ]; do
        i=$((i + 1))
        if [ $i -gt 30 ]; then
            echo "FAIL: $3: $1 has $(area "$1") panel px, expected $2"; FAIL=1
            block | sed 's/^/    /'
            return
        fi
        sleep 0.1
    done
    echo "ok: $3 ($1 $2 px)"
}
# check: the last block has a line matching $1 within 3 s
expect_line() {
    i=0
    while ! block | grep -q -- "$1"; do
        i=$((i + 1))
        if [ $i -gt 30 ]; then echo "FAIL: $2: no line '$1'"; FAIL=1; block | sed 's/^/    /'; return; fi
        sleep 0.1
    done
    echo "ok: $2"
}
expect_log() {
    if grep -q -- "$1" /out/client.log; then echo "ok: $2"; else echo "FAIL: $2: the client got no '$1'"; FAIL=1; fi
}
expect_no_log() {
    if grep -q -- "$1" /out/client.log; then echo "FAIL: $2: the client got '$1'"; FAIL=1; else echo "ok: $2"; fi
}

wait_log "^ready 800x600 scale 2"
# 1: left half 200x300 layout = 400x600 panel px
send "win 400 300"
wait_log "^ti-enter win"
expect ink-test 240000 "1 window's ink area"
expect_line "^ink r [0-9]* 1 0 0 400 600 ink-test\$" "1 window's rect with its brush, S 1"
# 2: cursor 20,40 2x20 -> popup 160x80 at 20,60 = panel 40,120 320x160
send "ti 20 40"
wait_log "^im-activate"
send "popup 0 160 80 1"
expect input-method 51200 "2 popup's ink area"
expect_line "^ink r [0-9]* 1 40 120 320 160 input-method\$" "2 popup's rect, S 1, with a brush, at the popup"
expect ink-test 188800 "2 the popup cuts the window"
# 3: the pointer on the popup (popup-local 80,40), a drag, a release
send "ptr 100 100"
wait_log "^ptr-enter popup0 80 40"
send "down"
send "ptr 120 110"
send "up"
wait_log "^ptr-button popup0 up"
expect_log "^ptr-button popup0 down" "3 the press reaches the popup"
expect_log "^ptr-motion popup0 100 50" "3 the drag reaches the popup"
expect_no_log "^ti-leave" "3 the text field keeps the focus"
expect input-method 51200 "3 the popup stays"
# 4: a second popup above, 60x30 at the same place, no areas
send "popup 1 60 30 0"
expect input-method 44000 "4 a popup above cuts the first; one without areas adds none"
send "kill 1"
expect input-method 51200 "4 back when it goes"
# 5: cursor to 100,150: popup at 100,170 = panel 200,340
send "cur 100 150"
send "show 0"
expect_line "^ink r [0-9]* 1 200 340 320 160 input-method\$" "5 the rect follows the popup"
expect ink-test 208000 "5 the window's hole moves too"
# 6
send "hide 0"
expect input-method 0 "6 unmapped: no rect"
expect ink-test 240000 "6 unmapped: the window's region is whole again"
send "show 0"
expect input-method 51200 "6 mapped again"
send "kill 0"
expect input-method 0 "6 destroyed: no rect"
expect ink-test 240000 "6 destroyed: the window's region is whole again"
# 7: layer 400x100 at the bottom (layout y 200)
send "tioff"
send "layer 100"
expect ink-test-layer 160000 "7 layer surface's ink area (APP = its namespace)"
expect ink-test 160000 "7 the layer surface cuts the window"
# 8: focus the layer (on demand: a click), a text field at 10,10 on it:
# no room below (y 230 + 80 > 300) -> the popup goes above: 10,130
send "ptr 50 250"
send "down"
send "up"
wait_log "^ti-enter layer"
send "ti 10 10"
send "popup 2 160 80 1"
expect input-method 51200 "8 popup over a layer surface"
expect_line "^ink r [0-9]* 1 20 260 320 160 input-method\$" "8 its rect, above the cursor"
expect ink-test-layer 153600 "8 the popup cuts the layer surface"
expect ink-test 115200 "8 and the window"
send "ptr 50 150"
wait_log "^ptr-enter popup2 40 20"
send "down"
send "up"
wait_log "^ptr-button popup2 up"
expect_log "^ptr-button popup2 down" "8 the press reaches the popup"
expect_no_log "^ti-leave layer" "8 a click on the popup keeps the layer's focus"
expect input-method 51200 "8 the popup stays"
finish
