/* glassine-handwriting-sway.c — sway's side of the pen interface (glassine-handwriting.c):
 * the identity from the security context, wl_region contents, the output's px per mm, the
 * tablet tool's events, and the ink areas told when the writing regions change. A stroke
 * starts where the tip touches a surface with a handwriting object and ends where it lifts,
 * also when sway sends the app emulated pointer events instead of tablet ones. The eraser
 * end writes nothing here.
 *
 * The ink's units: the logical px per mm of the output the pen writes on (its density over
 * its scale, whatever the scale), from its physical size when that is plausible
 * (glassine_od_size_plausible), else from the person's density for that output
 * (glassine-output-density.h, kept by Settings), else none: the units stay the pen's own
 * (spec 004 FR-017). A stroke is in its surface's coordinates. The surface of a pixel view
 * (glassine-view-px.h: a whole-scale-only client on an output of fractional scale) counts in
 * units of N output pixels, not in logical px, so while the pen writes on one the units are
 * the density over N: a millimetre stays a millimetre.
 *
 * GLASSINE_HANDWRITING_SERVICE names the service's program (default
 * glassine-handwriting-service on PATH); empty: no service, raw strokes only. */
#include <math.h>
#include <pixman.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>
#include "sway/input/cursor.h"
#include "sway/input/tablet.h"
#include "sway/server.h"
#include "sway/tree/root.h"
#include "glassine-handwriting.h"
#include "glassine-handwriting-sway.h"
#include "glassine-ink-area.h"
#include "glassine-output-density.h"
#include "glassine-view-px.h"

static void identify(struct wl_client *client, char *buf, size_t size)
{
	buf[0] = 0;
	if (!server.security_context_manager_v1) return;
	const struct wlr_security_context_v1_state *s =
		wlr_security_context_manager_v1_lookup_client(server.security_context_manager_v1, client);
	if (s && s->app_id && *s->app_id) snprintf(buf, size, "%s", s->app_id);
	else if (s) snprintf(buf, size, "unknown");     /* sandboxed, without a name: no name it could pick */
}

static void *region_copy(struct wl_resource *res)
{
	const pixman_region32_t *src = wlr_region_from_resource(res);
	pixman_region32_t *r = malloc(sizeof *r);
	if (!r) return NULL;
	pixman_region32_init(r);
	pixman_region32_copy(r, (pixman_region32_t *)src);
	return r;
}

static bool region_contains(void *region, double x, double y)
{
	return pixman_region32_contains_point(region, (int)floor(x), (int)floor(y), NULL);
}

static void region_free(void *region)
{
	pixman_region32_fini(region);
	free(region);
}

static void region_box(void *region, int box[4])
{
	const pixman_box32_t *e = pixman_region32_extents(region);
	box[0] = e->x1; box[1] = e->y1; box[2] = e->x2; box[3] = e->y2;
}

void glassine_hw_sway_init(struct wl_display *display)
{
	static const struct glassine_hw_hooks hooks = {
		.identify = identify, .region_copy = region_copy, .region_contains = region_contains,
		.region_free = region_free, .region_box = region_box,
		.regions_changed = glassine_ink_changed,       /* the regions are ink areas */
	};
	const char *cmd = getenv("GLASSINE_HANDWRITING_SERVICE");
	glassine_hw_init(display, &hooks, cmd ? cmd : "glassine-handwriting-service");
}

/* the person's density for an output of unknown size: the file read again when it changed */
static double setting(struct wlr_output *o)
{
	static char path[4096], key[512];
	static struct timespec mtime;
	static off_t size = -1;
	static double density;
	char p[4096], k[512];
	glassine_od_file(p, sizeof p);
	glassine_od_key(o->make, o->model, o->serial, o->name, k, sizeof k);
	struct stat st;
	if (stat(p, &st)) return 0;
	if (strcmp(p, path) || strcmp(k, key) || st.st_size != size || st.st_mtim.tv_sec != mtime.tv_sec ||
	    st.st_mtim.tv_nsec != mtime.tv_nsec) {
		snprintf(path, sizeof path, "%s", p);
		snprintf(key, sizeof key, "%s", k);
		mtime = st.st_mtim;
		size = st.st_size;
		density = glassine_od_lookup(p, k);
	}
	return density;
}

/* units per layout px of the surface the stroke is on (1 unless a pixel view's) */
static double pen_units = 1;

static void per_mm(struct sway_cursor *cursor)
{
	/* the mode's px over the panel's mm, both as the mode is (unturned), in logical px */
	struct wlr_output *o = wlr_output_layout_output_at(root->output_layout, cursor->cursor->x, cursor->cursor->y);
	if (!o || o->width <= 0) return;
	double scale = o->scale > 0 ? o->scale : 1;
	if (glassine_od_size_plausible(o->width, o->height, o->phys_width, o->phys_height)) {
		glassine_hw_set_per_mm(o->width / scale / o->phys_width * pen_units);
		return;
	}
	double d = setting(o);
	glassine_hw_set_per_mm(d > 0 ? d / scale * pen_units : 0);
}

void glassine_hw_sway_tip(struct sway_cursor *cursor, struct sway_tablet_tool *tool, struct wlr_surface *surface,
			  double sx, double sy, uint32_t time, bool down)
{
	if (!down) {
		glassine_hw_pen_up(time);
		return;
	}
	if (tool->tablet_v2_tool->wlr_tool->type == WLR_TABLET_TOOL_TYPE_ERASER) return;
	pen_units = glassine_px_surface_units(surface);
	per_mm(cursor);
	glassine_hw_axis(NAN, tool->tilt_x, tool->tilt_y);
	glassine_hw_pen_down(surface ? surface->resource : NULL, sx, sy, cursor->cursor->x * pen_units,
			     cursor->cursor->y * pen_units, time);
}

void glassine_hw_sway_axis(struct sway_cursor *cursor, struct sway_tablet_tool *tool, double pressure, uint32_t time)
{
	glassine_hw_axis(pressure, tool->tilt_x, tool->tilt_y);
	if (glassine_hw_pen_active()) glassine_hw_pen_motion(cursor->cursor->x * pen_units, cursor->cursor->y * pen_units, time);
}

void glassine_hw_sway_out(uint32_t time)
{
	glassine_hw_pen_up(time);
}
