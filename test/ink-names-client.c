/*
 * ink-names-client: binds the ink manager, makes a brush and an ink surface, sets
 * the brush, and exits 0 only if the compositor raised no protocol error. It's
 * written against glassine_ink_v1; ink-names.sh also builds it with the old name.
 */
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include "ink-client-protocol.h"

static struct wl_compositor *comp;
static struct glassine_ink_manager_v1 *mgr;

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	(void)d;
	if (!strcmp(iface, wl_compositor_interface.name))
		comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, glassine_ink_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name, &glassine_ink_manager_v1_interface, ver < 3 ? ver : 3);
}

static void global_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)d; (void)r; (void)name;
}

static const struct wl_registry_listener listener = { global, global_remove };

int main(void)
{
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		puts("FAIL no display");
		return 2;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &listener, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !mgr) {
		printf("FAIL %s not offered\n", glassine_ink_manager_v1_interface.name);
		return 1;
	}
	struct wl_surface *s = wl_compositor_create_surface(comp);
	struct glassine_ink_brush_v1 *b = glassine_ink_manager_v1_create_brush(mgr);
	struct glassine_ink_surface_v1 *i = glassine_ink_manager_v1_get_ink_surface(mgr, s);
	glassine_ink_surface_v1_set_brush_object(i, GLASSINE_INK_SURFACE_V1_TOOL_TIP, b, wl_fixed_from_int(2), wl_fixed_from_int(8));
	glassine_ink_surface_v1_set_brush_object(i, GLASSINE_INK_SURFACE_V1_TOOL_TIP, NULL, 0, 0);
	glassine_ink_brush_v1_destroy(b);
	glassine_ink_surface_v1_destroy(i);
	wl_display_roundtrip(dpy);
	int err = wl_display_get_error(dpy);
	if (err) {
		printf("FAIL %s: protocol error %d\n", glassine_ink_manager_v1_interface.name, err);
		return 1;
	}
	printf("ok %s\n", glassine_ink_manager_v1_interface.name);
	return 0;
}
