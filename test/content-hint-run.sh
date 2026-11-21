#!/bin/sh
# The container half of content-hint-test.sh (Alpine 3.24, as root). The panel output
# EPD-1 is 800x600 at scale 2 (layout 400x300); the app's window floats at 20,30 and is
# 100x60: 200x120+40+60 in the frame's buffer.
# The cases, each run with the output as it is and turned by 90 degrees:
#  1  the global is version 4; HELLO says the frames carry hints
#  2  a commit said to be a page: its FRAME says page, at the window's place (the same
#     rect as the damage of a commit that repaints the whole window)
#  3  the next commit, with nothing said: a FRAME as before version 4 (stride 0, no
#     byte more than its damage rects)
#  4  motion: its FRAME says motion
#  5  a value this version does not know: nothing said
#  6  a page said for a commit that changes nothing: no FRAME, and the next commit with
#     damage says nothing (the hint was that commit's)
#  7  a page whose commit damages a small part: the FRAME's damage is that part, the
#     place said is the window
#  8  a client bound at version 3 (no set_content): its frames say nothing
set -eu
apk add -q build-base meson ninja pkgconf basu-dev cairo-dev eudev-dev gdk-pixbuf-dev json-c-dev libevdev-dev \
    libinput-dev libxkbcommon-dev linux-pam-dev pango-dev pcre2-dev wayland-dev wayland-protocols curl \
    librsvg-dev glib-dev libxres-dev linux-headers libdrm-dev
cp -r /s /tmp/t
cd /tmp/t
rm -rf build subprojects out-arm64 build-test
if [ -e /sway-bin ]; then
    SWAY=/sway-bin
else
    WLR=$(sh wlroots/setup.sh /tmp/t)
    # shellcheck disable=SC2086
    meson setup -Dsd-bus-provider=basu -Dman-pages=disabled -Dswaybar=false -Dswaynag=false $WLR build >/dev/null
    ninja -C build sway/sway >/tmp/ninja.log 2>&1 || { tail -30 /tmp/ninja.log; exit 1; }
    grep -i "warning" /tmp/ninja.log | grep -i glassine || true
    SWAY=/tmp/t/build/sway/sway
fi

G=/tmp/gen
mkdir -p $G
for x in /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml protocols/glassine-ink-v1.xml; do
    n=$(basename "$x" .xml)
    wayland-scanner client-header "$x" $G/$n-client-protocol.h
    wayland-scanner private-code "$x" $G/$n-protocol.c
done
gcc -std=c11 -O2 -Wall -Werror -I$G -o /tmp/client test/content-hint-client.c $G/*-protocol.c -lwayland-client
gcc -O2 -Wall -Werror -Isway -o /tmp/sink test/glassine-epd-sink.c -lm

export XDG_RUNTIME_DIR=/tmp/rt
mkdir -p -m 700 $XDG_RUNTIME_DIR
S=$XDG_RUNTIME_DIR/glassine-epd.sock
rm -f /out/*.log
FAIL=0
ok() { echo "ok: $1"; }
bad() { echo "FAIL: $1"; FAIL=1; }

# one run: $1 = a name for the logs, $2 = the output's transform, $3 = the client's version
run() {
    L=/out/sink-$1.log
    /tmp/sink -s $S > $L 2>&1 &
    SK=$!
    sleep 0.3
    printf "output EPD-1 scale 2 transform %s\noutput HEADLESS-1 disable\ndefault_border none\nfor_window [app_id=\"content-hint\"] floating enable, move position 20 30\n" "$2" >/tmp/sway.conf
    GLASSINE_HANDWRITING_SERVICE= GLASSINE_EPD=1 GLASSINE_EPD_SOCK=$S GLASSINE_EPD_SIZE=800x600 WLR_BACKENDS=headless WLR_RENDERER=pixman \
        WLR_LIBINPUT_NO_DEVICES=1 $SWAY -c /tmp/sway.conf -d >/out/sway-$1.log 2>&1 &
    SW=$!
    i=0
    while [ ! -S $XDG_RUNTIME_DIR/wayland-1 ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    rm -f /tmp/cmd
    mkfifo /tmp/cmd
    WAYLAND_DISPLAY=wayland-1 /tmp/client "$3" </tmp/cmd >/out/client-$1.log 2>&1 &
    CL=$!
    exec 3>/tmp/cmd
}
stop() {
    exec 3>&-
    kill $CL $SW $SK 2>/dev/null || true
    wait $CL $SW $SK 2>/dev/null || true
    rm -f $XDG_RUNTIME_DIR/wayland-1* $S
}
# a command to the client; then the frames it caused are in the sink's log
N0=0
send() {
    N0=$(wc -l < $L)
    echo "$2" >&3
    i=0
    while ! grep -q -- "^ok $2\$" /out/client-$1.log; do
        i=$((i + 1))
        if [ $i -gt 50 ]; then bad "$1: no answer to '$2'"; tail -3 /out/client-$1.log; return; fi
        sleep 0.1
    done
    sleep 0.5
}
new() { tail -n +$((N0 + 1)) $L; }
# the damage rect of the only FRAME since the last send, as WxH+X+Y ("" unless one FRAME with one rect)
dmg() { new | awk '/^FRAME/ { f++ } /^  [0-9]+x[0-9]+\+/ { r = $1; n++ } END { if (f == 1 && n == 1) print r }'; }
said() { new | awk '/^  SAID/ { print $3, $4 }'; }
# every FRAME since the last send says nothing and is as long as its damage rects
plain() { new | awk '/^  LEN/ { split($3, b, "="); split($4, r, "="); split($5, s, "="); n++; if (s[2] != 0 || b[2] != 36 + 16 * r[2]) x = 1 } END { exit !(n > 0 && !x) }'; }

cases() {
    T=$1
    run $T "$2" 4
    i=0
    while ! grep -q "^ready" /out/client-$T.log && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    grep -q "^ink version 4" /out/client-$T.log && ok "$T: the ink global is version 4" || bad "$T: the ink global's version: $(grep 'ink version' /out/client-$T.log)"
    send $T "win 100 60"
    caps=$(awk '/^HELLO/ { print $3 }' $L | tail -1)
    [ $(( 0x${caps:-0} & 16 )) -eq 16 ] && ok "$T: HELLO says the frames carry hints (caps $caps)" || bad "$T: HELLO caps '$caps' lack the hint bit"
    send $T "paint 0 0 100 60 none"
    WIN=$(dmg)
    [ -n "$WIN" ] && plain && ok "$T: nothing said: a frame as before ($WIN)" || { bad "$T: nothing said: not one plain FRAME"; new; }
    [ "$2" != normal ] || [ "$WIN" = "200x120+40+60" ] || bad "$T: the window's damage is $WIN, expected 200x120+40+60"
    send $T "paint 0 0 100 60 page"
    [ "$(said)" = "kind=1 at=$WIN" ] && [ "$(dmg)" = "$WIN" ] && ok "$T: a page: said with its FRAME, at the window ($WIN)" || { bad "$T: a page: said '$(said)', damage '$(dmg)', the window $WIN"; new; }
    send $T "paint 0 0 100 60 none"
    [ "$(dmg)" = "$WIN" ] && plain && ok "$T: the commit after it says nothing" || { bad "$T: the commit after a page"; new; }
    send $T "paint 0 0 100 60 motion"
    [ "$(said)" = "kind=2 at=$WIN" ] && ok "$T: motion: said with its FRAME" || { bad "$T: motion: said '$(said)'"; new; }
    send $T "paint 0 0 100 60 7"
    [ "$(dmg)" = "$WIN" ] && plain && ok "$T: an unknown value says nothing" || { bad "$T: an unknown value"; new; }
    send $T "say page"
    [ -z "$(new | grep '^FRAME' || true)" ] && ok "$T: a page said for a commit that changes nothing: no frame" || { bad "$T: a commit without damage made a frame"; new; }
    send $T "paint 0 0 100 60 none"
    [ "$(dmg)" = "$WIN" ] && plain && ok "$T: ... and the next commit says nothing" || { bad "$T: a hint outlived its commit"; new; }
    send $T "paint 10 10 20 20 page"
    D=$(dmg)
    [ "$(said)" = "kind=1 at=$WIN" ] && [ -n "$D" ] && [ "$D" != "$WIN" ] && ok "$T: a page with a small change: damage $D, said for the window" || { bad "$T: a small page: said '$(said)', damage '$D'"; new; }
    stop
}
cases normal normal
cases turned 90

run v3 normal 3
i=0
while ! grep -q "^ready" /out/client-v3.log && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
send v3 "win 100 60"
send v3 "paint 0 0 100 60 page"
[ "$(dmg)" = "200x120+40+60" ] && plain && ok "v3: a client at version 3 says nothing" || { bad "v3: a version 3 client's frame"; new; }
stop

chown -R "$HOST_UID" /out
if [ $FAIL -eq 0 ]; then echo "content-hint: all cases passed"; else echo "content-hint: FAILED"; fi
exit $FAIL
