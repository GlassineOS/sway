#!/bin/sh
# handwriting-standin.sh [OUT]: builds test/handwriting-standin.c with sway's
# glassine-handwriting.c (libwayland-server, json-c, wayland-scanner; the host's C
# compiler) into OUT (default build-test/handwriting-standin). GlassineOS/handwriting's
# tests run the pen interface against it (raw strokes, the service, the client library).
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$D/build-test/handwriting-standin}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
wayland-scanner server-header "$D/protocols/glassine-handwriting-v1.xml" "$T/glassine-handwriting-v1-protocol.h"
wayland-scanner private-code "$D/protocols/glassine-handwriting-v1.xml" "$T/glassine-handwriting-v1-protocol.c"
mkdir -p "$(dirname "$OUT")"
${CC:-cc} -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -Werror -I"$T" -I"$D/sway" \
	$(pkg-config --cflags wayland-server json-c) -o "$OUT" \
	"$D/test/handwriting-standin.c" "$D/sway/glassine-handwriting.c" "$T/glassine-handwriting-v1-protocol.c" \
	$(pkg-config --libs wayland-server json-c) -lm
echo "$OUT"
