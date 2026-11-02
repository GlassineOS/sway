#!/bin/sh
# Host test of the glassine-epd sway backend, no device: Alpine 3.22
# (sway 1.10.1 / wlroots 0.18.2, as on the device) in a container. Builds sway
# with the patches (0001 glassine-epd backend, 0002 touch gestures) + ../src/wayland/glassine-epd-backend.c, and
# glassine-epd-sink (a stand-in for glassine-epdd-desktop); runs sway headless with the pixman
# renderer and GLASSINE_EPD=1, a terminal (foot) in it, and checks what the sink
# received. Frames land in OUT (default ./out): frame-*.ppm, sink.log, sway.log.
# EPDD=1: the real glassine-epdd-desktop (GLASSINE_EPDD_SRC=wl) instead of the sink, with its
# panel engine stubbed (../src/test/epd-stream-stub.c): epdd.log has the
# policy's updates, panel.pgm what the panel would show (panel order).
# host-test.sh [OUT] ARCH=arm64 builds/runs under qemu (slow)
# env: CMD (shell command in the terminal), DELAY (ms before DONE, 250), RUN (s, 8)
# FENCE=1 (with GLES=1): hold every client dma-buf commit until its fence
# fd is readable (wlroots 0002, GLASSINE_FENCE_WAIT_FORCE: exercises the hold path)
# HEAP=1 (with GLES=1): sway's render targets from the host's system dma-heap
# (our wlroots 0006, GLASSINE_EPD_HEAP=1; /dev/dma_heap/system into the container,
# whose root may open it): the cached-memory path of
# INGEST=cpu|gpu|check (with EPDD=1 GLES=1): glassine-epdd-desktop's ingest (default check)
# GREY=1 (with GLES=1): the grey output (R8 in panel order,
# our wlroots' luma shaders); GREY_TR=0..7 overrides its transform
# GLES=1: sway renders with GLES (WLR_RENDERER=gles2) on the host's render
# node (/dev/dri/renderD128 into the container, Mesa's drivers): GBM dma-bufs
# go to glassine-epdd-desktop; with EPDD=1 glassine-epdd-desktop runs ingest=check (GPU
# ingest against the CPU, every frame) and the log has the "ingest" line
# APK: extra Alpine packages for CMD (e.g. wayland-utils mesa-demos)
# INKDEMO=1: builds sway/glassine-ink-demo (run it from CMD); without EPDD a FIFO
# reader logs sway's ink messages to ink-fifo.log, brush files in OUT/glassine-ink
# KOREADER=1: KOReader (official x86_64 build, gcompat + libglassine-glibcshim,
# started by overlay/usr/bin/koreader) instead of the terminal
set -eu
D=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$D/../src" && pwd)
OUT=${1:-$D/out}
mkdir -p "$OUT"
DEV=
[ -z "${GLES:-}" ] || DEV="--device /dev/dri/renderD128"
[ -z "${HEAP:-}" ] || DEV="$DEV --device /dev/dma_heap/system"
docker run --rm $DEV --platform linux/${ARCH:-amd64} -e GLES="${GLES:-}" -e CMD="${CMD:-}" -e DELAY="${DELAY:-250}" -e RUN="${RUN:-8}" -e EPDD="${EPDD:-}" -e GREY="${GREY:-}" -e GAMMA="${GAMMA:-}" -e GREY_TR="${GREY_TR:-}" -e HEAP="${HEAP:-}" -e INGEST="${INGEST:-}" -e FENCE="${FENCE:-}" -e APK="${APK:-}" -e KOREADER="${KOREADER:-}" -e INK="${INK:-}" -e INKDEMO="${INKDEMO:-}" -e INKGTK="${INKGTK:-}" -v "$D/../overlay/usr/bin/koreader":/ko-launcher:ro -v "$D":/sway:ro -v "$SRC":/src:ro -v "$D/../overlay/usr/share/glassine/sway":/conf:ro -v "$OUT":/out alpine:3.22 sh -c '
set -e
apk add -q build-base meson ninja pkgconf basu-dev cairo-dev eudev-dev gdk-pixbuf-dev json-c-dev libevdev-dev \
    libinput-dev libxkbcommon-dev linux-pam-dev pango-dev pcre2-dev wayland-dev wayland-protocols wlroots-dev \
    curl foot font-dejavu >/dev/null
if [ -n "$KOREADER" ]; then
    apk add -q gcompat xz >/dev/null
    gcc -O2 -Wall -shared -fPIC -o /tmp/libglassine-glibcshim.so /src/glassine-glibcshim.c
    install -D /tmp/libglassine-glibcshim.so /usr/lib/glassine/libglassine-glibcshim.so
    curl -sfL https://github.com/koreader/koreader/releases/download/v2026.07.1/koreader-linux-x86_64-v2026.07.1.tar.xz | tar xJ -C /tmp
    mv /tmp/lib/koreader /usr/lib/koreader
    install -m 755 /ko-launcher /usr/bin/koreader
fi
[ -z "$GLES" ] || apk add -q mesa-dri-gallium mesa-egl mesa-gbm mesa-gles >/dev/null
[ -z "$APK" ] || apk add -q $APK >/dev/null
[ -z "$EPDD" ] || apk add -q libx11-dev libxext-dev libxdamage-dev libxfixes-dev libxi-dev libxcomposite-dev mesa-dev >/dev/null
cd /tmp && curl -sfL https://github.com/swaywm/sway/archive/1.10.1/sway-1.10.1.tar.gz | tar xz && cd sway-1.10.1
for p in /sway/0*.patch; do patch -p1 -s < $p; done
WLR=$(sh /sway/wlroots/setup.sh /tmp/sway-1.10.1)
cp /src/wayland/glassine-epd-backend.c /src/wayland/glassine-epd-backend.h /src/glassine-epd-output-proto.h \
    /src/wayland/glassine-gesture.c /src/wayland/glassine-gesture.h /src/wayland/glassine-popup-hint.c /src/wayland/glassine-popup-hint.h \
    /src/wayland/glassine-ink-area.c /src/wayland/glassine-ink-area.h /src/epd-brush.c /src/epd-brush.h sway/
cp /src/wayland/glassine-ink-v1.xml protocols/
cp /src/wayland/glassine-gesture.h sway/input/
meson setup -Dsd-bus-provider=basu -Dman-pages=disabled -Dswaybar=false -Dswaynag=false $WLR build >/dev/null
ninja -C build sway/sway swaymsg/swaymsg >/tmp/ninja.log 2>&1 || { tail -30 /tmp/ninja.log; exit 1; }
grep -i "warning" /tmp/ninja.log | grep -i glassine || true
XDG_RUNTIME_DIR=/tmp WLR_BACKENDS=headless WLR_RENDERER=pixman build/sway/sway -C -c /conf/config >/out/config-check.log 2>&1 && echo "device config: valid" || { echo "device config: INVALID"; cat /out/config-check.log; }
gcc -O2 -Wall -o /tmp/sink /src/wayland/glassine-epd-sink.c
if [ -n "$INKDEMO" ]; then        # the glassine_ink_v1 example client (ink area = its left half)
    P=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml; I=/src/wayland/glassine-ink-v1.xml; G=/tmp/inkdemo; mkdir -p $G
    wayland-scanner client-header $P $G/xdg-shell-client-protocol.h; wayland-scanner private-code $P $G/xdg-shell-protocol.c
    wayland-scanner client-header $I $G/glassine-ink-v1-client-protocol.h; wayland-scanner private-code $I $G/glassine-ink-v1-protocol.c
    gcc -O2 -Wall -I$G -o /tmp/glassine-ink-demo /src/wayland/glassine-ink-demo.c $G/*.c -lwayland-client -lm
fi
if [ -n "$INKGTK" ]; then         # the GTK module (canvas widgets as ink areas) + Xournal++
    apk add -q gtk+3.0-dev xournalpp >/dev/null
    I=/src/wayland/glassine-ink-v1.xml; G=/tmp/inkgtk; mkdir -p $G /usr/lib/gtk-3.0/modules
    wayland-scanner client-header $I $G/glassine-ink-v1-client-protocol.h; wayland-scanner private-code $I $G/glassine-ink-v1-protocol.c
    gcc -O2 -Wall -shared -fPIC -I$G -o /usr/lib/gtk-3.0/modules/libglassine-ink.so /src/wayland/glassine-ink-gtk.c $G/glassine-ink-v1-protocol.c \
        $(pkg-config --cflags --libs gtk+-3.0 wayland-client)
fi
export XDG_RUNTIME_DIR=/tmp/rt; mkdir -p -m 700 $XDG_RUNTIME_DIR
S=$XDG_RUNTIME_DIR/glassine-epd.sock
if [ -n "$EPDD" ]; then
    gcc -O2 -Wall -o /tmp/glassine-epdd-desktop /src/glassine-epdd-desktop.c /src/epd-wl.c /src/epd-ingest.c /src/epd-policy.c /src/epd-motion.c /src/epd-engine.c /src/epd-comp.c /src/epd-brush.c \
        /src/test/epd-stream-stub.c -lX11 -lXext -lXdamage -lXfixes -lXi -lXcomposite -lEGL -lm -lpthread
    [ -z "$INK" ] || { mkdir -p /etc/glassine; echo "foot mode=auto touch=auto ink=on brush=4-10^2 eraser=30" > /etc/glassine/eink.conf; }    # INK=1: foot is an ink app (the ink areas)
    GLASSINE_EPDD=${GLES:+ingest=${INGEST:-check}} GLASSINE_EPDD_SRC=wl GLASSINE_EPD_SOCK=$S STUB_OUT=/out /tmp/glassine-epdd-desktop -v > /out/epdd.log 2>&1 &
    sleep 0.5
else
    /tmp/sink -s $S -d $DELAY -o /out > /out/sink.log 2>&1 &
    if [ -n "$INKDEMO" ]; then    # no glassine-epdd-desktop: what sway sends its FIFO (ink-fifo.log) + the brush files (OUT/glassine-ink)
        rm -rf /out/glassine-ink; mkfifo /run/glassine-epdd-desktop.ctl
        (while :; do cat /run/glassine-epdd-desktop.ctl; done) > /out/ink-fifo.log 2>/dev/null &
        export GLASSINE_INK_DIR=/out/glassine-ink
    fi
fi
CMD=${CMD:-"echo Glassine e-ink via sway; sleep 1; ls /; sleep 30"}
if [ -n "$KOREADER" ]; then
    printf "output EPD-1 scale 2\noutput HEADLESS-1 disable\ndefault_border none\nexec /usr/bin/koreader > /out/koreader.log 2>&1\n" > /tmp/sway.conf
else
    printf "output EPD-1 scale 2\ndefault_border none\nexec foot -e sh -c \"%s\"\n" "$CMD" > /tmp/sway.conf
fi
REN=pixman; [ -z "$GLES" ] || REN=gles2
GLASSINE_EPD_HEAP=$HEAP GLASSINE_FENCE_WAIT=$FENCE GLASSINE_FENCE_WAIT_FORCE=$FENCE GLASSINE_EPD_GAMMA=$GAMMA GLASSINE_EPD_GREY=$GREY GLASSINE_EPD_GREY_TRANSFORM=$GREY_TR GLASSINE_EPD=1 GLASSINE_EPD_SOCK=$S WLR_BACKENDS=headless WLR_RENDERER=$REN WLR_LIBINPUT_NO_DEVICES=1 \
    build/sway/sway -c /tmp/sway.conf -d > /out/sway.log 2>&1 &
SW=$!
sleep $RUN
build/swaymsg/swaymsg -s $XDG_RUNTIME_DIR/sway-ipc.*.sock -t get_outputs 2>/dev/null | grep -E "\"name\"|current_mode|\"scale\"" | head -6 || true
build/swaymsg/swaymsg -s $XDG_RUNTIME_DIR/sway-ipc.*.sock -t get_tree 2>/dev/null > /out/tree.json || true
grep -oE "\"(app_id|shell|fullscreen_mode|name)\": [^,]*" /out/tree.json | grep -vE "__i3|root" | tail -8 || true
kill $SW; sleep 1
if [ -n "$EPDD" ]; then
    echo stats > /run/glassine-epdd-desktop.ctl; sleep 0.3
    grep -E "wl:|SUBMIT|running|ingest" /out/epdd.log | head -24
    grep "ink areas" /out/epdd.log | tail -3
    grep "  ink .* brush" /out/epdd.log | sort | uniq -c | tail -6
    grep -c SUBMIT /out/epdd.log | sed "s/^/updates: /"
else
    grep -c "^FRAME" /out/sink.log | sed "s/^/frames: /"
    grep -E "^(BUF|DROP)" /out/sink.log | head
fi
grep -iE "glassine-epd|glassine_heap" /out/sway.log | head -10
'
echo "frames in $OUT"
