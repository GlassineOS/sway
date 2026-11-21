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
| `sway/glassine-ink-area.c`, `protocols/glassine-ink-v1.xml` | the pen's ink areas: on windows, input-method popups (the keyboard's handwriting paper) and layer surfaces, each owner with a key that lasts its lifetime; the panel daemon inks a stroke only if it starts in an area, and then only on that owner's part of the panel, past a writing region's edge over the visible window (GlassineOS/epd `epd-inkarea.h`) |
| `sway/glassine-handwriting.*`, `sway/glassine-handwriting-sway.*`, `protocols/glassine-handwriting-v1.xml` | the pen interface of handwriting (GlassineOS/handwriting spec 004): an app's pen strokes as numbered vector strokes, with the handwriting service (started from here, its channel on fd 3) their neatened versions, readings and candidates; `glassine-handwriting.c` uses libwayland-server and json-c only, `-sway.c` feeds it the tablet tool and the security context's app id. `GLASSINE_HANDWRITING_SERVICE` names the service (empty: none). Version 2: writing regions (spec 005), each stroke routed to the topmost region it starts in, in region coordinates; every region in ink mode is also an ink area of its surface (`glassine-ink-area.c` asks `glassine_hw_regions`), so the pen's raw ink shows there without the app registering areas; the channel's region ops are in `docs/handwriting-channel.md` |
| `sway/glassine-output-density.*` | the person's px per mm of an output of unknown size, from `~/.config/glassine/output-density` (written by Settings), for the pen interface's units (handwriting spec 004 FR-017); and which reported physical sizes count (GlassineOS/shell spec 012 FR-006): an output whose reported size is implausible is of unknown size |
| `sway/glassine-view-px.*`, `wlroots/0010-pixel-trees.patch` | fractional output scales without resampling, see "Fractional output scales" below |
| `wlroots/` | our wlroots 0.18.2 patches; `setup.sh` puts them in a subproject, linked statically |
| `build-glassine.sh` | arm64 build on Alpine 3.24 |
| `test/` | host tests, see `test/README.md` (`gesture-action-test.sh`, `gesture-edge-test.sh`, `shm-seal-test.sh`, `ink-popup-test.sh`, `handwriting-test.sh`, `handwriting-regions-test.sh`, `output-density-test.sh` and `ui-scale-test.sh` run; the rest not ported yet) |

## Fractional output scales

An output's scale F may be fractional (`output NAME scale F`, kept in 120ths). Nothing a
client draws is resampled for it:

- **A client with fractional scale** (it made a `wp_fractional_scale_v1` object for the
  surface) is told F and draws buffers of round(size × F) pixels, as in upstream sway. Where
  such a buffer is a pixel wider or higher than its box in output pixels (the scene rounds
  a box's edges, the client its size), the extra row or column is cut; where it is a pixel
  short, its last row or column is drawn twice. It is never stretched.
- **A client that takes only whole scales** (no such object on its toplevel's surface when
  the window maps) is told the nearest whole number N to F, at least 1, as `wl_output.scale`
  and as the preferred buffer scale (upstream: the next whole number up), and its window is a
  *pixel view*: it is shown pixel for pixel, one buffer pixel on one output pixel. The window
  then has coordinates of its own, *units*: one unit is N output pixels, F/N units make a
  layout px. Everything the client is told or tells is in units: the configure size (the
  box's output pixels over N, rounded up; the cut part of the last unit is not shown), its
  geometry and size hints, its subsurfaces' and popups' positions, the popup's constraint
  box, pointer, touch and tablet positions, pointer constraints, the text field's cursor
  rectangle, ink areas, writing regions and pen strokes. A client that wants a size in
  layout px (design px) draws F/N of its units per layout px; it sees only N, so F has to
  reach it another way (the session's published scale).
- At a whole F, and wherever N equals F, a window takes sway's own path: nothing differs
  from a whole scale before.

The pen interface's points per mm stay the output's density over its scale; while the pen
writes on a pixel view they are the density over N, as the strokes are in units.

Limits: layer surfaces, input-method popups, lock surfaces and drag icons of whole-scale-only
clients are told N too but are still scaled by the renderer (from N to F); Xwayland windows
are scaled as before (Xwayland is told the nearest whole scale); a window that lies on two
outputs of different scales is pixel for pixel only on the one its workspace is on; an ink
area's edge on a pixel view can be a layout px off; `glassine_ink` command boxes on a pixel
view are in its units. An output's layout size is its pixels over F cut to whole layout px,
so up to a layout px at its right and bottom edge shows nothing (as in upstream sway).

`get_outputs` carries `glassine_physical_width` and `glassine_physical_height`: the size in
mm the output reports (0: none), unjudged.

**Status:** not what devices run yet. Builds still come from the private development
tree, which also still uses older runtime names. Those names change for both at the
cutover, so this repository doesn't work against a running system today.

Written largely by AI agents and not reviewed by anyone who knows these subsystems well.
