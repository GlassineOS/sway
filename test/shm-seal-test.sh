#!/bin/sh
# Host test: the buffers sway sends the e-ink panel daemon can be sealed against
# shrinking (the daemon seals every file before it maps it and refuses one that
# can't be sealed). Builds wlroots' util/shm.c from the wlroots version in
# wlroots/setup.sh with our patches applied, and checks that allocate_shm_file
# (the pixman renderer's buffers) returns a file that takes F_SEAL_SHRINK; then
# that every memfd_create in the backend allows sealing (the cursor).
# Needs curl, patch and a C compiler (CC). Work files under build-test/ (git-ignored).
set -eu
D=$(cd "$(dirname "$0")/.." && pwd)
V=$(sed -n 's|.*/wlroots/-/archive/\([0-9.]*\)/.*|\1|p' "$D/wlroots/setup.sh" | head -1)
mkdir -p "$D/build-test"
T=$(mktemp -d "$D/build-test/shm-seal.XXXXXX")
trap 'rm -rf "$T"' EXIT
curl -sfL "https://gitlab.freedesktop.org/wlroots/wlroots/-/archive/$V/wlroots-$V.tar.gz" | tar xz -C "$T"
W=$T/wlroots-$V
for p in "$D"/wlroots/0*.patch; do patch -d "$W" -p1 -s < "$p"; done
mkdir -p "$T/inc/wlr"
: > "$T/inc/wlr/config.h"     # meson generates it; util/shm.c needs none of its macros
cat > "$T/main.c" <<'C'
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int allocate_shm_file(size_t size);
int main(void)
{
    int fd = allocate_shm_file(4096);
    if (fd < 0) { perror("allocate_shm_file"); return 1; }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) { perror("F_ADD_SEALS F_SEAL_SHRINK"); return 1; }
    if (!(fcntl(fd, F_GET_SEALS) & F_SEAL_SHRINK)) { fprintf(stderr, "seal not set\n"); return 1; }
    if (ftruncate(fd, 1024) == 0) { fprintf(stderr, "shrank a sealed file\n"); return 1; }
    printf("shm-seal: allocate_shm_file gives a file sealed against shrinking\n");
    return 0;
}
C
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Werror -I"$T/inc" -I"$W/include" -o "$T/t" "$T/main.c" "$W/util/shm.c"
"$T/t"
bad=$(grep -n 'memfd_create(' "$D/sway/glassine-epd-backend.c" | grep -v MFD_ALLOW_SEALING || true)
if [ -n "$bad" ]; then echo "shm-seal: memfd_create without MFD_ALLOW_SEALING: $bad" >&2; exit 1; fi
echo "shm-seal: every memfd of the backend allows sealing"
