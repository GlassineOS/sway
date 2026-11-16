/* glassine-handwriting-sway.c — sway's side of the pen interface (glassine-handwriting.c):
 * the identity from the security context, wl_region contents, the output's px per mm, and
 * the tablet tool's events. A stroke starts where the tip touches a surface with a
 * handwriting object and ends where it lifts, also when sway sends the app emulated pointer
 * events instead of tablet ones. The eraser end writes nothing here.
 *
 * GLASSINE_HANDWRITING_SERVICE names the service's program (default
 * glassine-handwriting-service on PATH); empty: no service, raw strokes only. */
#include <math.h>
#include <pixman.h>
#include <stdlib.h>
#include <string.h>
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
	};
	const char *cmd = getenv("GLASSINE_HANDWRITING_SERVICE");
	glassine_hw_init(display, &hooks, cmd ? cmd : "glassine-handwriting-service");
}

static void per_mm(struct sway_cursor *cursor)
{
	/* the mode's px over the panel's mm, both as the mode is (unturned), in logical px */
	struct wlr_output *o = wlr_output_layout_output_at(root->output_layout, cursor->cursor->x, cursor->cursor->y);
	if (!o || o->phys_width <= 0 || o->width <= 0) return;
	glassine_hw_set_per_mm(o->width / (o->scale > 0 ? o->scale : 1) / o->phys_width);
}

void glassine_hw_sway_tip(struct sway_cursor *cursor, struct sway_tablet_tool *tool, struct wlr_surface *surface,
			  double sx, double sy, uint32_t time, bool down)
{
	if (!down) {
		glassine_hw_pen_up(time);
		return;
	}
	if (tool->tablet_v2_tool->wlr_tool->type == WLR_TABLET_TOOL_TYPE_ERASER) return;
	per_mm(cursor);
	glassine_hw_axis(NAN, tool->tilt_x, tool->tilt_y);
	glassine_hw_pen_down(surface ? surface->resource : NULL, sx, sy, cursor->cursor->x, cursor->cursor->y, time);
}

void glassine_hw_sway_axis(struct sway_cursor *cursor, struct sway_tablet_tool *tool, double pressure, uint32_t time)
{
	glassine_hw_axis(pressure, tool->tilt_x, tool->tilt_y);
	if (glassine_hw_pen_active()) glassine_hw_pen_motion(cursor->cursor->x, cursor->cursor->y, time);
}

void glassine_hw_sway_out(uint32_t time)
{
	glassine_hw_pen_up(time);
}
