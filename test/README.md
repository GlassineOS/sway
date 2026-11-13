# Host tests

`glassine-epd-sink.c` stands in for the panel daemon. `host-test.sh` and `band-test.sh`
(with `band-check.py`, `band-dots.py`, `glassine-band-client.c`) run this sway headless
and check what it sends.

**Not ported yet.** These scripts still expect the tree they came from: they apply patches
to a sway tarball and take sources from a neighbouring `src/` directory. They don't run
from this repository until that's changed. `build-glassine.sh` does run.

`gesture-action-test.sh` does run, with the host's C compiler and nothing else: it checks
`sway/glassine-gesture-action.c`, the gestures file's old and new action ids, the
defaults, and the request each action sends (glassine-swayd's socket faked in a temporary
`XDG_RUNTIME_DIR`). The binary goes to `build-test/`, which git ignores; the fake socket's
directory goes under `$TMPDIR` (`/tmp`), since a deep checkout's path would be over the 108-byte
socket path limit. A `$TMPDIR` that is still too long makes the test exit 77: a skip, not a pass.

`shm-seal-test.sh` runs too (curl, patch and a C compiler): it builds wlroots' `util/shm.c`
with our patches and checks that the pixman renderer's buffers take `F_SEAL_SHRINK`, as the
panel daemon requires, and that the backend's memfds (the cursor) allow sealing.
