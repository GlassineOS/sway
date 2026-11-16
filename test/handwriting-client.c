/* handwriting-client.c — the smallest client of glassine_handwriting_v1 for
 * handwriting-test.sh: binds the manager, prints "caps C" after each done, makes a surface
 * with neatening on and keeps running until stdin closes. */
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "glassine-handwriting-v1-client-protocol.h"

static struct wl_compositor *comp;
static struct glassine_handwriting_manager_v1 *mgr;
static uint32_t caps;

static void m_caps(void *d, struct glassine_handwriting_manager_v1 *m, uint32_t c) { caps = c; }
static void m_lang(void *d, struct glassine_handwriting_manager_v1 *m, const char *l) { printf("language %s\n", l); }
static void m_done(void *d, struct glassine_handwriting_manager_v1 *m) { printf("caps %u\n", caps); fflush(stdout); }
static const struct glassine_handwriting_manager_v1_listener ml = { m_caps, m_lang, m_done };

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	if (!strcmp(iface, glassine_handwriting_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name, &glassine_handwriting_manager_v1_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener rl = { global, global_remove };

int main(void)
{
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) return 1;
	wl_registry_add_listener(wl_display_get_registry(dpy), &rl, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !mgr) { printf("no global\n"); return 2; }
	glassine_handwriting_manager_v1_add_listener(mgr, &ml, NULL);
	struct wl_surface *s = wl_compositor_create_surface(comp);
	struct glassine_handwriting_v1 *h = glassine_handwriting_manager_v1_get_handwriting(mgr, s);
	glassine_handwriting_v1_set_neaten(h, 1, wl_fixed_from_double(0.5));
	wl_display_roundtrip(dpy);
	for (;;) {
		wl_display_flush(dpy);
		struct pollfd p[2] = { { wl_display_get_fd(dpy), POLLIN, 0 }, { 0, POLLIN, 0 } };
		if (poll(p, 2, -1) < 0) break;
		if (p[0].revents & POLLIN && wl_display_dispatch(dpy) < 0) break;
		if (p[0].revents & (POLLHUP | POLLERR) || p[1].revents) break;
	}
	return 0;
}
