#!/bin/sh
# The dead band's host test (no device): GLES sway with the grey
# output on the host's render node, as host-test.sh GLES=1 GREY=1, one container.
# src/wayland/glassine-band-client draws exact greys, glassine-epd-sink logs what sway sends;
# band-check.py judges it: T 0 == the tree before the band, byte for byte (BASE:
# that tree, a sway of its own); T 4 / 8 error < T, drift through, flicker held,
# after the quiet the true frame == T 0's picture; identity table and gamma 1.8;
# T changing live; a reconnect. Then the real glassine-epdd-desktop (panel engine stubbed,
# ingest=cpu, ingest_view on) at T 0, T 4 (eink.conf) and T 4 set live on the FIFO:
# it logs the true frame, and the panel it leaves has the same levels as at T 0.
# Then the dots (band-dots.py): a dithered motion run through the same epdd (eink.conf "* mode=fast",
# blue noise and dizzy, T 0 / 4 / 8): the panel image after each update, dots against T 0, flips.
# Dizzy with ingest_view=off: its per-pixel reads of a discrete GPU's mapped VRAM made ~10 updates
# in 20 s here (never a run end), a host artefact the copy avoids. PATS="dizzy": only that pattern.
# band-test.sh [OUT] (OUT default: a directory under $TMPDIR)
# PHASES="dots" (default "sink epdd dots"): only those; without "sink" no base tree is built
set -eu
D=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$D/../src" && pwd)
OUT=${1:-$(mktemp -d)}
BASE=${BASE:-091b4db3}
mkdir -p "$OUT/base-tree"
(cd "$D/../../.." && git archive "$BASE" tools/pmos-glassine/sway tools/pmos-glassine/src tools/pmos-glassine/overlay/usr/share/glassine/sway) | tar -x -C "$OUT/base-tree"
timeout 1500 docker run --rm -e PHASES="${PHASES:-sink epdd dots}" -e PATS="${PATS:-blue dizzy}" --device /dev/dri/renderD128 -v "$D":/sway:ro -v "$SRC":/src:ro -v "$OUT/base-tree/tools/pmos-glassine":/base:ro -v "$OUT":/out alpine:3.22 sh -c '
set -e
apk add -q build-base meson ninja pkgconf basu-dev cairo-dev eudev-dev gdk-pixbuf-dev json-c-dev libevdev-dev \
    libinput-dev libxkbcommon-dev linux-pam-dev pango-dev pcre2-dev wayland-dev wayland-protocols wlroots-dev \
    curl python3 py3-numpy mesa-dri-gallium mesa-egl mesa-gbm mesa-gles \
    libx11-dev libxext-dev libxdamage-dev libxfixes-dev libxi-dev libxcomposite-dev mesa-dev >/dev/null
build() {   # tree dir, into /tmp/$2
    S=$1/sway; R=$1/src
    mkdir -p /tmp/$2 && cd /tmp/$2
    curl -sfL https://github.com/swaywm/sway/archive/1.10.1/sway-1.10.1.tar.gz | tar xz && cd sway-1.10.1
    for p in $S/0*.patch; do patch -p1 -s < $p; done
    WLR=$(sh $S/wlroots/setup.sh /tmp/$2/sway-1.10.1)
    cp $R/wayland/glassine-epd-backend.c $R/wayland/glassine-epd-backend.h $R/glassine-epd-output-proto.h \
        $R/wayland/glassine-gesture.c $R/wayland/glassine-gesture.h $R/wayland/glassine-popup-hint.c $R/wayland/glassine-popup-hint.h \
        $R/wayland/glassine-ink-area.c $R/wayland/glassine-ink-area.h $R/epd-brush.c $R/epd-brush.h sway/
    cp $R/wayland/glassine-ink-v1.xml protocols/
    cp $R/wayland/glassine-gesture.h sway/input/
    meson setup -Dsd-bus-provider=basu -Dman-pages=disabled -Dswaybar=false -Dswaynag=false $WLR build >/dev/null
    ninja -C build sway/sway >/tmp/ninja-$2.log 2>&1 || { tail -30 /tmp/ninja-$2.log; exit 1; }
    grep -iE "warning" /tmp/ninja-$2.log | grep -i glassine || true
}
mkdir -p /tmp/new /tmp/old
ln -sfn /sway /tmp/new/sway; ln -sfn /src /tmp/new/src
ln -sfn /base/sway /tmp/old/sway; ln -sfn /base/src /tmp/old/src
build /tmp/new n
case "$PHASES" in *sink*) build /tmp/old o; echo "built: the tree before the band";; esac
echo "built: this tree"
cd /tmp
I=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml; G=/tmp/cl; mkdir -p $G
wayland-scanner client-header $I $G/xdg-shell-client-protocol.h; wayland-scanner private-code $I $G/xdg-shell-protocol.c
gcc -O2 -Wall -Wextra -I$G -o /tmp/band-client /src/wayland/glassine-band-client.c $G/xdg-shell-protocol.c -lwayland-client
gcc -O2 -Wall -o /tmp/sink /src/wayland/glassine-epd-sink.c -lm
gcc -O2 -Wall -o /tmp/glassine-epdd-desktop /src/glassine-epdd-desktop.c /src/epd-wl.c /src/epd-ingest.c /src/epd-policy.c /src/epd-motion.c /src/epd-engine.c /src/epd-comp.c /src/epd-brush.c \
    /src/test/epd-stream-stub.c -lX11 -lXext -lXdamage -lXfixes -lXi -lXcomposite -lEGL -lm -lpthread
set +e
export XDG_RUNTIME_DIR=/tmp/rt; mkdir -p -m 700 $XDG_RUNTIME_DIR
S=$XDG_RUNTIME_DIR/glassine-epd.sock
N=120
L=/out/logs; rm -rf $L; mkdir -p $L
printf "output EPD-1 scale 1\noutput HEADLESS-1 disable\ndefault_border none\nexec /tmp/band-client $N 1000 >$L/client.log 2>&1\n" > /tmp/sway.conf
sway_up() {     # which, log
    GLASSINE_EPD_GREY=1 GLASSINE_EPD=1 GLASSINE_EPD_SOCK=$S WLR_BACKENDS=headless WLR_RENDERER=gles2 WLR_LIBINPUT_NO_DEVICES=1 \
        /tmp/$1/sway-1.10.1/build/sway/sway -c /tmp/sway.conf -d > $L/$2.sway.log 2>&1 &
    SW=$!
}
run() {         # name, which, sink options (the sink delay: a short panel update)
    /tmp/sink -s $S -d 20 -o $L "$@" > $L/$NAME.log 2>&1 &
    SK=$!
    sleep 0.2
    sway_up $WHICH $NAME
    sleep ${RUN:-20}
    kill $SW 2>/dev/null; kill $SK 2>/dev/null; wait $SW 2>/dev/null; wait $SK 2>/dev/null
    grep -E "dead band|gamma pass failed" $L/$NAME.sway.log | sed "s/^/$NAME: /" | tail -2
}
case "$PHASES" in *sink*)
for g in 1 1.8; do
    tg=; [ $g = 1 ] || tg=-g18
    NAME=base$tg WHICH=o run -g $g -b 8
    NAME=t0$tg WHICH=n run -g $g
    NAME=t4$tg WHICH=n run -g $g -b 4
    NAME=t8$tg WHICH=n run -g $g -b 8
done
NAME=change WHICH=n run -g 1 -b 8 -c 50:0
# epdd gone with banded greys in our last target: the sink exits after 40 frames, the next comes up
timeout 40 /tmp/sink -s $S -d 20 -g 1 -b 8 -n 40 > $L/reconnect1.log 2>&1 &
SK=$!
sleep 0.2; sway_up n reconnect
wait $SK
/tmp/sink -s $S -d 20 -g 1 -b 8 > $L/reconnect2.log 2>&1 &
SK=$!
sleep 4
kill $SW $SK 2>/dev/null; wait $SW 2>/dev/null; wait $SK 2>/dev/null
python3 /sway/band-check.py $L $N || true
;; esac
case "$PHASES" in *epdd*)
# the real epdd: the true frame reaches the cleanup, ingest_view on (it reads sway in place).
# T 4 set by eink.conf; "ctl": T 0 at the start, then deadband=4 on the FIFO with the window moving
for T in 0 4 ctl; do
    D=/out/epdd$T; rm -rf $D; mkdir -p $D
    rm -f /run/glassine-epdd-desktop.ctl /etc/glassine/eink.conf; mkdir -p /etc/glassine
    [ $T != 4 ] || echo "@deadband=4" > /etc/glassine/eink.conf
    GLASSINE_EPDD="ingest=cpu ingest_view=on" GLASSINE_EPDD_SRC=wl GLASSINE_EPD_SOCK=$S STUB_OUT=$D /tmp/glassine-epdd-desktop -v > $D/epdd.log 2>&1 &
    E=$!
    sleep 0.5
    sway_up n epdd$T
    if [ $T = ctl ]; then sleep 2; echo deadband=4 > /run/glassine-epdd-desktop.ctl; sleep $((${RUN:-20} - 2)); else sleep ${RUN:-20}; fi
    echo stats > /run/glassine-epdd-desktop.ctl; sleep 0.3
    kill $SW; sleep 0.3; kill $E 2>/dev/null; wait $E 2>/dev/null
    echo "--- epdd T $T"
    grep -E "gamma on|deadband=|ingest_view=" $D/epdd.log | tail -4
    grep "dead band" $L/epdd$T.sway.log | cut -d" " -f6-
    grep -c SUBMIT $D/epdd.log | sed "s/^/updates: /"
    grep -E "SUBMIT|true frame:" $D/epdd.log | tail -5
done
# this run ends in a GC16 of greys and the scan goes by g >> 4, so a true grey inside the held
# one's level is not sent again: its panel buffers compare as levels. No statement about dots (the
# dither rows read the whole byte): the dots phase below
python3 - <<PY
for t in ("4", "ctl"):
    a = open("/out/epdd0/panel.pgm", "rb").read(); b = open(f"/out/epdd{t}/panel.pgm", "rb").read()
    ha, hb = a.index(b"255\n") + 4, b.index(b"255\n") + 4
    a, b = a[ha:], b[hb:]
    print(f"panel T {t} vs T 0: {sum(x != y for x, y in zip(a, b))} of {len(a)} bytes differ, in level: {sum((x ^ y) >> 4 != 0 for x, y in zip(a, b))}")
PY
;; esac
case "$PHASES" in *dots*)
printf "output EPD-1 scale 1\noutput HEADLESS-1 disable\ndefault_border none\nexec /tmp/band-client $N 1000 g >$L/client.log 2>&1\n" > /tmp/sway.conf
for P in $PATS; do for T in 0 4 8; do
    D=/out/dots-$P-$T; rm -rf $D; mkdir -p $D
    rm -f /run/glassine-epdd-desktop.ctl; mkdir -p /etc/glassine
    printf "@deadband=$T\n@dither_pattern=$P\n* mode=fast\n" > /etc/glassine/eink.conf
    V=on; [ $P != dizzy ] || V=off
    STUB_DUMP=/tmp/dump-$P-$T.raw GLASSINE_EPDD="ingest=cpu ingest_view=$V" GLASSINE_EPDD_SRC=wl GLASSINE_EPD_SOCK=$S STUB_OUT=$D /tmp/glassine-epdd-desktop -v > $D/epdd.log 2>&1 &
    E=$!
    sleep 0.5
    sway_up n dots-$P-$T
    sleep ${RUN:-20}
    kill $SW; sleep 0.3; kill $E 2>/dev/null; wait $E 2>/dev/null
done; python3 /sway/band-dots.py /tmp $P /out; rm -f /tmp/dump-$P-*.raw; done
;; esac
chown -R $(stat -c %u /out) /out 2>/dev/null
'
echo "logs in $OUT/logs"
