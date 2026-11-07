#!/bin/sh
# Host test of the gesture actions (sway/glassine-gesture-action.c, no wlroots):
# gesture-action-test.c built with the host's cc and run in a temporary
# directory under build-test/ (git-ignored). CC overrides the compiler.
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$D/build-test"
T=$(mktemp -d "$D/build-test/gesture-action.XXXXXX")
trap 'rm -rf "$T"' EXIT
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -I"$D/sway" -o "$T/t" \
    "$D/test/gesture-action-test.c" "$D/sway/glassine-gesture-action.c"
mkdir "$T/dir"
"$T/t" "$T/dir"
