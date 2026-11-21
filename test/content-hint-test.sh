#!/bin/sh
# Host test of the content hints (glassine_ink_surface_v1.set_content, version 4): what
# an app says a commit is goes out with the panel output's FRAME. Runs this sway headless
# (pixman) in an Alpine 3.24 container of the host's architecture, with the glassine-epd
# output, glassine-epd-sink.c as the panel daemon and content-hint-client.c as the app,
# and checks the FRAMEs the sink got. See content-hint-run.sh for the cases.
# content-hint-test.sh [SWAY]: SWAY = a sway-glassine built by build-glassine.sh for this
# architecture (CI); without it the container builds this tree.
# Logs under build-test/content-hint/ (git-ignored).
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
OUT=$D/build-test/content-hint
mkdir -p "$OUT"
BIN=
if [ -n "${1:-}" ]; then
    BIN="-v $(cd "$(dirname "$1")" && pwd)/$(basename "$1"):/sway-bin:ro"
fi
# shellcheck disable=SC2086
docker run --rm $BIN -e HOST_UID="$(id -u)" -v "$D":/s:ro -v "$OUT":/out alpine:3.24 sh /s/test/content-hint-run.sh
