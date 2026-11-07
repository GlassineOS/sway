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
`XDG_RUNTIME_DIR`). Its temporary files go to `build-test/`, which git ignores.
