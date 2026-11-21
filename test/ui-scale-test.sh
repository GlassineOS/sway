#!/bin/sh
# Host test of fractional output scales: a client that takes only whole buffer
# scales gets the nearest one and is shown pixel for pixel (a pixel view,
# sway/glassine-view-px.h), a client with fractional scale is never scaled by a
# pixel, and at a whole scale nothing differs. Runs this sway headless (pixman)
# in an Alpine 3.24 container of the host's architecture, drives
# ui-scale-client.c and compares screenshots with the clients' buffers. See
# ui-scale-run.sh for the cases.
# ui-scale-test.sh [SWAY [BASE]]: SWAY = a sway-glassine built by
# build-glassine.sh for this architecture (CI); without it (or "") the
# container builds this tree. BASE = a sway built from the tree before a
# change: the scale 2 cases then run on both and must give the same output,
# byte for byte. Logs under build-test/ui-scale/ (git-ignored).
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
OUT=$D/build-test/ui-scale
mkdir -p "$OUT"
BIN=
if [ -n "${1:-}" ]; then
    BIN="-v $(cd "$(dirname "$1")" && pwd)/$(basename "$1"):/sway-bin:ro"
fi
if [ -n "${2:-}" ]; then
    BIN="$BIN -v $(cd "$(dirname "$2")" && pwd)/$(basename "$2"):/base-bin:ro"
fi
# shellcheck disable=SC2086
docker run --rm $BIN -e HOST_UID="$(id -u)" -v "$D":/s:ro -v "$OUT":/out alpine:3.24 sh /s/test/ui-scale-run.sh
