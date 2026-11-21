# GlassineOS sway

Sway 1.10.1 with the patches GlassineOS needs for its e-ink panel, as separate commits on
branch `glassine/1.10.1` on top of upstream's tag. Releases are tagged
`<ver>-glassine<N>`. Upstream's `README.md` still describes sway itself.

Our additions are MIT, like sway (`LICENSE`), except `protocols/glassine-ink-v1.xml` and
`protocols/glassine-handwriting-v1.xml`, which are GPL-2.0-or-later like their main copies in
[GlassineOS/protocols](https://github.com/GlassineOS/protocols).

| Path | What |
|---|---|
| `sway/glassine-epd-backend.c`, `sway/glassine-epd-output-proto.h` | the output backend that hands frames to the panel daemon ([GlassineOS/epd](https://github.com/GlassineOS/epd)) |
| `sway/glassine-gesture.*`, `sway/glassine-popup-hint.*` | touch gestures (3/4-finger swipes, two-finger tap, edge swipes), popup hints |
| `sway/glassine-gesture-action.*` | the gestures file's action ids (old ones read and mapped; `show-bar` for the bar over an immersive window), defaults, the edge lines, the exit guard, and the request each action sends |
| `sway/glassine-ink-area.c`, `protocols/glassine-ink-v1.xml` | the pen's ink areas: on windows, input-method popups (the keyboard's handwriting paper) and layer surfaces |
| `sway/glassine-handwriting.*`, `sway/glassine-handwriting-sway.*`, `protocols/glassine-handwriting-v1.xml` | the pen interface of handwriting (GlassineOS/handwriting spec 004): an app's pen strokes as numbered vector strokes, with the handwriting service (started from here, its channel on fd 3) their neatened versions, readings and candidates; `glassine-handwriting.c` uses libwayland-server and json-c only, `-sway.c` feeds it the tablet tool and the security context's app id. `GLASSINE_HANDWRITING_SERVICE` names the service (empty: none). Version 2: writing regions (spec 005), each stroke routed to the topmost region it starts in, in region coordinates; every region in ink mode is also an ink area of its surface (`glassine-ink-area.c` asks `glassine_hw_regions`), so the pen's raw ink shows there without the app registering areas; the channel's region ops are in `docs/handwriting-channel.md` |
| `sway/glassine-output-density.*` | the person's px per mm of an output of unknown size, from `~/.config/glassine/output-density` (written by Settings), for the pen interface's units (handwriting spec 004 FR-017) |
| `wlroots/` | our wlroots 0.18.2 patches; `setup.sh` puts them in a subproject, linked statically |
| `build-glassine.sh` | arm64 build on Alpine 3.24 |
| `test/` | host tests, see `test/README.md` (`gesture-action-test.sh`, `gesture-edge-test.sh`, `shm-seal-test.sh`, `ink-popup-test.sh`, `handwriting-test.sh`, `handwriting-regions-test.sh` and `output-density-test.sh` run; the rest not ported yet) |

**Status:** not what devices run yet. Builds still come from the private development
tree, which also still uses older runtime names. Those names change for both at the
cutover, so this repository doesn't work against a running system today.

Written largely by AI agents and not reviewed by anyone who knows these subsystems well.
