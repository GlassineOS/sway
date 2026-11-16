#!/bin/sh
# The container half of handwriting-test.sh (Alpine 3.24, as root).
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
    SWAY=/tmp/t/build/sway/sway
fi
G=/tmp/gen
mkdir -p $G
wayland-scanner client-header protocols/glassine-handwriting-v1.xml $G/glassine-handwriting-v1-client-protocol.h
wayland-scanner private-code protocols/glassine-handwriting-v1.xml $G/glassine-handwriting-v1-protocol.c
gcc -std=c11 -O2 -Wall -Werror -I$G -o /tmp/client test/handwriting-client.c $G/glassine-handwriting-v1-protocol.c \
    -lwayland-client

# the stand-in service: hello on fd 3, then every line it gets into a log
cat >/tmp/service <<'EOS'
#!/bin/sh
[ "$1 $2" = "--pen-fd 3" ] || exit 64
printf '{"op":"hello","version":1,"neaten":true,"text":true,"letters":false,"languages":["en","de"]}\n' >&3
exec cat <&3 >>/out/service.log
EOS
chmod +x /tmp/service

export XDG_RUNTIME_DIR=/tmp/rt
mkdir -p -m 700 $XDG_RUNTIME_DIR
rm -f /out/*.log
printf "output HEADLESS-1 mode 800x600\n" >/tmp/sway.conf
FAIL=0
check() {   # check NAME COND...
    n=$1; shift
    if "$@"; then echo "  ok   $n"; else echo "  FAIL $n"; FAIL=1; fi
}
run() {     # run SERVICE LOG: sway with that service, the client for 2 s
    WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 GLASSINE_HANDWRITING_SERVICE="$1" \
        $SWAY -c /tmp/sway.conf -d >/out/sway-$2.log 2>&1 &
    SW=$!
    i=0
    while [ ! -S $XDG_RUNTIME_DIR/wayland-1 ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
    sleep 0.5
    (sleep 2) | WAYLAND_DISPLAY=wayland-1 /tmp/client >/out/client-$2.log 2>&1 || true
    kill $SW 2>/dev/null || true
    wait $SW 2>/dev/null || true
    rm -f $XDG_RUNTIME_DIR/wayland-1 $XDG_RUNTIME_DIR/wayland-1.lock
}
echo "handwriting: without the service"
run "" none
check "the global, raw strokes only (caps 1)" grep -qx "caps 1" /out/client-none.log
echo "handwriting: with the stand-in service"
run /tmp/service svc
check "the service's capabilities (strokes, neaten, text: 7)" grep -qx "caps 7" /out/client-svc.log
check "its languages" grep -qx "language de" /out/client-svc.log
check "the surface, with the app's pid" grep -q '"op":"surface","s":1,"pid":[1-9]' /out/service.log
check "its choices (neaten 0.5)" grep -q '"op":"set","s":1,"neaten":true,"strength":0.5' /out/service.log
check "the surface's end when the app goes" grep -q '"op":"destroy","s":1' /out/service.log
chown -R "$HOST_UID" /out
if [ $FAIL -eq 0 ]; then echo "handwriting: all cases passed"; else echo "handwriting: FAILED"; fi
exit $FAIL
