#!/bin/sh
# Host test of the pen interface (glassine-handwriting.c) in this sway, headless in an
# Alpine 3.24 container: the global, the capabilities without the service, the service
# started with its channel on fd 3 (a stand-in service that says hello and logs what it
# gets), the capabilities it gives, and the surface it is told of with the app's identity.
# The pen's strokes themselves are tested with the stand-in compositor
# (handwriting-standin.sh, GlassineOS/handwriting's tests): headless sway has no tablet.
# handwriting-test.sh [SWAY]: as ink-popup-test.sh. Logs under build-test/handwriting/.
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
OUT=$D/build-test/handwriting
mkdir -p "$OUT"
BIN=
if [ -n "${1:-}" ]; then
    BIN="-v $(cd "$(dirname "$1")" && pwd)/$(basename "$1"):/sway-bin:ro"
fi
# shellcheck disable=SC2086
docker run --rm $BIN -e HOST_UID="$(id -u)" -v "$D":/s:ro -v "$OUT":/out alpine:3.24 sh /s/test/handwriting-run.sh
