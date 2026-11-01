#!/bin/sh
# build-glassine.sh [OUT]: sway-glassine for arm64, on Alpine 3.24 (qemu on x86, slow), with
# our wlroots linked in statically (wlroots/setup.sh). Not the wvkbd or daemon builds.
set -eu
D=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$D/out-arm64}
mkdir -p "$OUT"
docker run --rm --platform linux/arm64 -e HOST_UID="$(id -u)" -v "$D":/s:ro -v "$OUT":/out alpine:3.24 sh -ec '
apk add -q build-base meson ninja pkgconf basu-dev cairo-dev eudev-dev gdk-pixbuf-dev json-c-dev libevdev-dev \
    libinput-dev libxkbcommon-dev linux-pam-dev pango-dev pcre2-dev wayland-dev wayland-protocols curl \
    librsvg-dev glib-dev libxres-dev linux-headers libdrm-dev
cp -r /s /tmp/t; cd /tmp/t; rm -rf build subprojects out-arm64
WLR=$(sh wlroots/setup.sh /tmp/t)
meson setup -Dsd-bus-provider=basu -Dman-pages=disabled -Dswaybar=false -Dswaynag=false --buildtype=release $WLR build >/dev/null
ninja -C build sway/sway >/tmp/ninja.log 2>&1 || { tail -30 /tmp/ninja.log; exit 1; }
install -m 755 build/sway/sway /out/sway-glassine
chown -R "$HOST_UID" /out'
ls -la "$OUT/sway-glassine"
