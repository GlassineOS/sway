#!/bin/sh
# Host test of the edge swipes in sway/glassine-gesture.c (pure logic, no
# wlroots): gesture-edge-test.c built with the host's cc under build-test/
# (git-ignored) and run. CC overrides the compiler.
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$D/build-test"
T=$(mktemp -d "$D/build-test/gesture-edge.XXXXXX")
trap 'rm -rf "$T"' EXIT
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -I"$D/sway" -o "$T/t" \
    "$D/test/gesture-edge-test.c" "$D/sway/glassine-gesture.c" -lm
"$T/t"
