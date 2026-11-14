/* ink-popup-client: the scripted Wayland client of ink-popup-test.sh. One
 * client plays an app (an xdg toplevel or a layer surface with a text field,
 * text-input-v3), an input method (input-method-v2 popups, like the on-screen
 * keyboard's handwriting paper) and a pointer (wlr virtual pointer, standing in
 * for the pen that sway turns into a pointer). Commands on stdin, one per line:
 *   win W H            an xdg toplevel (app_id ink-test; W H unless configured),
 *                      ink area: the left half of its first size
 *   layer H            a layer surface (top layer, bottom edge, namespace
 *                      ink-test-layer, keyboard on demand), ink area: all of it
 *   ti X Y             enable the text input on the entered surface, cursor X Y 2x20
 *   cur X Y            move the cursor rectangle (the popups follow at their next commit)
 *   tioff              disable it
 *   popup N W H A      input-method popup N (0..3) of WxH; A 1: ink area all of
 *                      it + a pen brush, A 0: an ink object without areas
 *   hide N / show N    unmap (null buffer) / map again
 *   kill N             destroy popup N
 *   ptr X Y            pointer to layout X Y (extent: the output's layout size)
 *   down / up          left button
 * It prints what it gets: ti-enter/ti-leave, im-activate/im-deactivate,
 * ptr-enter/ptr-leave/ptr-button/ptr-motion with the surface's name, and
 * "ok" after each command once the compositor has seen it (a roundtrip). */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "glassine-ink-v1-client-protocol.h"

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm;
static struct zwp_text_input_manager_v3 *tim;
static struct zwp_input_method_manager_v2 *imm;
static struct zwlr_virtual_pointer_manager_v1 *vpm;
static struct zwlr_layer_shell_v1 *lsh;
static struct glassine_ink_manager_v1 *inkm;
static struct wl_output *out;

static struct wl_surface *win_s, *layer_s, *pop_s[4], *ti_surface;
static struct xdg_surface *win_xs;
static struct xdg_toplevel *win_top;
static struct zwlr_layer_surface_v1 *layer_ls;
static struct zwp_input_popup_surface_v2 *pop[4];
static struct glassine_ink_surface_v1 *pop_ink[4];
static int pop_w[4], pop_h[4];
static struct zwp_text_input_v3 *ti;
static struct zwp_input_method_v2 *im;
static struct zwlr_virtual_pointer_v1 *vp;
static struct wl_pointer *pointer;
static int win_w, win_h, win_cfg, layer_w, layer_h, layer_cfg;
static int out_w, out_h, out_scale = 1;

static const char *name_of(struct wl_surface *s)
{
	static char b[16];
	if (s && s == win_s) return "win";
	if (s && s == layer_s) return "layer";
	for (int i = 0; i < 4; i++) {
		if (s && s == pop_s[i]) {
			snprintf(b, sizeof b, "popup%d", i);
			return b;
		}
	}
	return "other";
}

static struct wl_buffer *buffer(int w, int h)
{
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("ink-popup", MFD_CLOEXEC);
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

static void ink_area(struct wl_surface *s, int x, int y, int w, int h)
{
	struct glassine_ink_surface_v1 *i = glassine_ink_manager_v1_get_ink_surface(inkm, s);
	struct wl_region *r = wl_compositor_create_region(comp);
	wl_region_add(r, x, y, w, h);
	glassine_ink_surface_v1_set_areas(i, r);
	wl_region_destroy(r);
	glassine_ink_surface_v1_set_brush(i, GLASSINE_INK_SURFACE_V1_TOOL_TIP, GLASSINE_INK_SURFACE_V1_KIND_PEN,
		wl_fixed_from_int(1), wl_fixed_from_int(3), wl_fixed_from_int(1));
}

/* ---- listeners ---- */
static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t serial) { xdg_wm_base_pong(w, serial); }
static const struct xdg_wm_base_listener wm_l = { .ping = wm_ping };

static int win_bw, win_bh;      /* the buffer's size */
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	win_cfg = 1;
	if (win_bw && (win_w != win_bw || win_h != win_bh)) {   /* mapped: a new size (tiling) */
		win_bw = win_w;
		win_bh = win_h;
		wl_surface_attach(win_s, buffer(win_w, win_h), 0, 0);
		wl_surface_commit(win_s);
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

static void ls_configure(void *d, struct zwlr_layer_surface_v1 *l, uint32_t serial, uint32_t w, uint32_t h)
{
	zwlr_layer_surface_v1_ack_configure(l, serial);
	layer_w = w;
	layer_h = h;
	layer_cfg = 1;
}
static void ls_closed(void *d, struct zwlr_layer_surface_v1 *l) {}
static const struct zwlr_layer_surface_v1_listener ls_l = { .configure = ls_configure, .closed = ls_closed };

static void ti_enter(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s)
{
	ti_surface = s;
	printf("ti-enter %s\n", name_of(s));
}
static void ti_leave(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s)
{
	printf("ti-leave %s\n", name_of(s));
	ti_surface = NULL;
}
static void ti_preedit(void *d, struct zwp_text_input_v3 *t, const char *x, int32_t b, int32_t e) {}
static void ti_commit(void *d, struct zwp_text_input_v3 *t, const char *x) {}
static void ti_delete(void *d, struct zwp_text_input_v3 *t, uint32_t b, uint32_t a) {}
static void ti_done(void *d, struct zwp_text_input_v3 *t, uint32_t serial) {}
static const struct zwp_text_input_v3_listener ti_l = { .enter = ti_enter, .leave = ti_leave,
	.preedit_string = ti_preedit, .commit_string = ti_commit, .delete_surrounding_text = ti_delete, .done = ti_done };

static void im_activate(void *d, struct zwp_input_method_v2 *m) { printf("im-activate\n"); }
static void im_deactivate(void *d, struct zwp_input_method_v2 *m) { printf("im-deactivate\n"); }
static void im_surrounding(void *d, struct zwp_input_method_v2 *m, const char *t, uint32_t c, uint32_t a) {}
static void im_cause(void *d, struct zwp_input_method_v2 *m, uint32_t c) {}
static void im_purpose(void *d, struct zwp_input_method_v2 *m, uint32_t h, uint32_t p) {}
static void im_done(void *d, struct zwp_input_method_v2 *m) {}
static void im_unavailable(void *d, struct zwp_input_method_v2 *m) { printf("im-unavailable\n"); }
static const struct zwp_input_method_v2_listener im_l = { .activate = im_activate, .deactivate = im_deactivate,
	.surrounding_text = im_surrounding, .text_change_cause = im_cause, .content_type = im_purpose,
	.done = im_done, .unavailable = im_unavailable };

static void pop_rect(void *d, struct zwp_input_popup_surface_v2 *p, int32_t x, int32_t y, int32_t w, int32_t h) {}
static const struct zwp_input_popup_surface_v2_listener pop_l = { .text_input_rectangle = pop_rect };

static struct wl_surface *ptr_surface;
static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	ptr_surface = s;
	printf("ptr-enter %s %d %d\n", name_of(s), wl_fixed_to_int(x), wl_fixed_to_int(y));
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	printf("ptr-leave %s\n", name_of(s));
	ptr_surface = NULL;
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	printf("ptr-motion %s %d %d\n", name_of(ptr_surface), wl_fixed_to_int(x), wl_fixed_to_int(y));
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st)
{
	printf("ptr-button %s %s\n", name_of(ptr_surface), st ? "down" : "up");
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {}
static void p_frame(void *d, struct wl_pointer *p) {}
static void p_src(void *d, struct wl_pointer *p, uint32_t s) {}
static void p_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void p_disc(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static void p_v120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static void p_reldir(void *d, struct wl_pointer *p, uint32_t a, uint32_t dir) {}
static const struct wl_pointer_listener p_l = { .enter = p_enter, .leave = p_leave, .motion = p_motion,
	.button = p_button, .axis = p_axis, .frame = p_frame, .axis_source = p_src, .axis_stop = p_stop,
	.axis_discrete = p_disc, .axis_value120 = p_v120, .axis_relative_direction = p_reldir };

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &p_l, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener seat_l = { .capabilities = seat_caps, .name = seat_name };

static void o_geom(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp,
	const char *mk, const char *md, int32_t tr) {}
static void o_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r)
{
	if (f & WL_OUTPUT_MODE_CURRENT) {
		out_w = w;
		out_h = h;
	}
}
static void o_done(void *d, struct wl_output *o) {}
static void o_scale(void *d, struct wl_output *o, int32_t s) { out_scale = s; }
static void o_name(void *d, struct wl_output *o, const char *n) {}
static void o_desc(void *d, struct wl_output *o, const char *n) {}
static const struct wl_output_listener o_l = { .geometry = o_geom, .mode = o_mode, .done = o_done,
	.scale = o_scale, .name = o_name, .description = o_desc };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
#define BIND(var, intf, v) if (!strcmp(iface, intf.name)) var = wl_registry_bind(r, name, &intf, v)
	BIND(comp, wl_compositor_interface, 4);
	BIND(shm, wl_shm_interface, 1);
	BIND(wm, xdg_wm_base_interface, 1);
	BIND(tim, zwp_text_input_manager_v3_interface, 1);
	BIND(imm, zwp_input_method_manager_v2_interface, 1);
	BIND(vpm, zwlr_virtual_pointer_manager_v1_interface, 1);
	BIND(lsh, zwlr_layer_shell_v1_interface, 1);
	BIND(inkm, glassine_ink_manager_v1_interface, 2);
	if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, 7);
		wl_seat_add_listener(seat, &seat_l, NULL);
	}
	if (!strcmp(iface, wl_output_interface.name) && !out) {
		out = wl_registry_bind(r, name, &wl_output_interface, 4);
		wl_output_add_listener(out, &o_l, NULL);
	}
#undef BIND
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_l = { .global = reg_global, .global_remove = reg_remove };

static void wait_for(int *flag)
{
	while (!*flag && wl_display_dispatch(dpy) >= 0) {
	}
}

static uint32_t now(void)
{
	static uint32_t t = 1000;
	return t += 10;
}

static void command(char *l)
{
	int a, b, c, n;
	if (sscanf(l, "win %d %d", &a, &b) == 2) {
		win_s = wl_compositor_create_surface(comp);
		win_xs = xdg_wm_base_get_xdg_surface(wm, win_s);
		xdg_surface_add_listener(win_xs, &xs_l, NULL);
		win_top = xdg_surface_get_toplevel(win_xs);
		xdg_toplevel_add_listener(win_top, &top_l, NULL);
		xdg_toplevel_set_app_id(win_top, "ink-test");
		win_w = a;
		win_h = b;
		wl_surface_commit(win_s);
		wait_for(&win_cfg);
		ink_area(win_s, 0, 0, win_w / 2, win_h);
		win_bw = win_w;
		win_bh = win_h;
		wl_surface_attach(win_s, buffer(win_w, win_h), 0, 0);
		wl_surface_commit(win_s);
		printf("win %dx%d\n", win_w, win_h);
	} else if (sscanf(l, "layer %d", &a) == 1) {
		layer_s = wl_compositor_create_surface(comp);
		layer_ls = zwlr_layer_shell_v1_get_layer_surface(lsh, layer_s, out, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "Ink-Test layer");
		zwlr_layer_surface_v1_add_listener(layer_ls, &ls_l, NULL);
		zwlr_layer_surface_v1_set_anchor(layer_ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_size(layer_ls, 0, a);
		zwlr_layer_surface_v1_set_keyboard_interactivity(layer_ls,
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
		wl_surface_commit(layer_s);
		wait_for(&layer_cfg);
		ink_area(layer_s, 0, 0, layer_w, layer_h);
		wl_surface_attach(layer_s, buffer(layer_w, layer_h), 0, 0);
		wl_surface_commit(layer_s);
		printf("layer %dx%d\n", layer_w, layer_h);
	} else if (sscanf(l, "ti %d %d", &a, &b) == 2) {
		zwp_text_input_v3_enable(ti);
		zwp_text_input_v3_set_content_type(ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
			ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
		zwp_text_input_v3_set_cursor_rectangle(ti, a, b, 2, 20);
		zwp_text_input_v3_commit(ti);
	} else if (sscanf(l, "cur %d %d", &a, &b) == 2) {
		zwp_text_input_v3_set_cursor_rectangle(ti, a, b, 2, 20);
		zwp_text_input_v3_commit(ti);
	} else if (!strcmp(l, "tioff")) {
		zwp_text_input_v3_disable(ti);
		zwp_text_input_v3_commit(ti);
	} else if (sscanf(l, "popup %d %d %d %d", &n, &a, &b, &c) == 4 && n >= 0 && n < 4) {
		pop_s[n] = wl_compositor_create_surface(comp);
		pop[n] = zwp_input_method_v2_get_input_popup_surface(im, pop_s[n]);
		zwp_input_popup_surface_v2_add_listener(pop[n], &pop_l, NULL);
		if (c) {
			ink_area(pop_s[n], 0, 0, a, b);
		} else {
			pop_ink[n] = glassine_ink_manager_v1_get_ink_surface(inkm, pop_s[n]);
		}
		pop_w[n] = a;
		pop_h[n] = b;
		wl_surface_attach(pop_s[n], buffer(a, b), 0, 0);
		wl_surface_commit(pop_s[n]);
	} else if (sscanf(l, "hide %d", &n) == 1 && n >= 0 && n < 4 && pop_s[n]) {
		wl_surface_attach(pop_s[n], NULL, 0, 0);
		wl_surface_commit(pop_s[n]);
	} else if (sscanf(l, "show %d", &n) == 1 && n >= 0 && n < 4 && pop_s[n]) {
		wl_surface_attach(pop_s[n], buffer(pop_w[n], pop_h[n]), 0, 0);
		wl_surface_commit(pop_s[n]);
	} else if (sscanf(l, "kill %d", &n) == 1 && n >= 0 && n < 4 && pop_s[n]) {
		zwp_input_popup_surface_v2_destroy(pop[n]);
		wl_surface_destroy(pop_s[n]);
		pop_s[n] = NULL;
	} else if (sscanf(l, "ptr %d %d", &a, &b) == 2) {
		int ew = out_w / out_scale, eh = out_h / out_scale;
		zwlr_virtual_pointer_v1_motion_absolute(vp, now(), a, b, ew, eh);
		zwlr_virtual_pointer_v1_frame(vp);
	} else if (!strcmp(l, "down") || !strcmp(l, "up")) {
		zwlr_virtual_pointer_v1_button(vp, now(), BTN_LEFT, l[0] == 'd' ? WL_POINTER_BUTTON_STATE_PRESSED :
			WL_POINTER_BUTTON_STATE_RELEASED);
		zwlr_virtual_pointer_v1_frame(vp);
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
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_l, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !seat || !wm || !tim || !imm || !vpm || !lsh || !inkm || !out) {
		fprintf(stderr, "missing a global\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm, &wm_l, NULL);
	ti = zwp_text_input_manager_v3_get_text_input(tim, seat);
	zwp_text_input_v3_add_listener(ti, &ti_l, NULL);
	im = zwp_input_method_manager_v2_get_input_method(imm, seat);
	zwp_input_method_v2_add_listener(im, &im_l, NULL);
	vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vpm, seat);
	wl_display_roundtrip(dpy);
	printf("ready %dx%d scale %d\n", out_w, out_h, out_scale);

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
