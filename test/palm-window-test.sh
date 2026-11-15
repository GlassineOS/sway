#!/bin/sh
# Host test of the writing paper's decision window (sway/glassine-palm-window.c,
# no wlroots): palm-window-test.c built with the host's cc under build-test/
# (git-ignored) and run. CC overrides the compiler.
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$D/build-test"
T=$(mktemp -d "$D/build-test/palm-window.XXXXXX")
trap 'rm -rf "$T"' EXIT
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -I"$D/sway" -o "$T/t" \
    "$D/test/palm-window-test.c" "$D/sway/glassine-palm-window.c"
"$T/t"
