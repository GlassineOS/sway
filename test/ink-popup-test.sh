#!/bin/sh
# Host test of the pen's ink areas on surfaces without a view: input-method
# popups (the on-screen keyboard's handwriting paper) and layer surfaces, next
# to a window's. Runs this sway headless (pixman, scale 2) in an Alpine 3.24
# container of the host's architecture, drives ink-popup-client.c and checks
# the ink lines sway writes to the panel daemon's FIFO (areas in panel px) and
# where the pointer's events go. See ink-popup-run.sh for the cases.
# ink-popup-test.sh [SWAY]: SWAY = a sway-glassine built by build-glassine.sh
# for this architecture (CI); without it the container builds this tree.
# Logs under build-test/ink-popup/ (git-ignored).
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
OUT=$D/build-test/ink-popup
mkdir -p "$OUT"
BIN=
if [ -n "${1:-}" ]; then
    BIN="-v $(cd "$(dirname "$1")" && pwd)/$(basename "$1"):/sway-bin:ro"
fi
# shellcheck disable=SC2086
docker run --rm $BIN -e HOST_UID="$(id -u)" -v "$D":/s:ro -v "$OUT":/out alpine:3.24 sh /s/test/ink-popup-run.sh
