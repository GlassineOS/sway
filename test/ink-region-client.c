/* ink-region-client: the scripted Wayland client of ink-popup-test.sh's writing region
 * cases. An app (an xdg toplevel, app_id ink-region) with a handwriting object
 * (glassine_handwriting_v1 version 2) on its surface and writing regions on it, without
 * any glassine_ink_v1 areas unless asked: the regions alone make its ink areas. Commands
 * on stdin, one per line (surface coordinates; nothing is committed unless said):
 *   win W H            the toplevel (W H unless configured), committed with a buffer
 *   r N X Y W H        region N (0..63; made on first use) at X Y, W x H (set_rect)
 *   clip N X Y W H     its clip; clip N none: the whole rectangle
 *   layer N L          its layer
 *   mode N ink|text    its mode
 *   all ink|text       every region's mode (the object's set_mode)
 *   kill N             destroy region N
 *   many N             regions 10..10+N-1: 20 x 10 each, in rows of 8, 10 apart
 *   inkarea X Y W H    a glassine_ink_v1 area of the app's own
 *   commit             commit the surface (a new frame of the same buffer)
 * It prints "ready", win-size WxH (a new size) and "ok" after each command once the
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
#include "glassine-handwriting-v1-client-protocol.h"
#include "glassine-ink-v1-client-protocol.h"

#define NR 64

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct glassine_handwriting_manager_v1 *hwm;
static struct glassine_ink_manager_v1 *inkm;
static struct wl_surface *win_s;
static struct xdg_surface *win_xs;
static struct xdg_toplevel *win_top;
static struct glassine_handwriting_v1 *hw;
static struct glassine_handwriting_region_v1 *reg[NR];
static struct glassine_ink_surface_v1 *ink;
static struct wl_buffer *buf;
static int win_w, win_h, win_bw, win_bh, win_cfg;

static struct wl_buffer *buffer(int w, int h)
{
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("ink-region", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		perror("memfd");
		exit(1);
	}
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	memset(p, 0xff, size);
	munmap(p, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t serial) { xdg_wm_base_pong(w, serial); }
static const struct xdg_wm_base_listener wm_l = { .ping = wm_ping };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	win_cfg = 1;
	if (win_bw && (win_w != win_bw || win_h != win_bh)) {   /* mapped: a new size (tiling) */
		win_bw = win_w;
		win_bh = win_h;
		buf = buffer(win_w, win_h);
		wl_surface_attach(win_s, buf, 0, 0);
		wl_surface_commit(win_s);
		printf("win-size %dx%d\n", win_w, win_h);
	}
}
static const struct xdg_surface_listener xs_l = { .configure = xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st)
{
	if (w > 0 && h > 0) {
		win_w = w;
		win_h = h;
	}
}
static void top_close(void *d, struct xdg_toplevel *t) {}
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) {}
static const struct xdg_toplevel_listener top_l = { .configure = top_configure, .close = top_close,
	.configure_bounds = top_bounds, .wm_capabilities = top_caps };

/* the handwriting objects' events don't matter here: no listener (libwayland drops them) */

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
#define BIND(var, intf, v) if (!strcmp(iface, intf.name)) var = wl_registry_bind(r, name, &intf, v)
	BIND(comp, wl_compositor_interface, 4);
	BIND(shm, wl_shm_interface, 1);
	BIND(wm, xdg_wm_base_interface, 1);
	BIND(hwm, glassine_handwriting_manager_v1_interface, 2);
	BIND(inkm, glassine_ink_manager_v1_interface, 1);
#undef BIND
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_l = { .global = reg_global, .global_remove = reg_remove };

static struct glassine_handwriting_region_v1 *region(int n)
{
	if (n < 0 || n >= NR) {
		return NULL;
	}
	if (!reg[n]) {
		char name[16];
		snprintf(name, sizeof name, "r%d", n);
		reg[n] = glassine_handwriting_v1_get_region(hw, name);
	}
	return reg[n];
}

static void set_clip(struct glassine_handwriting_region_v1 *r, int x, int y, int w, int h)
{
	struct wl_region *c = wl_compositor_create_region(comp);
	wl_region_add(c, x, y, w, h);
	glassine_handwriting_region_v1_set_clip(r, c);
	wl_region_destroy(c);
}

static void command(const char *l)
{
	int n, a, b, c, e;
	char m[16];
	struct glassine_handwriting_region_v1 *r;
	if (sscanf(l, "win %d %d", &a, &b) == 2) {
		win_w = a;
		win_h = b;
		win_s = wl_compositor_create_surface(comp);
		hw = glassine_handwriting_manager_v1_get_handwriting(hwm, win_s);
		win_xs = xdg_wm_base_get_xdg_surface(wm, win_s);
		xdg_surface_add_listener(win_xs, &xs_l, NULL);
		win_top = xdg_surface_get_toplevel(win_xs);
		xdg_toplevel_add_listener(win_top, &top_l, NULL);
		xdg_toplevel_set_app_id(win_top, "ink-region");
		wl_surface_commit(win_s);
		while (!win_cfg) {
			wl_display_roundtrip(dpy);
		}
		win_bw = win_w;
		win_bh = win_h;
		buf = buffer(win_w, win_h);
		wl_surface_attach(win_s, buf, 0, 0);
		wl_surface_commit(win_s);
		printf("win-size %dx%d\n", win_w, win_h);
	} else if (sscanf(l, "r %d %d %d %d %d", &n, &a, &b, &c, &e) == 5 && (r = region(n))) {
		glassine_handwriting_region_v1_set_rect(r, wl_fixed_from_int(a), wl_fixed_from_int(b),
			wl_fixed_from_int(c), wl_fixed_from_int(e));
	} else if (sscanf(l, "clip %d %d %d %d %d", &n, &a, &b, &c, &e) == 5 && (r = region(n))) {
		set_clip(r, a, b, c, e);
	} else if (sscanf(l, "clip %d %15s", &n, m) == 2 && !strcmp(m, "none") && (r = region(n))) {
		glassine_handwriting_region_v1_set_clip(r, NULL);
	} else if (sscanf(l, "layer %d %d", &n, &a) == 2 && (r = region(n))) {
		glassine_handwriting_region_v1_set_layer(r, a);
	} else if (sscanf(l, "mode %d %15s", &n, m) == 2 && (r = region(n))) {
		glassine_handwriting_region_v1_set_mode(r, !strcmp(m, "text") ? GLASSINE_HANDWRITING_V1_MODE_TEXT :
			GLASSINE_HANDWRITING_V1_MODE_INK);
	} else if (sscanf(l, "all %15s", m) == 1) {
		glassine_handwriting_v1_set_mode(hw, !strcmp(m, "text") ? GLASSINE_HANDWRITING_V1_MODE_TEXT :
			GLASSINE_HANDWRITING_V1_MODE_INK);
	} else if (sscanf(l, "kill %d", &n) == 1 && n >= 0 && n < NR && reg[n]) {
		glassine_handwriting_region_v1_destroy(reg[n]);
		reg[n] = NULL;
	} else if (sscanf(l, "many %d", &n) == 1 && n > 0 && n <= NR - 10) {
		for (int i = 0; i < n; i++) {
			glassine_handwriting_region_v1_set_rect(region(10 + i), wl_fixed_from_int(10 + i % 8 * 30),
				wl_fixed_from_int(10 + i / 8 * 20), wl_fixed_from_int(20), wl_fixed_from_int(10));
		}
	} else if (sscanf(l, "inkarea %d %d %d %d", &a, &b, &c, &e) == 4) {
		if (!ink) {
			ink = glassine_ink_manager_v1_get_ink_surface(inkm, win_s);
		}
		struct wl_region *ar = wl_compositor_create_region(comp);
		wl_region_add(ar, a, b, c, e);
		glassine_ink_surface_v1_set_areas(ink, ar);
		wl_region_destroy(ar);
	} else if (!strcmp(l, "commit")) {
		wl_surface_attach(win_s, buf, 0, 0);
		wl_surface_damage(win_s, 0, 0, 1, 1);
		wl_surface_commit(win_s);
	} else {
		printf("bad command: %s\n", l);
	}
	wl_display_roundtrip(dpy);
	printf("ok %s\n", l);
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "no wayland display\n");
		return 1;
	}
	struct wl_registry *rg = wl_display_get_registry(dpy);
	wl_registry_add_listener(rg, &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !wm || !hwm || !inkm) {
		fprintf(stderr, "missing a global\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm, &wm_l, NULL);
	wl_display_roundtrip(dpy);
	printf("ready\n");

	char line[256];
	size_t len = 0;
	struct pollfd p[2] = { { 0, POLLIN, 0 }, { wl_display_get_fd(dpy), POLLIN, 0 } };
	for (;;) {
		wl_display_flush(dpy);
		if (poll(p, 2, -1) < 0) {
			break;
		}
		if (p[1].revents & POLLIN) {
			if (wl_display_dispatch(dpy) < 0) {
				fprintf(stderr, "display error\n");
				return 1;
			}
		}
		if (p[0].revents & (POLLIN | POLLHUP)) {
			ssize_t k = read(0, line + len, sizeof line - 1 - len);
			if (k <= 0) {
				break;
			}
			len += k;
			char *nl;
			while ((nl = memchr(line, '\n', len))) {
				*nl = 0;
				if (nl > line) {
					command(line);
				}
				len -= nl + 1 - line;
				memmove(line, nl + 1, len);
			}
		}
	}
	return 0;
}
