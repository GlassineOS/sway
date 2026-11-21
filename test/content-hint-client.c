/* content-hint-client: the scripted Wayland client of content-hint-test.sh. An app (an
 * xdg toplevel, app_id content-hint) with a glassine_ink_v1 object on its surface, bound
 * at the version given (argv[1], default 4), that says what its commits are
 * (glassine_ink_surface_v1.set_content, version 4). Commands on stdin, one per line
 * (buffer = surface coordinates, scale 1):
 *   win W H                    the toplevel, W x H, committed with a buffer
 *   paint X Y W H KIND         a new grey in that rect, damaged, committed after
 *                              set_content KIND: none (no request), page, motion,
 *                              or a number (sent as it is)
 *   say KIND                   set_content alone, then a commit without damage
 * It prints "ink version N", "ready" and "ok CMD" after each command once the
 * compositor has seen it (a roundtrip). */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "glassine-ink-v1-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct glassine_ink_manager_v1 *inkm;
static struct glassine_ink_surface_v1 *ink;
static struct wl_surface *surf;
static struct xdg_surface *xs;
static struct xdg_toplevel *top;
static struct wl_buffer *buf;
static uint32_t *px;
static int W, H, configured, want = 4, have;
static unsigned grey = 0x20;

static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t serial) { xdg_wm_base_pong(w, serial); }
static const struct xdg_wm_base_listener wm_l = { .ping = wm_ping };
static void xs_configure(void *d, struct xdg_surface *x, uint32_t serial)
{
	xdg_surface_ack_configure(x, serial);
	configured = 1;
}
static const struct xdg_surface_listener xs_l = { .configure = xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {}
static void top_close(void *d, struct xdg_toplevel *t) {}
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) {}
static const struct xdg_toplevel_listener top_l = { .configure = top_configure, .close = top_close,
	.configure_bounds = top_bounds, .wm_capabilities = top_caps };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_compositor_interface.name))
		comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name))
		wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	else if (!strcmp(iface, glassine_ink_manager_v1_interface.name)) {
		have = (int)ver < want ? (int)ver : want;
		inkm = wl_registry_bind(r, name, &glassine_ink_manager_v1_interface, have);
		printf("ink version %u\n", ver);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_l = { .global = reg_global, .global_remove = reg_remove };

static uint32_t kind_of(const char *k)
{
	return !strcmp(k, "page") ? GLASSINE_INK_SURFACE_V1_CONTENT_PAGE :
		!strcmp(k, "motion") ? GLASSINE_INK_SURFACE_V1_CONTENT_MOTION : (uint32_t)atoi(k);
}

static void say(const char *k)
{
	if (strcmp(k, "none") && have >= 4)
		glassine_ink_surface_v1_set_content(ink, kind_of(k));
}

static void command(const char *l)
{
	int x, y, w, h;
	char k[32];
	if (sscanf(l, "win %d %d", &w, &h) == 2) {
		W = w, H = h;
		int size = W * H * 4, fd = memfd_create("content-hint", MFD_CLOEXEC);
		if (fd < 0 || ftruncate(fd, size) < 0) {
			perror("memfd");
			exit(1);
		}
		px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		memset(px, 0xff, size);
		struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
		buf = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4, WL_SHM_FORMAT_XRGB8888);
		wl_shm_pool_destroy(pool);
		surf = wl_compositor_create_surface(comp);
		xs = xdg_wm_base_get_xdg_surface(wm, surf);
		xdg_surface_add_listener(xs, &xs_l, NULL);
		top = xdg_surface_get_toplevel(xs);
		xdg_toplevel_add_listener(top, &top_l, NULL);
		xdg_toplevel_set_app_id(top, "content-hint");
		ink = glassine_ink_manager_v1_get_ink_surface(inkm, surf);
		wl_surface_commit(surf);
		while (!configured)
			wl_display_roundtrip(dpy);
		wl_surface_attach(surf, buf, 0, 0);
		wl_surface_damage_buffer(surf, 0, 0, W, H);
		wl_surface_commit(surf);
	} else if (sscanf(l, "paint %d %d %d %d %31s", &x, &y, &w, &h, k) == 5) {
		grey = (grey + 0x30) & 0xff;
		for (int j = y; j < y + h && j < H; j++)
			for (int i = x; i < x + w && i < W; i++)
				px[j * W + i] = 0xff000000u | grey * 0x010101u;
		wl_surface_attach(surf, buf, 0, 0);
		wl_surface_damage_buffer(surf, x, y, w, h);
		say(k);
		wl_surface_commit(surf);
	} else if (sscanf(l, "say %31s", k) == 1) {
		say(k);
		wl_surface_commit(surf);
	} else
		printf("bad command: %s\n", l);
	wl_display_roundtrip(dpy);
	printf("ok %s\n", l);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc > 1)
		want = atoi(argv[1]);
	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "no wayland display\n");
		return 1;
	}
	struct wl_registry *rg = wl_display_get_registry(dpy);
	wl_registry_add_listener(rg, &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !wm || !inkm) {
		fprintf(stderr, "missing a global\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm, &wm_l, NULL);
	wl_display_roundtrip(dpy);
	printf("ready\n");
	char line[256];
	size_t n = 0;
	struct pollfd pf[2] = { { 0, POLLIN, 0 }, { wl_display_get_fd(dpy), POLLIN, 0 } };
	for (;;) {
		wl_display_flush(dpy);
		if (poll(pf, 2, -1) < 0)
			break;
		if (pf[1].revents & POLLIN && wl_display_dispatch(dpy) < 0) {
			fprintf(stderr, "display error\n");
			return 1;
		}
		if (pf[0].revents & (POLLIN | POLLHUP)) {
			char c;
			ssize_t r = read(0, &c, 1);
			if (r <= 0)
				break;
			if (c == '\n') {
				line[n] = 0;
				n = 0;
				command(line);
			} else if (n < sizeof line - 1)
				line[n++] = c;
		}
	}
	return 0;
}
