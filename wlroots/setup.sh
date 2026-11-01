#!/bin/sh
# Our wlroots 0.18.2 (+ ./0*.patch: the GLES grey output) as the wlroots subproject of a sway source tree, so sway-glassine links it
# statically and the device's libwlroots stays as it is.
# setup.sh SWAY_SRC_DIR (inside the Alpine 3.24 build containers)
# Prints the meson options for sway's setup on stdout.
set -eu
D=$(cd "$(dirname "$0")" && pwd)
SW=$1
apk add -q libdrm-dev mesa-dev pixman-dev libinput-dev eudev-dev libxkbcommon-dev libseat-dev hwdata-dev \
    libdisplay-info-dev libliftoff-dev xcb-util-wm-dev xcb-util-renderutil-dev xwayland-dev lcms2-dev >/dev/null
mkdir -p "$SW/subprojects"
rm -rf "$SW/subprojects/wlroots"
curl -sfL https://gitlab.freedesktop.org/wlroots/wlroots/-/archive/0.18.2/wlroots-0.18.2.tar.gz | tar xz -C "$SW/subprojects"
mv "$SW/subprojects/wlroots-0.18.2" "$SW/subprojects/wlroots"
for p in "$D"/0*.patch; do patch -d "$SW/subprojects/wlroots" -p1 -s < "$p"; done
echo "-Dwerror=false -Dwlroots:werror=false --force-fallback-for=wlroots -Dwlroots:default_library=static -Dwlroots:examples=false -Dwlroots:renderers=gles2 -Dwlroots:backends=drm,libinput -Dwlroots:xwayland=enabled -Dwlroots:color-management=enabled -Dwlroots:session=enabled -Dwlroots:libliftoff=disabled"
