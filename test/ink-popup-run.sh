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
#  5  the cursor moves to another line: the popup moves down with it but
#     keeps its x (centred on the window)
#  6  text input off / unmap / map / destroy: its rect goes and comes back,
#     the window gets its region back
#  7  a layer surface (bottom edge) with an ink area: APP = its namespace,
#     cuts the window
#  8  a popup for a text field on the layer surface (centred on the output,
#     flipped above the cursor, no room below); a click on it keeps the
#     layer's focus
#  9  a text field without a cursor rectangle (Chromium), its window shrunk
#     by a keyboard-like layer's exclusive zone: the popup lies
#     over the window's top; the zone goes (the window grows), the popup
#     commits (ink): it stays where it was, and the next press reaches it
# 10  writing regions (ink-region-client.c, a window without ink areas of its
#     own): a region in ink mode is an ink area (S 1, a brush with the eraser
#     off), nothing else of the window is; it follows a move without a new
#     frame, its clip, a text mode region above it cuts it, text mode and
#     destroy take it away, the app's own areas add to it, many regions fit
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
        protocols/wlr-layer-shell-unstable-v1.xml protocols/glassine-ink-v1.xml \
        protocols/glassine-handwriting-v1.xml; do
    n=$(basename "$x" .xml)
    wayland-scanner client-header "$x" $G/$n-client-protocol.h
    wayland-scanner private-code "$x" $G/$n-protocol.c
done
gcc -std=c11 -O2 -Wall -Werror -I$G -o /tmp/client test/ink-popup-client.c $(ls $G/*-protocol.c | grep -v handwriting) \
    -lwayland-client
gcc -std=c11 -O2 -Wall -Werror -I$G -o /tmp/rclient test/ink-region-client.c $G/xdg-shell-protocol.c \
    $G/glassine-ink-v1-protocol.c $G/glassine-handwriting-v1-protocol.c -lwayland-client

export XDG_RUNTIME_DIR=/tmp/rt
mkdir -p -m 700 $XDG_RUNTIME_DIR
F=/run/glassine-epdd-desktop.ctl
rm -f $F /out/*.log
mkfifo $F
cat 0<>$F >/out/ink-fifo.log &      # read-write: the FIFO always has a reader
printf "output HEADLESS-1 mode 800x600 scale 2\ndefault_border none\n" >/tmp/sway.conf
GLASSINE_HANDWRITING_SERVICE= WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 $SWAY -c /tmp/sway.conf -d >/out/sway.log 2>&1 &
SW=$!
i=0
while [ ! -S $XDG_RUNTIME_DIR/wayland-1 ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
mkfifo /tmp/cmd
WAYLAND_DISPLAY=wayland-1 /tmp/client </tmp/cmd >/out/client.log 2>&1 &
CL=$!
exec 3>/tmp/cmd

FAIL=0
RC=
finish() {
    exec 3>&-
    exec 4>&- 2>/dev/null || true
    kill $CL $RC $SW 2>/dev/null || true
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
# lines the client printed since mark
mark() { MARK=$(wc -l </out/client.log); }
wait_new() {
    i=0
    while ! tail -n +$((MARK + 1)) /out/client.log | grep -q -- "$1"; do
        i=$((i + 1))
        if [ $i -gt 50 ]; then echo "FAIL: no new '$1' from the client"; FAIL=1; tail -5 /out/client.log; finish; fi
        sleep 0.1
    done
}
# APP's first rect in the last block: "X Y W H" (panel px)
rect() {
    block | awk -v app="$1" '$1 == "ink" && $2 == "r" && $9 == app { print $5, $6, $7, $8; exit }'
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
# 2: cursor 20,40 2x20 -> popup 160x80 centred on the 400 wide window, below
# the cursor: 120,60 = panel 240,120 320x160; it covers 80x80 of the ink area
send "ti 20 40"
wait_log "^im-activate"
send "popup 0 160 80 1"
expect input-method 51200 "2 popup's ink area"
expect_line "^ink r [0-9]* 1 240 120 320 160 input-method\$" "2 popup's rect, S 1, with a brush, at the popup"
expect ink-test 214400 "2 the popup cuts the window"
# 3: the pointer on the popup (popup-local 80,40), a drag, a release
send "ptr 200 100"
wait_log "^ptr-enter popup0 80 40"
send "down"
send "ptr 220 110"
send "up"
wait_log "^ptr-button popup0 up"
expect_log "^ptr-button popup0 down" "3 the press reaches the popup"
expect_log "^ptr-motion popup0 100 50" "3 the drag reaches the popup"
expect_no_log "^ti-leave" "3 the text field keeps the focus"
expect input-method 51200 "3 the popup stays"
# 4: a second popup above, 60x30 (centred: 170,60, inside the first), no areas
send "popup 1 60 30 0"
expect input-method 44000 "4 a popup above cuts the first; one without areas adds none"
send "kill 1"
expect input-method 51200 "4 back when it goes"
# 5: cursor to 100,150: popup at 120,170 = panel 240,340 (same x as before)
send "cur 100 150"
send "show 0"
expect_line "^ink r [0-9]* 1 240 340 320 160 input-method\$" "5 the rect follows the cursor's line, not its x"
expect ink-test 214400 "5 the window's hole moves too"
# 6: the field loses the text input with the popup still mapped
send "tioff"
wait_log "^im-deactivate"
expect input-method 0 "6 text input off: no rect"
expect ink-test 240000 "6 text input off: the window's region is whole again"
send "ti 100 150"
send "show 0"
expect input-method 51200 "6 text input on again: the rect is back"
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
# no room below (y 230 + 80 > 300) -> the popup goes above, centred on the
# output: 120,130
send "ptr 50 250"
send "down"
send "up"
wait_log "^ti-enter layer"
send "ti 10 10"
send "popup 2 160 80 1"
expect input-method 51200 "8 popup over a layer surface"
expect_line "^ink r [0-9]* 1 240 260 320 160 input-method\$" "8 its rect, above the cursor"
expect ink-test-layer 153600 "8 the popup cuts the layer surface"
expect ink-test 137600 "8 and the window"
send "ptr 160 150"
wait_log "^ptr-enter popup2 40 20"
send "down"
send "up"
wait_log "^ptr-button popup2 up"
expect_log "^ptr-button popup2 down" "8 the press reaches the popup"
expect_no_log "^ti-leave layer" "8 a click on the popup keeps the layer's focus"
expect input-method 51200 "8 the popup stays"
# 9: the window shrinks to 400x200 (a keyboard-like layer, zone 100), a field without
# a cursor rectangle gets the focus; the popup 160x80 lies over the window's
# top, centred: 120,0 = panel 240,0 320x160
send "kill 2"
send "tioff"
mark
send "unlayer"
wait_new "^ti-enter win"
send "osk 100"
wait_new "^win-size 400x200"
mark
send "tinr"
wait_new "^im-activate"
send "popup 3 160 80 1"
expect_line "^ink r [0-9]* 1 240 0 320 160 input-method\$" "9 no cursor rectangle: over the window's top"
R9=$(rect input-method)
mark
send "unosk"
wait_new "^win-size 400x300"
send "show 3"
expect_line "^ink r [0-9]* 1 240 0 320 160 input-method\$" "9 the window grew, the popup committed: it stays"
[ "$(rect input-method)" = "$R9" ] || { echo "FAIL: 9 moved: $R9 -> $(rect input-method)"; FAIL=1; }
# the pen where the popup was first put (a quarter down into it)
set -- $R9
mark
send "ptr $((($1 + $3 / 2) / 2)) $((($2 + $4 / 4) / 2))"
wait_new "^ptr-enter "
send "down"
send "up"
wait_new "^ptr-button popup3 up"
if tail -n +$((MARK + 1)) /out/client.log | grep -q "^ptr-button popup3 down"; then echo "ok: 9 the next press reaches the popup"; else echo "FAIL: 9 the next press missed the popup"; FAIL=1; fi
if tail -n +$((MARK + 1)) /out/client.log | grep -q "^ti-leave\|^im-deactivate"; then echo "FAIL: 9 the field lost the focus"; FAIL=1; else echo "ok: 9 the field keeps the focus"; fi

# 10: writing regions. The first client goes (its window, popups and layers with it);
# the region client's window then has all of the output: 400x300 layout = 800x600 panel
exec 3>&-
kill $CL 2>/dev/null || true
mkfifo /tmp/rcmd
WAYLAND_DISPLAY=wayland-1 /tmp/rclient </tmp/rcmd >/out/rclient.log 2>&1 &
RC=$!
exec 4>/tmp/rcmd
rwait() {
    i=0
    while ! grep -q -- "$1" /out/rclient.log; do
        i=$((i + 1))
        if [ $i -gt 50 ]; then echo "FAIL: no '$1' from the region client"; FAIL=1; tail -5 /out/rclient.log; finish; fi
        sleep 0.1
    done
}
rsend() {
    echo "$1" >&4
    rwait "^ok $1\$"
}
# the number of APP's rect lines in the last block (with a brush or without)
lines() {
    block | awk -v app="$1" '$1 == "ink" && $2 == "r" { if ($9 == app) n++; next }
        $1 == "ink" && $2 != "brush" && $7 == app { n++ } END { print n + 0 }'
}
rwait "^ready"
rsend "win 400 300"
expect ink-test 0 "10 the first client's window is gone"
rsend "commit"
expect_line "^ink 0 0 0 800 600 ink-region\$" "10 no region yet: the whole window, S 0 (inks only with ink=on)"
rsend "r 0 10 10 100 50"
expect ink-region 20000 "10 a region in ink mode: an ink area, alone"
expect_line "^ink r [0-9]* 1 20 20 200 100 ink-region\$" "10 its rect, S 1, at the region"
expect_line "^ink brush [0-9]* 0 0.00 0.00 1.000 1 0.00 0.00 1.000\$" "10 the regions' brush: the app's tip, the eraser off"
rsend "r 0 50 60 100 50"
expect_line "^ink r [0-9]* 1 100 120 200 100 ink-region\$" "10 a move without a new frame: the rect follows"
rsend "clip 0 50 60 50 50"
expect ink-region 10000 "10 its clip"
rsend "clip 0 none"
expect ink-region 20000 "10 no clip: the whole rectangle"
rsend "r 1 50 60 50 25"
rsend "layer 1 1"
rsend "mode 1 text"
expect ink-region 15000 "10 a text mode region above cuts it"
rsend "mode 1 ink"
expect ink-region 20000 "10 both in ink mode: their union"
rsend "layer 1 -1"
rsend "mode 1 text"
expect ink-region 20000 "10 a text mode region below cuts nothing"
rsend "all text"
expect ink-region 0 "10 every region in text mode: no ink"
rsend "all ink"
expect ink-region 20000 "10 back in ink mode"
rsend "kill 0"
rsend "kill 1"
expect ink-region 0 "10 regions destroyed: no ink (the surface stays in region mode)"
rsend "r 2 10 10 100 50"
rsend "inkarea 300 0 100 100"
expect ink-region 60000 "10 the app's own areas add to the regions"
rsend "kill 2"
expect ink-region 40000 "10 the app's areas alone"
rsend "many 40"
expect ink-region 72000 "10 40 more regions: all inked"
[ "$(lines ink-region)" = 41 ] && echo "ok: 10 41 rects sent" || { echo "FAIL: 10 $(lines ink-region) rects, expected 41"; FAIL=1; }
finish
