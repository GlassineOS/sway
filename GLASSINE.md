# GlassineOS sway

Sway 1.10.1 with the patches GlassineOS needs for its e-ink panel, as separate commits on
branch `glassine/1.10.1` on top of upstream's tag. Releases are tagged
`<ver>-glassine<N>`. Upstream's `README.md` still describes sway itself.

| Path | What |
|---|---|
| `sway/glassine-epd-backend.c`, `sway/glassine-epd-output-proto.h` | the output backend that hands frames to the panel daemon ([GlassineOS/epd](https://github.com/GlassineOS/epd)) |
| `sway/glassine-gesture.*`, `sway/glassine-popup-hint.*` | touch gestures, popup hints |
| `sway/glassine-ink-area.c`, `protocols/glassine-ink-v1.xml` | the pen's ink areas |
| `wlroots/` | our wlroots 0.18.2 patches; `setup.sh` puts them in a subproject, linked statically |
| `build-glassine.sh` | arm64 build on Alpine 3.24 |
| `test/` | host tests, see `test/README.md` (not ported yet) |

**Status:** not what devices run yet. Builds still come from the private development
tree, which also still uses older runtime names. Those names change for both at the
cutover, so this repository doesn't work against a running system today.

Written largely by AI agents and not reviewed by anyone who knows these subsystems well.
