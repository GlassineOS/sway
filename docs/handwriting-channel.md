# The handwriting service's channel: writing regions

What `sway/glassine-handwriting.c` sends the handwriting service and takes from it for the
writing regions of the pen interface's version 2 (`protocols/glassine-handwriting-v1.xml`,
GlassineOS/handwriting spec 005), in addition to the channel that GlassineOS/handwriting's
`docs/service.md` ("The compositor's channel") describes. JSON objects, one per line, as
there; neither side waits for an answer.

`s` names a surface, `r` a region (numbered by the compositor, never reused while it runs),
`k` a stroke by the region's own numbers. Points and coordinates are in the region's
coordinates: from its top left corner, so a scroll changes none of them.

The compositor sends region ops only to a service whose `hello` has `"regions": true`. With
a service that lacks it, regions still route raw strokes to the app and nothing reaches the
service; the pen interface's `regions` capability is off.

## From the compositor

| `op` | Fields | Meaning |
|---|---|---|
| `surface` | `s`, `sandbox` or `pid` | as before, sent before the surface's first region. A surface in region mode gets no `set`, `stroke`, `clear` or `report`; its `destroy` ends its regions too. A surface that was used without regions first gets `destroy` when it makes its first region, and `surface` again with that region |
| `region` | `s`, `r`, `name` | a region that holds writing (its first stroke, edit or restore) and wants the service (reading or neatening on, not private). An empty region is never sent (FR-007) |
| `region_set` | `s`, `r`, `reading`, `neaten`, `strength`, `purpose`, `hints`, `languages`, `guides` ([spacing, first]), `size` ([w, h]: the writing area is [0, 0, w, h]), `units` | right after `region`, and again when a choice, the size or the units change. A move alone sends nothing (FR-003, SC-004) |
| `region_mode` | `s`, `r`, `mode` (`ink`, `text`) | the region's mode changed; also right after `region_set` for a region announced in text mode |
| `region_stroke` | `s`, `r`, `k`, `points` ([x, y, t, pressure, tilt_x, tilt_y]) | a raw stroke, at pen-up |
| `region_edit` | `s`, `r`, `text` | the person's keyboard text: the region's new value, a correction of the words whose ink it changed (FR-017) |
| `region_report` | `s`, `r`, `serial`, `action` (`picked`, `accepted`, `corrected`), `rank`, `text` | a report on the value `serial` or its candidates. The compositor answers by itself (`not_kept`, `unavailable` or `stale`) a report on a private region, a `discarded` one, one without the service, one with reading off, and one on a region the service doesn't hold |
| `region_restore` | `s`, `r`, `strokes` ([{`k`, `points`}]), `value` (string; absent: none), `state` (base64; absent: none) | stored ink handed back: it replaces what the region holds. The compositor also sends it by itself after a service restart (below) |
| `region_destroy` | `s`, `r` | forget the region at once: it was removed, became private, or has reading and neatening both off |

## From the service

| `op` | Fields | Meaning |
|---|---|---|
| `hello` | as before, plus `regions` (true) | the service reads regions |
| `region_neat` | `s`, `r`, `k`, `how` (`kept`, `line`, `pulled`), `points` ([x, y] per raw point) | a neatened stroke, a raw or a restored one; passed on only while the region's neatening is on |
| `region_value` | `s`, `r`, `serial`, `text`, `words`, `gone` ([k]), `state` (base64) | the region's whole value and its layout, one update. `serial` grows with each value of the region. `words` in the text's order: an ink word `{start, length, strokes: [k], dx, dy}`, a set word `{start, length, box: [x0, y0, x1, y1], path}`; `start` and `length` in UTF-8 bytes of `text`. `gone`: strokes that left the region for good; the compositor stops holding them |
| `region_candidates` | `s`, `r`, `serial` (the value's), `items` ([{`text`, `from`, `fit`}]) | the ranked alternatives at a pause |
| `region_gesture` | `s`, `r`, `serial` (the value that carries its effect), `kind` (`strike`, `scratch`, `caret`), `strokes` ([k]) | a gesture written in the region |
| `region_report` | `s`, `r`, `serial`, `result` (as `report`) | the answer to a `region_report` |

`path`: a flat array of ops, each a letter followed by its coordinates: `"M", x, y`,
`"L", x, y`, `"Q", cx, cy, x, y`, `"C", c1x, c1y, c2x, c2y, x, y`, `"Z"`; filled with the
non-zero rule. A value whose text isn't UTF-8 or whose state isn't base64 is dropped, as is a
set word whose path doesn't parse.

## A service restart

The compositor holds, per region that wants the service, its raw and restored strokes in
writing order (less those named in `gone`), the last value's text and the last state; also
what is written, restored or edited while the service is away (starting, or in the seconds
before it is started again). After the service says hello it sends `surface`, `region`,
`region_set`, one `region_restore` with them and then the held edit as `region_edit`, so the
region's writing survives a restart without the app. It holds nothing for a private region
(what it held is dropped when the region becomes private), a drawing region (reading and
neatening off), with no service configured, once the service has failed to start five times,
or when the service's hello has no `regions`.

## Units

`units` is `device:PX_PER_MM`, with the logical px per mm of the output the pen writes on: its
mode's px over its physical width, over its scale; for an output of unknown size the person's
density over the scale, from `$XDG_CONFIG_HOME/glassine/output-density` (written by Settings:
`DENSITY OUTPUT-KEY` lines, the key the output's make, model and serial, else its connector
name; `sway/glassine-output-density.h`). Without either it is `device`, and the service applies
the default density its documentation states (spec 004 FR-017).
