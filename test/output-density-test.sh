#!/bin/sh
# Host test of the person's output density (sway/glassine-output-density.c, libc only):
# output-density-test.c built with the host's cc under build-test/ (git-ignored). CC
# overrides the compiler.
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$D/build-test"
T=$(mktemp -d "$D/build-test/output-density.XXXXXX")
trap 'rm -rf "$T"' EXIT
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -I"$D/sway" -o "$T/t" \
    "$D/test/output-density-test.c" "$D/sway/glassine-output-density.c"
"$T/t" "$T"
