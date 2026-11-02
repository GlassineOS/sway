#!/bin/sh
# ink-names.sh: builds this sway on Alpine 3.24 and runs it headless, then a client under
# each protocol name, glassine_ink_v1 and na1_ink_v1, which must both work. Exit 0 = both.
# A control client under a made-up name must not be offered, to show the check can fail.
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm --platform linux/amd64 -v "$D":/s:ro alpine:3.24 sh -ec '
apk add -q build-base meson ninja pkgconf basu-dev cairo-dev eudev-dev gdk-pixbuf-dev json-c-dev libevdev-dev \
    libinput-dev libxkbcommon-dev linux-pam-dev pango-dev pcre2-dev wayland-dev wayland-protocols curl \
    librsvg-dev glib-dev libxres-dev linux-headers libdrm-dev
cp -r /s /tmp/t; cd /tmp/t; rm -rf build subprojects out-arm64
WLR=$(sh wlroots/setup.sh /tmp/t)
meson setup -Dsd-bus-provider=basu -Dman-pages=disabled -Dswaybar=false -Dswaynag=false $WLR build >/dev/null
ninja -C build sway/sway >/tmp/ninja.log 2>&1 || { tail -30 /tmp/ninja.log; exit 1; }
gcc -O2 -Wall -Isway -o /tmp/sink test/glassine-epd-sink.c
build_client() { # NAME
    mkdir -p /tmp/c/$1
    sed "s/glassine_ink/$1_ink/g; s/GLASSINE_INK/$(echo $1 | tr a-z A-Z)_INK/g" test/ink-names-client.c > /tmp/c/$1/client.c
    sed "s/glassine_ink/$1_ink/g" protocols/glassine-ink-v1.xml > /tmp/c/$1/ink.xml
    wayland-scanner client-header /tmp/c/$1/ink.xml /tmp/c/$1/ink-client-protocol.h
    wayland-scanner private-code /tmp/c/$1/ink.xml /tmp/c/$1/ink.c
    gcc -O2 -Wall -I/tmp/c/$1 -o /tmp/c/$1/client /tmp/c/$1/client.c /tmp/c/$1/ink.c -lwayland-client
}
for n in glassine na1 absent; do build_client $n; done
export XDG_RUNTIME_DIR=/tmp/rt GLASSINE_INK_DIR=/tmp/ink; mkdir -p -m 700 $XDG_RUNTIME_DIR; mkdir -p $GLASSINE_INK_DIR
S=$XDG_RUNTIME_DIR/glassine-epd.sock
/tmp/sink -s $S -d 250 -o /tmp > /tmp/sink.log 2>&1 &
printf "output EPD-1 scale 2\n" > /tmp/sway.conf
GLASSINE_EPD=1 GLASSINE_EPD_SOCK=$S WLR_BACKENDS=headless WLR_RENDERER=pixman build/sway/sway -c /tmp/sway.conf > /tmp/sway.log 2>&1 &
W=
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    W=$(cd $XDG_RUNTIME_DIR && ls | grep -E "^wayland-[0-9]+$" | head -1 || true)
    [ -n "$W" ] && break
    sleep 1
done
[ -n "$W" ] || { echo "FAIL sway did not start"; tail -20 /tmp/sway.log; exit 1; }
export WAYLAND_DISPLAY=$W
rc=0
/tmp/c/glassine/client || rc=1
/tmp/c/na1/client || rc=1
if /tmp/c/absent/client; then echo "FAIL the control name was offered"; rc=1; else echo "ok control name not offered"; fi
kill -0 $! 2>/dev/null || { echo "FAIL sway died"; tail -20 /tmp/sway.log; rc=1; }
exit $rc'
