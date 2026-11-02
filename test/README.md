# Host tests

`glassine-epd-sink.c` stands in for the panel daemon. `host-test.sh` and `band-test.sh`
(with `band-check.py`, `band-dots.py`, `glassine-band-client.c`) run this sway headless
and check what it sends.

**Not ported yet.** These scripts still expect the tree they came from: they apply patches
to a sway tarball and take sources from a neighbouring `src/` directory. They don't run
from this repository until that's changed. `build-glassine.sh` does run.

`ink-names.sh` does run from this repository: it builds sway on Alpine 3.24, runs it headless, and
checks that a client works under each ink protocol name. It needs Docker, and doesn't test a real app.
