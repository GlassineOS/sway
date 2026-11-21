#!/bin/sh
# The container half of ui-scale-test.sh (Alpine 3.24, as root). A headless
# output of 1404x1872 at the scales 1.6, 1.8, 2.4, 0.8 and 2, each with a
# whole-scale-only client and a fractional one (ui-scale-client.c). The cases,
# for every scale and both clients:
#  1  the scale the client is told: wl_output.scale and the surface's preferred
#     buffer scale are the nearest whole number N (not the next one up); a
#     fractional client gets the output's scale in 120ths
#  2  a tiled window over the whole output: its configure size (whole: the
#     output's pixels over N; fractional: the layout size) and its buffer on the
#     output byte for byte
#  3  floating at a place and size whose box is no whole number of output
#     pixels: configured, and shown pixel for pixel from the box's first pixel;
#     the place is one where a fractional client's buffer (round(size x scale))
#     is a pixel wider than its box and a pixel less high: the extra column is
#     cut, the last row is drawn twice, nothing is scaled
#  4  a commit that changes a few pixels with only their box as damage shows
#  5  a subsurface and an xdg popup: where they are asked for, pixel for pixel
#  6  the pointer: positions on the popup and the window in the client's own
#     coordinates, also while a button is held (a drag)
# At scale 2 a client sees what it always did: scale 2, layout sizes, two
# output pixels per surface unit. With a second sway (/base-bin: one built from
# the tree before a change) the scale 2 cases run on both, and what the clients
# were told and three screenshots of the whole output must be the same.
# Then, once:
#  7  a window that takes one size only (it floats): it keeps it, and is not
#     configured again and again
#  8  two outputs of scales 1.6 and 2: a window moved from one to the other and
#     back gets the other N (or fractional scale), size and pixels
#  9  the output's scale changed while the window is shown (1.6 -> 1.8 -> 0.8)
# 10  get_outputs carries the reported physical size
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
for x in $WP/stable/xdg-shell/xdg-shell.xml $WP/stable/viewporter/viewporter.xml \
        $WP/staging/fractional-scale/fractional-scale-v1.xml \
        subprojects/wlroots/protocol/wlr-virtual-pointer-unstable-v1.xml \
        subprojects/wlroots/protocol/wlr-screencopy-unstable-v1.xml; do
    n=$(basename "$x" .xml)
    wayland-scanner client-header "$x" $G/$n-client-protocol.h
    wayland-scanner private-code "$x" $G/$n-protocol.c
done
# shellcheck disable=SC2046
gcc -std=c11 -O2 -Wall -Werror -I$G -o /tmp/client test/ui-scale-client.c $(ls $G/*-protocol.c) -lwayland-client

rm -f /out/*.log /out/*.xrgb
FAIL=0
RUN=0
SW=
CL=
LOG=

stop() {
    exec 3>&-
    [ -n "$CL" ] && kill "$CL" 2>/dev/null || true
    [ -n "$SW" ] && kill "$SW" 2>/dev/null || true
    [ -n "$SW" ] && wait "$SW" 2>/dev/null || true
    SW=
    CL=
}
finish() {
    stop
    chown -R "$HOST_UID" /out
    if [ $FAIL -eq 0 ]; then echo "ui-scale: all cases passed"; else echo "ui-scale: FAILED"; fi
    exit $FAIL
}
# start NAME CONFIG-LINES: a fresh sway and client; the logs are /out/NAME-*.log
start() {
    stop
    RUN=$((RUN + 1))
    export XDG_RUNTIME_DIR=/tmp/rt$RUN
    rm -rf $XDG_RUNTIME_DIR
    unset SWAYSOCK      # sway would take its socket's path from it
    mkdir -p -m 700 $XDG_RUNTIME_DIR
    printf "%b\ndefault_border none\ndefault_floating_border none\n" "$2" >/tmp/sway.conf
    GLASSINE_HANDWRITING_SERVICE='' WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
        WLR_HEADLESS_OUTPUTS="${3:-1}" $SWAY -c /tmp/sway.conf -d >"/out/$1-sway.log" 2>&1 &
    SW=$!
    i=0
    while { [ ! -S $XDG_RUNTIME_DIR/wayland-1 ] || ! ls $XDG_RUNTIME_DIR/sway-ipc.*.sock >/dev/null 2>&1; } &&
            [ $i -lt 100 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    SWAYSOCK=$(ls $XDG_RUNTIME_DIR/sway-ipc.*.sock)
    export SWAYSOCK
    LOG=/out/$1-client.log
    rm -f /tmp/cmd
    mkfifo /tmp/cmd
    WAYLAND_DISPLAY=wayland-1 /tmp/client </tmp/cmd >"$LOG" 2>&1 &
    CL=$!
    exec 3>/tmp/cmd
    MARK=0
    wait_new "^ready"
}
mark() { MARK=$(wc -l <"$LOG"); }
new() { tail -n +$((MARK + 1)) "$LOG"; }
# wait (5 s at most) for a line matching $1 after the mark
wait_new() {
    i=0
    while ! new | grep -q -- "$1"; do
        i=$((i + 1))
        if [ $i -gt 50 ]; then echo "FAIL: no '$1' from the client ($LOG)"; FAIL=1; tail -8 "$LOG"; finish; fi
        sleep 0.1
    done
}
# send a command and wait until the compositor has seen it; the mark is before it
send() {
    mark
    echo "$1" >&3
    wait_new "^ok $1\$"
}
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1"; FAIL=1; }
# expect LABEL REGEX: a line since the start of this client, within 3 s
expect() {
    i=0
    while ! grep -q -- "$2" "$LOG"; do
        i=$((i + 1))
        if [ $i -gt 30 ]; then bad "$1: no '$2'"; tail -6 "$LOG" | sed 's/^/    /'; return; fi
        sleep 0.1
    done
    ok "$1"
}
# the last line matching $1
last() { grep -- "$1" "$LOG" | tail -1; }
# check LABEL OUT SURFACE X Y W H: the surface's first WxH buffer pixels are the
# output's from X Y (retried for 3 s: sway draws after its transaction)
check() {
    i=0
    while :; do
        send "check $2 $3 $4 $5 $6 $7"
        if new | grep -q "^check $2 $3 ok "; then ok "$1 ($3 at $4,$5 ${6}x$7)"; return; fi
        i=$((i + 1))
        if [ $i -gt 15 ]; then bad "$1: $(new | grep '^check ')"; return; fi
        sleep 0.2
    done
}
# pixel LABEL OUT X Y RRGGBB
pixel() {
    i=0
    while :; do
        send "px $2 $3 $4"
        if new | grep -q "^px $2 $3 $4 $5\$"; then ok "$1"; return; fi
        i=$((i + 1))
        if [ $i -gt 15 ]; then bad "$1: $(new | grep '^px '), expected $5"; return; fi
        sleep 0.2
    done
}
# calc EXPR: awk arithmetic; r(v) rounds, c(a, b) is ceil(a / b), dev(x, w, s)
# is the output pixels of a layout box edge as the scene scales it, off(w, s) the
# first place where they are not round(w x s), pat(id, x, y) the client's pixel
calc() {
    awk "function r(v) { return int(v + 0.5) } function c(a, b) { return int((a + b - 1) / b) }
        function dev(x, w, s) { return r((x + w) * s) - r(x * s) } function min(a, b) { return a < b ? a : b }
        function off(w, s,   i) { for (i = 1; i < 60; i++) if (r(w * s) != dev(i, w, s)) return i; return 7 }
        function pat(id, x, y) { return sprintf(\"%s%02x%02x\", (x + y) % 2 ? \"ff\" : \"00\", (x * 5 + id * 40) % 256, (y * 3) % 256) }
        BEGIN { print $1 }"
}
# pointer LABEL SURFACE EVENT X Y: the last ptr event is on SURFACE at X Y (+- 0.02)
pointer() {
    i=0
    while :; do
        l=$(new | grep "^$3 " | tail -1)
        # shellcheck disable=SC2086
        set -- "$1" "$2" "$3" "$4" "$5" $l
        if [ "${7:-}" = "$2" ] && [ "$(calc "($8 - $4 < 0.02 && $4 - $8 < 0.02 && $9 - $5 < 0.02 && $5 - $9 < 0.02)")" = 1 ]; then
            ok "$1 ($2 $8 $9)"; return
        fi
        i=$((i + 1))
        if [ $i -gt 30 ]; then bad "$1: '$l', expected $3 $2 $4 $5"; return; fi
        sleep 0.1
    done
}

OW=1404
OH=1872
# shot N: with a second sway to compare with, keep a screenshot of the output
shot() {
    [ -z "$KEEP" ] || send "dump HEADLESS-1 /out/$KEEP-$1.xrgb"
}
# one scale, one kind of client; KEEP: a name to keep its screenshots under
scale_case() {
    S=$1
    M=$2
    KEEP=${3:-}
    T="$S $M:"
    N=$(calc "r($S) < 1 ? 1 : r($S)")
    K=$(calc "r($S * 120)")
    start "${KEEP:-s$S-$M}" "output HEADLESS-1 mode ${OW}x${OH} scale $S"
    # the output's layout size, as sway cuts it to whole layout px
    send "layout HEADLESS-1"
    LW=$(new | sed -n 's/^layout HEADLESS-1 0 0 \([0-9]*\)x.*/\1/p')
    LH=$(new | sed -n 's/^layout HEADLESS-1 0 0 [0-9]*x\([0-9]*\)$/\1/p')
    [ "$(calc "($LW <= $OW / $S && $LW > $OW / $S - 1.001 && $LH <= $OH / $S && $LH > $OH / $S - 1.001)")" = 1 ] ||
        bad "$T the output's layout size ${LW}x$LH"
    expect "$T 1 wl_output.scale is $N" "^output HEADLESS-1 ${OW}x${OH} scale $N\$"
    send "win $M 200 200"
    # 2: tiled over the whole output; its box in output pixels
    DW=$(calc "dev(0, $LW, $S)")
    DH=$(calc "dev(0, $LH, $S)")
    if [ "$M" = whole ]; then
        UW=$(calc "c($DW, $N)")
        UH=$(calc "c($DH, $N)")
        expect "$T 2 configured ${UW}x$UH, a buffer of $((UW * N))x$((UH * N))" \
            "^size win ${UW}x$UH buffer $((UW * N))x$((UH * N)) scale $N\$"
        expect "$T 1 the preferred buffer scale is $N" "^preferred win $N\$"
        if grep "^preferred win " "$LOG" | grep -qv "^preferred win $N\$"; then bad "$T 1 another preferred scale"; fi
    else
        BW=$(calc "r($LW * $S)")
        BH=$(calc "r($LH * $S)")
        expect "$T 1 the fractional scale is $K/120" "^frac win $K\$"
        expect "$T 2 configured ${LW}x$LH, a buffer of ${BW}x$BH" "^size win ${LW}x$LH buffer ${BW}x$BH scale $K\$"
    fi
    check "$T 2 tiled, pixel for pixel" HEADLESS-1 win 0 0 "$DW" "$DH"
    shot tiled
    # 3: floating; a place where round(size x scale) is not the box's pixels
    FW=333
    FH=222
    FX=$(calc "off($FW, $S)")
    FY=$(calc "off($FH, $S)")
    send "ipcw floating enable"
    send "ipcw resize set $FW $FH"
    send "ipcw move position $FX $FY"
    DX=$(calc "r($FX * $S)")
    DY=$(calc "r($FY * $S)")
    DW=$(calc "dev($FX, $FW, $S)")
    DH=$(calc "dev($FY, $FH, $S)")
    if [ "$M" = whole ]; then
        UW=$(calc "c($DW, $N)")
        UH=$(calc "c($DH, $N)")
        BW=$((UW * N))
        BH=$((UH * N))
        expect "$T 3 floating ${FW}x$FH at $FX,$FY: configured ${UW}x$UH" "^size win ${UW}x$UH buffer ${BW}x$BH scale $N\$"
    else
        BW=$(calc "r($FW * $S)")
        BH=$(calc "r($FH * $S)")
        expect "$T 3 floating ${FW}x$FH at $FX,$FY: a buffer of ${BW}x$BH" "^size win ${FW}x$FH buffer ${BW}x$BH scale $K\$"
        if [ "$K" != 240 ]; then
            [ "$BW" != "$DW" ] && [ "$BH" != "$DH" ] && ok "$T 3 the buffer ${BW}x$BH is not the box ${DW}x$DH" ||
                bad "$T 3 no place where the buffer ${BW}x$BH differs from the box ${DW}x$DH"
        fi
    fi
    CW=$(calc "min($BW, $DW)")
    CH=$(calc "min($BH, $DH)")
    check "$T 3 floating, pixel for pixel" HEADLESS-1 win "$DX" "$DY" "$CW" "$CH"
    if [ "$BW" -lt "$DW" ]; then
        pixel "$T 3 a buffer one column short: its last column twice" HEADLESS-1 $((DX + BW)) "$DY" "$(calc "pat(0, $BW - 1, 0)")"
    fi
    if [ "$BH" -lt "$DH" ]; then
        pixel "$T 3 a buffer one row short: its last row twice" HEADLESS-1 "$DX" $((DY + BH)) "$(calc "pat(0, 0, $BH - 1)")"
    fi
    # 4: a few pixels change, with their box as the only damage
    send "dmg 101 61 7 9"
    check "$T 4 a small damage shows" HEADLESS-1 win "$DX" "$DY" "$CW" "$CH"
    # 5: a subsurface at 10,20 (31x17) and a popup at 30,50 (100x60)
    send "sub 10 20 31 17"
    if [ "$M" = whole ]; then
        check "$T 5 the subsurface" HEADLESS-1 sub $((DX + 10 * N)) $((DY + 20 * N)) $((31 * N)) $((17 * N))
    else
        check "$T 5 the subsurface" HEADLESS-1 sub "$(calc "r(($FX + 10) * $S)")" "$(calc "r(($FY + 20) * $S)")" \
            "$(calc "min(r(31 * $S), dev($FX + 10, 31, $S))")" "$(calc "min(r(17 * $S), dev($FY + 20, 17, $S))")"
    fi
    send "popup 30 50 100 60"
    expect "$T 5 the popup is placed at 30,50" "^popup-at 30 50 100x60\$"
    if [ "$M" = whole ]; then
        check "$T 5 the popup" HEADLESS-1 popup $((DX + 30 * N)) $((DY + 50 * N)) $((100 * N)) $((60 * N))
    else
        check "$T 5 the popup" HEADLESS-1 popup "$(calc "r(($FX + 30) * $S)")" "$(calc "r(($FY + 50) * $S)")" \
            "$(calc "min(r(100 * $S), dev($FX + 30, 100, $S))")" "$(calc "min(r(60 * $S), dev($FY + 50, 60, $S))")"
    fi
    shot popup
    # 6: the pointer, in thousandths of a layout px: on the popup, then (popup
    # and subsurface gone) on the window, and dragged
    ptr_case "$T 6 the pointer on the popup" popup 30 50 "ptr-\(enter\|motion\)" 71300 70000
    send "unpopup"
    send "unsub"
    ptr_case "$T 6 the pointer on the window" win 0 0 "ptr-\(enter\|motion\)" 150400 99600
    send "down"
    ptr_case "$T 6 a drag on the window" win 0 0 "ptr-\(enter\|motion\)" 31250 77125
    send "up"
    expect "$T 6 the button" "^ptr-button win up\$"
    shot end
}
# ptr_case LABEL SURFACE UX UY EVENT MX MY: the pointer MX, MY thousandths of a
# layout px into the floating box; SURFACE's origin is UX, UY in the window
ptr_case() {
    AX=$(calc "$FX * 1000 + $6")
    AY=$(calc "$FY * 1000 + $7")
    if [ "$M" = whole ]; then
        EX=$(calc "($AX / 1000 * $S - $DX) / $N - $3")
        EY=$(calc "($AY / 1000 * $S - $DY) / $N - $4")
    else
        EX=$(calc "$6 / 1000 - $3")
        EY=$(calc "$7 / 1000 - $4")
    fi
    mark
    echo "ptr $AX $AY $((LW * 1000)) $((LH * 1000))" >&3
    wait_new "^ok ptr $AX $AY "
    pointer "$1" "$2" "$5" "$EX" "$EY"
}

for S in 1.6 1.8 2.4 0.8 2; do
    for M in whole frac; do
        scale_case $S $M
    done
done

# scale 2 on this sway and on the one to compare with
if [ -e /base-bin ]; then
    NEW=$SWAY
    for M in whole frac; do
        scale_case 2 $M "new-$M"
        SWAY=/base-bin
        scale_case 2 $M "base-$M"
        SWAY=$NEW
        told() { grep "^output \|^configure \|^size \|^preferred \|^frac \|^popup-at \|^ptr-" "/out/$1-client.log"; }
        told "new-$M" >/tmp/told-new
        told "base-$M" >/tmp/told-base
        cmp -s /tmp/told-new /tmp/told-base && ok "2 $M: the client is told the same as before" ||
            bad "2 $M: the client is told something else than before"
        for n in tiled popup end; do
            cmp -s "/out/new-$M-$n.xrgb" "/out/base-$M-$n.xrgb" && ok "2 $M: the output is the same as before ($n)" ||
                bad "2 $M: the output differs from before ($n)"
        done
        rm -f /out/new-$M-*.xrgb /out/base-$M-*.xrgb
    done
fi

# 7: a window of one size only
start fixed "output HEADLESS-1 mode ${OW}x${OH} scale 1.6"
send "fixed 301 201"
send "win whole 0 0"
send "ipcw move position 9 5"
expect "7 a fixed size window keeps its size" "^size win 301x201 buffer 602x402 scale 2\$"
check "7 pixel for pixel" HEADLESS-1 win "$(calc "r(9 * 1.6)")" "$(calc "r(5 * 1.6)")" 602 402
sleep 0.5
C=$(grep -c "^configure " "$LOG")
[ "$C" -le 6 ] && ok "7 not configured over and over ($C configures)" || bad "7 $C configures"

# 8, 9: two outputs; HEADLESS-2 is 1000x800 at scale 2, right of the first
two_case() {
    M=$1
    T="two outputs $M:"
    start "two-$M" "output HEADLESS-1 mode ${OW}x${OH} scale 1.6 pos 0 0\noutput HEADLESS-2 mode 1000x800 scale 2 pos 2000 0\nfocus_follows_mouse no\nworkspace 1 output HEADLESS-1\nworkspace 2 output HEADLESS-2" 2
    send "ipc workspace 1"
    send "win $M 200 200"
    if [ "$M" = whole ]; then
        expect "$T 8 on the first: N 2, 702x936" "^size win 702x936 buffer 1404x1872 scale 2\$"
    else
        expect "$T 8 on the first: 192/120, 877x1170" "^size win 877x1170 buffer 1403x1872 scale 192\$"
    fi
    check "$T 8 on the first, pixel for pixel" HEADLESS-1 win 0 0 1403 1872
    send "ipcw move container to output HEADLESS-2"
    if [ "$M" = whole ]; then
        wait_new "^size win 500x400 buffer 1000x800 scale 2\$"
    else
        wait_new "^size win 500x400 buffer 1000x800 scale 240\$"
    fi
    ok "$T 8 moved to the second: 500x400 at scale 2"
    check "$T 8 on the second, pixel for pixel" HEADLESS-2 win 0 0 1000 800
    send "ipcw move container to output HEADLESS-1"
    if [ "$M" = whole ]; then
        wait_new "^size win 702x936 buffer 1404x1872 scale 2\$"
    else
        wait_new "^size win 877x1170 buffer 1403x1872 scale 192\$"
    fi
    ok "$T 8 and back"
    check "$T 8 back on the first, pixel for pixel" HEADLESS-1 win 0 0 1403 1872
    # 9: the first output's scale changes under the window
    send "ipc output HEADLESS-1 scale 1.8"
    if [ "$M" = whole ]; then
        # 780x1040 layout px = 1404x1872 output pixels, N stays 2: the same size
        # in units, and no new buffer needed
        sleep 0.3
    else
        wait_new "^size win 780x1040 buffer 1404x1872 scale 216\$"
    fi
    check "$T 9 scale 1.8, pixel for pixel" HEADLESS-1 win 0 0 1404 1872
    send "ipc output HEADLESS-1 scale 0.8"
    if [ "$M" = whole ]; then
        wait_new "^output HEADLESS-1 ${OW}x${OH} scale 1\$"
        wait_new "^size win 1404x1872 buffer 1404x1872 scale 1\$"
        expect "$T 9 scale 0.8: preferred 1" "^preferred win 1\$"
    else
        wait_new "^size win 1755x2340 buffer 1404x1872 scale 96\$"
    fi
    ok "$T 9 scale 0.8: a new size"
    check "$T 9 scale 0.8, pixel for pixel" HEADLESS-1 win 0 0 1404 1872
    send "ipc output HEADLESS-1 scale 2"
    if [ "$M" = whole ]; then
        wait_new "^size win 702x936 buffer 1404x1872 scale 2\$"
    else
        wait_new "^size win 702x936 buffer 1404x1872 scale 240\$"
    fi
    check "$T 9 scale 2, pixel for pixel" HEADLESS-1 win 0 0 1404 1872
}
two_case whole
two_case frac

# 10: the reported physical size in get_outputs (a headless output reports none)
send "outputs"
if new | grep -q '"glassine_physical_width": 0, *"glassine_physical_height": 0'; then
    ok "10 get_outputs has the reported physical size"
else
    bad "10 get_outputs: no glassine_physical_width/height"
fi
finish
