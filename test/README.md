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

`palm-window-test.sh` runs with the host's C compiler too: it checks
`sway/glassine-palm-window.c`, the window that holds a touch beside the writing paper
before it counts as a palm (the pen came), a tap (it lifted) or a finger (still down when
the window ends). Touch can't be injected into a headless sway, so the glue in
`sway/input/cursor.c` is not covered.

`ink-popup-test.sh [SWAY]` runs too (Docker): it runs this sway headless in Alpine 3.24 with
`ink-popup-client.c` (a window, a layer surface, input-method popups and a virtual pointer)
and checks the ink lines sway writes to the panel daemon's FIFO: a popup's areas at its
position, cut by what is above it and cutting what is below, following it when it moves and
gone when it unmaps; a layer surface's areas; and that the pointer's press and drag reach
the popup without moving focus. SWAY: a `build-glassine.sh` binary for the host's
architecture (as in CI); without it the container builds this tree. Logs go to
`build-test/ink-popup/`.

`shm-seal-test.sh` runs too (curl, patch and a C compiler): it builds wlroots' `util/shm.c`
with our patches and checks that the pixman renderer's buffers take `F_SEAL_SHRINK`, as the
panel daemon requires, and that the backend's memfds (the cursor) allow sealing.
