#!/bin/sh
# Host test of the writing regions (glassine_handwriting_v1 version 2) in
# sway/glassine-handwriting.c: builds the stand-in compositor (handwriting-standin.sh) and
# handwriting-regions-client.c under build-test/ (git-ignored; the host's C compiler,
# libwayland, json-c, wayland-scanner, python3), then runs handwriting-regions-test.py with
# handwriting-regions-service.py as the service. Its sockets go under ${TMPDIR:-/tmp}.
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$D/build-test"
T=$(mktemp -d "$D/build-test/regions.XXXXXX")
R=$(mktemp -d "${TMPDIR:-/tmp}/glassine-hwr.XXXXXX")
trap 'rm -rf "$T" "$R"' EXIT
sh "$D/test/handwriting-standin.sh" "$T/standin" >/dev/null
wayland-scanner client-header "$D/protocols/glassine-handwriting-v1.xml" "$T/glassine-handwriting-v1-client-protocol.h"
wayland-scanner private-code "$D/protocols/glassine-handwriting-v1.xml" "$T/glassine-handwriting-v1-protocol.c"
${CC:-cc} -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -Werror -I"$T" -o "$T/client" \
	"$D/test/handwriting-regions-client.c" "$T/glassine-handwriting-v1-protocol.c" \
	$(pkg-config --cflags --libs wayland-client)
python3 "$D/test/handwriting-regions-test.py" "$T/standin" "$T/client" "$D/test/handwriting-regions-service.py" "$R"
