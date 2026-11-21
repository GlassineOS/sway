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
It also checks the `edge EDGE ACTION` lines, their defaults (top `show-bar`, the others `nothing`)
the exit guard (no trigger leaves an immersive window: the top edge is `show-bar`), and that a
`show-bar` edge is tracked only over an immersive window.

`gesture-edge-test.sh` runs too, with the host's C compiler: it checks the edge swipes in
`sway/glassine-gesture.c` (a finger landing alone within 24 px of an armed edge of the output's
box and moving straight inward by the swipe distance; what is no edge swipe: too far from the
edge, an edge set to `nothing`, sideways or outward, too short, a second finger first; and that
the rest of the sequence is swallowed). The touch glue in `sway/input/cursor.c` is not covered.

`ink-popup-test.sh [SWAY]` runs too (Docker): it runs this sway headless in Alpine 3.24 with
`ink-popup-client.c` (a window, a layer surface, input-method popups and a virtual pointer)
and checks the ink lines sway writes to the panel daemon's FIFO: a popup's areas at its
position, cut by what is above it and cutting what is below, following it when it moves and
gone when it unmaps; a layer surface's areas; and that the pointer's press and drag reach
the popup without moving focus. Then `ink-region-client.c` (a window with writing regions and
no ink areas of its own): each region in ink mode is an ink area, following a move without a
new frame, its clip, text mode regions above it, its destruction, the app's own areas (a
region over them wins) and many regions at once; each owner's lines come after its key line
(which stays while the window lives), and a window with regions sends its visible region,
where a stroke a region takes inks past the region's edge. SWAY: a `build-glassine.sh` binary for the host's
architecture (as in CI); without it the container builds this tree. Logs go to
`build-test/ink-popup/`.

`handwriting-test.sh [SWAY]` runs too (Docker, as `ink-popup-test.sh`): the pen interface's
global, its capabilities without the service, and with a stand-in service that the test
starts through sway (its channel on fd 3) the capabilities it gives, the surface it is told
of with the app's pid, the surface's choices and its end. Headless sway has no tablet, so
the strokes themselves are tested with `handwriting-standin.sh`: it builds
`handwriting-standin.c`, a Wayland server with `glassine-handwriting.c` and a stand-in pen on
stdin (the host's C compiler, libwayland-server, json-c), which GlassineOS/handwriting's
tests run the service and the client library against.

`handwriting-regions-test.sh` runs too (the host's C compiler, libwayland, json-c,
wayland-scanner, python3): the writing regions of the pen interface's version 2 through the
stand-in compositor, with `handwriting-regions-client.c` as the app and
`handwriting-regions-service.py` as the service: the regions bit only for a version 2 manager,
each stroke to the region it starts in (in its coordinates, also when it ends in another), the
higher layer and then the newer region where they overlap, no stroke outside every region or
outside a clip, a stroke dropped when its region is removed mid-stroke, raw strokes and no
service traffic for a private region and a drawing region (reading off), no stroke in text mode,
values with their word layout, set words and gone strokes, data parts over 2048 bytes, a scroll
that sends the service nothing, a strike-through as the region's gesture, reports, stored ink
handed back (numbers, value, state; strokes alone), the units from the output's px per mm, 40
empty regions costing the service nothing, a region that becomes private forgotten and sending
nothing more, the region's ink (also a stroke written while the service was away, in writing
order) handed to a restarted service, and a service without regions getting no region op.

`output-density-test.sh` runs too (the host's C compiler): the output key and the person's
density file (`sway/glassine-output-density.c`).

`shm-seal-test.sh` runs too (curl, patch and a C compiler): it builds wlroots' `util/shm.c`
with our patches and checks that the pixman renderer's buffers take `F_SEAL_SHRINK`, as the
panel daemon requires, and that the backend's memfds (the cursor) allow sealing.
