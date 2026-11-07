#!/bin/sh
# Host test of the gesture actions (sway/glassine-gesture-action.c, no wlroots):
# gesture-action-test.c built with the host's cc under build-test/ (git-ignored)
# and run in a temporary directory under ${TMPDIR:-/tmp}: the fake socket's path
# must fit sockaddr_un (108 bytes), which a deep checkout's path does not. CC
# overrides the compiler. Exit 77: skipped (a TMPDIR that is too long).
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$D/build-test"
T=$(mktemp -d "$D/build-test/gesture-action.XXXXXX")
R=$(mktemp -d "${TMPDIR:-/tmp}/glassine-ga.XXXXXX")
trap 'rm -rf "$T" "$R"' EXIT
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -I"$D/sway" -o "$T/t" \
    "$D/test/gesture-action-test.c" "$D/sway/glassine-gesture-action.c"
"$T/t" "$R"
