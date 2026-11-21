/* ui-scale-client: the scripted Wayland client of ui-scale-test.sh. It plays an
 * app that takes only whole buffer scales (wl_shm buffers at the wl_output's
 * scale, as GTK 3 does) or one that takes fractional scales
 * (wp_fractional_scale_v1 + wp_viewporter, buffers of round(size x scale)), a
 * pointer (wlr virtual pointer), a screenshot taker (wlr screencopy) and a sway
 * IPC client. Every buffer pixel has a value of its own (a one pixel
 * checkerboard in red, its column in green, its row in blue), so a screenshot
 * tells whether a surface is on the output pixel for pixel and where.
 * Commands on stdin, one per line:
 *   fixed W H          the next window asks for this size only (it floats)
 *   win whole|frac W H an xdg toplevel (app_id ui-scale-test), W H unless
 *                      configured; surface "win"
 *   sub X Y W H        a subsurface of it at X Y; surface "sub"
 *   popup X Y W H      an xdg popup whose top left corner is asked for at X Y
 *                      of the window; surface "popup"
 *   unsub / unpopup    destroy them
 *   dmg X Y W H        invert the checkerboard in this box of the window's
 *                      buffer and commit with only that box as damage
 *   check OUT S X Y [W H]  take a screenshot of output OUT and compare surface
 *                      S's buffer (its first W x H pixels) with the output's
 *                      pixels from X Y
 *   px OUT X Y         print the output's pixel
 *   turn DEGREES       the outputs' transform (90, 180, 270): check and px then
 *                      count in the turned output's pixels
 *   dump OUT FILE      write the screenshot (width, height, then XRGB rows)
 *   ptr X Y EW EH      pointer to X Y of an extent EW x EH (the layout's size,
 *                      or a multiple of it for positions between layout px)
 *   down / up          left button
 *   ipc COMMAND        a sway command
 *   ipcw COMMAND       a sway command on the window
 *   outputs            sway's get_outputs reply
 *   layout OUT         the output's box in the layout ("layout OUT X Y WxH")
 * It prints what it gets: "output NAME WxH scale N", "enter S NAME",
 * "preferred S N" (wl_surface.preferred_buffer_scale), "frac S N120",
 * "size S WxH buffer BWxBH scale N" for every buffer it attaches,
 * "popup-at X Y WxH", "ptr-enter/ptr-motion S X Y", "ptr-button S down|up", and
 * "ok COMMAND" after each command once the compositor has seen it. */
#define _GNU_SOURCE
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

enum { WIN, SUB, POPUP, NSURF };
static const char *names[NSURF] = { "win", "sub", "popup" };

struct output {
	struct wl_output *wl;
	char name[32];
	int w, h, scale;
};

struct surf {
	struct wl_surface *s;
	struct wp_viewport *vp;
	struct wp_fractional_scale_v1 *fs;
	int w, h;           /* the surface's size */
	int bw, bh;         /* its buffer's size */
	int frac;           /* the preferred fractional scale in 120ths, 0: none yet */
	int out;            /* the output it entered last, -1: none */
	int dx, dy, dw, dh; /* the box whose checkerboard is inverted */
	int mapped;
};

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct wl_subcompositor *subcomp;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm;
static struct wp_viewporter *viewporter;
static struct wp_fractional_scale_manager_v1 *fsm;
static struct zwlr_virtual_pointer_manager_v1 *vpm;
static struct zwlr_screencopy_manager_v1 *scm;
static struct zwlr_virtual_pointer_v1 *vp;
static struct wl_pointer *pointer;

static struct output outs[4];
static int nouts;
static struct surf surfs[NSURF];
static int fractional;             /* the window's kind */
static int fixed_w, fixed_h;
static struct xdg_surface *win_xs, *popup_xs;
static struct xdg_toplevel *win_top;
static struct xdg_popup *popup;
static struct wl_subsurface *sub;
static int cfg_w, cfg_h;
static struct wl_surface *ptr_surface;

static int surf_of(struct wl_surface *s)
{
	for (int i = 0; i < NSURF; i++) {
		if (s && surfs[i].s == s) return i;
	}
	return -1;
}

static uint32_t pattern(int id, const struct surf *f, int x, int y)
{
	int c = (x + y) & 1;
	if (x >= f->dx && x < f->dx + f->dw && y >= f->dy && y < f->dy + f->dh) c = !c;
	return 0xff000000u | (c ? 0xff0000u : 0) | (uint32_t)((x * 5 + id * 40) & 0xff) << 8 | (uint32_t)((y * 3) & 0xff);
}

/* the whole scale a surface draws at: its output's wl_output.scale */
static int whole_scale(const struct surf *f)
{
	int o = f->out >= 0 ? f->out : surfs[WIN].out >= 0 ? surfs[WIN].out : 0;
	return nouts > o && outs[o].scale > 0 ? outs[o].scale : 1;
}

static void draw(int id, int damage_only)
{
	struct surf *f = &surfs[id];
	if (!f->s || f->w <= 0 || f->h <= 0) return;
	int n = 1, bw, bh;
	if (fractional) {
		int s = f->frac ? f->frac : surfs[WIN].frac ? surfs[WIN].frac : 120;
		bw = (f->w * s + 60) / 120;
		bh = (f->h * s + 60) / 120;
	} else {
		n = whole_scale(f);
		bw = f->w * n;
		bh = f->h * n;
	}
	if (bw != f->bw || bh != f->bh) f->dw = f->dh = 0;
	f->bw = bw;
	f->bh = bh;
	int stride = bw * 4, size = stride * bh;
	int fd = memfd_create("ui-scale", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		perror("memfd");
		exit(1);
	}
	uint32_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int y = 0; y < bh; y++) {
		for (int x = 0; x < bw; x++) p[y * bw + x] = pattern(id, f, x, y);
	}
	munmap(p, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, bw, bh, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_attach(f->s, b, 0, 0);
	if (fractional) {
		wl_surface_set_buffer_scale(f->s, 1);
		wp_viewport_set_destination(f->vp, f->w, f->h);
	} else {
		wl_surface_set_buffer_scale(f->s, n);
	}
	if (damage_only) wl_surface_damage_buffer(f->s, f->dx, f->dy, f->dw, f->dh);
	else wl_surface_damage_buffer(f->s, 0, 0, bw, bh);
	wl_surface_commit(f->s);
	f->mapped = 1;
	printf("size %s %dx%d buffer %dx%d scale %d\n", names[id], f->w, f->h, bw, bh, fractional ? f->frac : n);
}

static void redraw_all(void)
{
	for (int i = NSURF - 1; i >= 0; i--) {
		if (surfs[i].mapped) draw(i, 0);
	}
}

/* ---- surfaces ---- */
static void s_enter(void *d, struct wl_surface *s, struct wl_output *o)
{
	int id = surf_of(s);
	for (int i = 0; i < nouts; i++) {
		if (outs[i].wl != o || id < 0) continue;
		printf("enter %s %s\n", names[id], outs[i].name);
		int was = whole_scale(&surfs[id]);
		surfs[id].out = i;
		if (!fractional && surfs[id].mapped && whole_scale(&surfs[id]) != was) redraw_all();
	}
}
static void s_leave(void *d, struct wl_surface *s, struct wl_output *o) {}
static void s_scale(void *d, struct wl_surface *s, int32_t n)
{
	int id = surf_of(s);
	if (id >= 0) printf("preferred %s %d\n", names[id], n);
}
static void s_transform(void *d, struct wl_surface *s, uint32_t t) {}
static const struct wl_surface_listener s_l = { s_enter, s_leave, s_scale, s_transform };

static void fs_scale(void *d, struct wp_fractional_scale_v1 *o, uint32_t scale)
{
	struct surf *f = d;
	int id = (int)(f - surfs);
	printf("frac %s %u\n", names[id], scale);
	if (f->frac == (int)scale) return;
	f->frac = scale;
	if (f->mapped) draw(id, 0);
}
static const struct wp_fractional_scale_v1_listener fs_l = { fs_scale };

static void surf_create(int id)
{
	struct surf *f = &surfs[id];
	memset(f, 0, sizeof *f);
	f->out = -1;
	f->s = wl_compositor_create_surface(comp);
	wl_surface_add_listener(f->s, &s_l, NULL);
	if (fractional) {
		f->vp = wp_viewporter_get_viewport(viewporter, f->s);
		f->fs = wp_fractional_scale_manager_v1_get_fractional_scale(fsm, f->s);
		wp_fractional_scale_v1_add_listener(f->fs, &fs_l, f);
	}
}

static void surf_destroy(int id)
{
	struct surf *f = &surfs[id];
	if (f->vp) wp_viewport_destroy(f->vp);
	if (f->fs) wp_fractional_scale_v1_destroy(f->fs);
	if (f->s) wl_surface_destroy(f->s);
	memset(f, 0, sizeof *f);
}

/* ---- xdg ---- */
static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t serial) { xdg_wm_base_pong(w, serial); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	struct surf *f = &surfs[WIN];
	xdg_surface_ack_configure(xs, serial);
	if (cfg_w > 0 && cfg_h > 0 && !fixed_w) {
		if (f->mapped && cfg_w == f->w && cfg_h == f->h) return;
		f->w = cfg_w;
		f->h = cfg_h;
	} else if (f->mapped) {
		return;
	}
	draw(WIN, 0);
}
static const struct xdg_surface_listener xs_l = { xs_configure };
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st)
{
	cfg_w = w;
	cfg_h = h;
	printf("configure %dx%d\n", w, h);
}
static void top_close(void *d, struct xdg_toplevel *t) {}
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) {}
static const struct xdg_toplevel_listener top_l = { top_configure, top_close, top_bounds, top_caps };

static void pxs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	if (!surfs[POPUP].mapped) draw(POPUP, 0);
}
static const struct xdg_surface_listener pxs_l = { pxs_configure };
static void pop_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
	printf("popup-at %d %d %dx%d\n", x, y, w, h);
}
static void pop_done(void *d, struct xdg_popup *p) {}
static void pop_repositioned(void *d, struct xdg_popup *p, uint32_t token) {}
static const struct xdg_popup_listener pop_l = { pop_configure, pop_done, pop_repositioned };

/* ---- outputs ---- */
static void o_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp,
		       const char *make, const char *model, int32_t tr) {}
static void o_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
	struct output *out = d;
	out->w = w;
	out->h = h;
}
static void o_done(void *d, struct wl_output *o)
{
	struct output *out = d;
	printf("output %s %dx%d scale %d\n", out->name, out->w, out->h, out->scale);
	if (!fractional) {
		for (int i = 0; i < NSURF; i++) {
			if (surfs[i].mapped && surfs[i].bw != surfs[i].w * whole_scale(&surfs[i])) draw(i, 0);
		}
	}
}
static void o_scale(void *d, struct wl_output *o, int32_t scale)
{
	struct output *out = d;
	out->scale = scale;
}
static void o_name(void *d, struct wl_output *o, const char *name)
{
	struct output *out = d;
	snprintf(out->name, sizeof out->name, "%s", name);
}
static void o_desc(void *d, struct wl_output *o, const char *desc) {}
static const struct wl_output_listener o_l = { o_geometry, o_mode, o_done, o_scale, o_name, o_desc };

/* ---- pointer ---- */
static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	int id = surf_of(s);
	ptr_surface = s;
	wl_pointer_set_cursor(p, serial, NULL, 0, 0);   /* no cursor in the screenshots */
	printf("ptr-enter %s %.2f %.2f\n", id >= 0 ? names[id] : "other", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	ptr_surface = NULL;
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	int id = surf_of(ptr_surface);
	printf("ptr-motion %s %.2f %.2f\n", id >= 0 ? names[id] : "other", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t state)
{
	int id = surf_of(ptr_surface);
	printf("ptr-button %s %s\n", id >= 0 ? names[id] : "other", state ? "down" : "up");
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {}
static void p_frame(void *d, struct wl_pointer *p) {}
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static const struct wl_pointer_listener p_l = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame,
	p_axis_source, p_axis_stop, p_axis_discrete };

/* ---- registry ---- */
static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
#define BIND(var, i, v) if (!strcmp(iface, i.name)) var = wl_registry_bind(r, name, &i, ver < v ? ver : v)
	BIND(comp, wl_compositor_interface, 6);
	BIND(subcomp, wl_subcompositor_interface, 1);
	BIND(shm, wl_shm_interface, 1);
	BIND(seat, wl_seat_interface, 5);
	BIND(wm, xdg_wm_base_interface, 3);
	BIND(viewporter, wp_viewporter_interface, 1);
	BIND(fsm, wp_fractional_scale_manager_v1_interface, 1);
	BIND(vpm, zwlr_virtual_pointer_manager_v1_interface, 1);
	BIND(scm, zwlr_screencopy_manager_v1_interface, 1);
	if (!strcmp(iface, wl_output_interface.name) && nouts < 4) {
		struct output *o = &outs[nouts++];
		o->wl = wl_registry_bind(r, name, &wl_output_interface, 4);
		o->scale = 1;
		wl_output_add_listener(o->wl, &o_l, o);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

/* ---- screenshots ---- */
static struct {
	uint32_t format, w, h, stride;
	uint32_t *data;
	int done, flags;
} shot;

static void sc_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format, uint32_t w, uint32_t h,
		      uint32_t stride)
{
	shot.format = format;
	shot.w = w;
	shot.h = h;
	shot.stride = stride;
}
static void sc_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) { shot.flags = flags; }
static void sc_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns) { shot.done = 1; }
static void sc_failed(void *d, struct zwlr_screencopy_frame_v1 *f) { shot.done = -1; }
static const struct zwlr_screencopy_frame_v1_listener sc_l = { sc_buffer, sc_flags, sc_ready, sc_failed };

static int take_shot(const char *name)
{
	struct output *o = NULL;
	for (int i = 0; i < nouts; i++) {
		if (!strcmp(outs[i].name, name)) o = &outs[i];
	}
	if (!o) return 0;
	if (shot.data) munmap(shot.data, shot.stride * shot.h);
	memset(&shot, 0, sizeof shot);
	struct zwlr_screencopy_frame_v1 *f = zwlr_screencopy_manager_v1_capture_output(scm, 0, o->wl);
	zwlr_screencopy_frame_v1_add_listener(f, &sc_l, NULL);
	wl_display_roundtrip(dpy);
	if (!shot.w || (shot.format != WL_SHM_FORMAT_XRGB8888 && shot.format != WL_SHM_FORMAT_ARGB8888)) {
		printf("shot: format %u\n", shot.format);
		return 0;
	}
	int size = shot.stride * shot.h;
	int fd = memfd_create("ui-scale-shot", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) return 0;
	shot.data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, shot.w, shot.h, shot.stride, shot.format);
	wl_shm_pool_destroy(pool);
	close(fd);
	zwlr_screencopy_frame_v1_copy(f, b);
	while (!shot.done && wl_display_dispatch(dpy) >= 0) {}
	wl_buffer_destroy(b);
	zwlr_screencopy_frame_v1_destroy(f);
	return shot.done == 1;
}

/* the output's transform (turn): checks count in the turned output's pixels */
static int turn;
static int shot_w(void) { return turn % 180 ? (int)shot.h : (int)shot.w; }
static int shot_h(void) { return turn % 180 ? (int)shot.w : (int)shot.h; }

static uint32_t raw_px(int x, int y)
{
	if (shot.flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) y = shot.h - 1 - y;
	return shot.data[y * (shot.stride / 4) + x] | 0xff000000u;
}

static uint32_t shot_px(int x, int y)
{
	switch (turn) {
	case 270: return raw_px(y, shot_w() - 1 - x);
	case 180: return raw_px(shot_w() - 1 - x, shot_h() - 1 - y);
	case 90: return raw_px(shot_h() - 1 - y, x);
	}
	return raw_px(x, y);
}

/* ---- sway IPC ---- */
static char *ipc(uint32_t type, const char *payload)
{
	const char *path = getenv("SWAYSOCK");
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (!path || fd < 0) return NULL;
	snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
	if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
		close(fd);
		return NULL;
	}
	uint32_t len = strlen(payload), head[2];
	char h[14];
	memcpy(h, "i3-ipc", 6);
	memcpy(h + 6, &len, 4);
	memcpy(h + 10, &type, 4);
	char *reply = NULL;
	if (write(fd, h, 14) == 14 && write(fd, payload, len) == (ssize_t)len && read(fd, h, 14) == 14) {
		memcpy(head, h + 6, 8);
		reply = calloc(1, head[0] + 1);
		uint32_t got = 0;
		while (got < head[0]) {
			ssize_t n = read(fd, reply + got, head[0] - got);
			if (n <= 0) break;
			got += n;
		}
	}
	close(fd);
	return reply;
}

static uint32_t now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void command(char *l)
{
	int a, b, c, e, n;
	char o[32], s[32], file[256];
	if (sscanf(l, "fixed %d %d", &a, &b) == 2) {
		fixed_w = a;
		fixed_h = b;
	} else if (sscanf(l, "win %31s %d %d", s, &a, &b) == 3) {
		fractional = !strcmp(s, "frac");
		surf_create(WIN);
		surfs[WIN].w = fixed_w ? fixed_w : a;
		surfs[WIN].h = fixed_w ? fixed_h : b;
		win_xs = xdg_wm_base_get_xdg_surface(wm, surfs[WIN].s);
		xdg_surface_add_listener(win_xs, &xs_l, NULL);
		win_top = xdg_surface_get_toplevel(win_xs);
		xdg_toplevel_add_listener(win_top, &top_l, NULL);
		xdg_toplevel_set_app_id(win_top, "ui-scale-test");
		xdg_toplevel_set_title(win_top, s);
		if (fixed_w) {
			xdg_toplevel_set_min_size(win_top, fixed_w, fixed_h);
			xdg_toplevel_set_max_size(win_top, fixed_w, fixed_h);
		}
		wl_surface_commit(surfs[WIN].s);
	} else if (sscanf(l, "sub %d %d %d %d", &a, &b, &c, &e) == 4) {
		surf_create(SUB);
		surfs[SUB].w = c;
		surfs[SUB].h = e;
		surfs[SUB].out = surfs[WIN].out;
		sub = wl_subcompositor_get_subsurface(subcomp, surfs[SUB].s, surfs[WIN].s);
		wl_subsurface_set_position(sub, a, b);
		wl_subsurface_set_desync(sub);
		draw(SUB, 0);
		wl_surface_commit(surfs[WIN].s);
	} else if (sscanf(l, "turn %d", &a) == 1) {
		turn = a;
	} else if (!strcmp(l, "unsub")) {
		wl_subsurface_destroy(sub);
		surf_destroy(SUB);
		wl_surface_commit(surfs[WIN].s);
	} else if (sscanf(l, "popup %d %d %d %d", &a, &b, &c, &e) == 4) {
		surf_create(POPUP);
		surfs[POPUP].w = c;
		surfs[POPUP].h = e;
		surfs[POPUP].out = surfs[WIN].out;
		struct xdg_positioner *pos = xdg_wm_base_create_positioner(wm);
		xdg_positioner_set_size(pos, c, e);
		xdg_positioner_set_anchor_rect(pos, a, b, 1, 1);
		xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
		xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
		xdg_positioner_set_constraint_adjustment(pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
			XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
		popup_xs = xdg_wm_base_get_xdg_surface(wm, surfs[POPUP].s);
		xdg_surface_add_listener(popup_xs, &pxs_l, NULL);
		popup = xdg_surface_get_popup(popup_xs, win_xs, pos);
		xdg_popup_add_listener(popup, &pop_l, NULL);
		xdg_positioner_destroy(pos);
		wl_surface_commit(surfs[POPUP].s);
	} else if (!strcmp(l, "unpopup")) {
		xdg_popup_destroy(popup);
		xdg_surface_destroy(popup_xs);
		surf_destroy(POPUP);
	} else if (sscanf(l, "dmg %d %d %d %d", &a, &b, &c, &e) == 4) {
		surfs[WIN].dx = a;
		surfs[WIN].dy = b;
		surfs[WIN].dw = c;
		surfs[WIN].dh = e;
		draw(WIN, 1);
	} else if ((n = sscanf(l, "check %31s %31s %d %d %d %d", o, s, &a, &b, &c, &e)) >= 4) {
		int id = !strcmp(s, "sub") ? SUB : !strcmp(s, "popup") ? POPUP : WIN;
		struct surf *f = &surfs[id];
		int w = n == 6 ? c : f->bw, h = n == 6 ? e : f->bh, bad = 0, fx = 0, fy = 0, lx = 0, ly = 0;
		if (!f->mapped || !take_shot(o)) {
			printf("check %s %s bad: no %s\n", o, s, f->mapped ? "screenshot" : "surface");
		} else {
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					int X = a + x, Y = b + y;
					if (X >= 0 && Y >= 0 && X < shot_w() && Y < shot_h() &&
					    shot_px(X, Y) == pattern(id, f, x, y)) continue;
					if (!bad++) {
						fx = x;
						fy = y;
					}
					lx = x;
					ly = y;
				}
			}
			if (!bad) {
				printf("check %s %s ok %d %d %dx%d\n", o, s, a, b, w, h);
			} else {
				int X = a + fx, Y = b + fy, in = X >= 0 && Y >= 0 && X < shot_w() && Y < shot_h();
				printf("check %s %s bad %d %d %dx%d: %d pixels, first at %d %d: %06x, expected %06x, last at %d %d\n",
				       o, s, a, b, w, h, bad, fx, fy, in ? shot_px(X, Y) & 0xffffff : 0,
				       pattern(id, f, fx, fy) & 0xffffff, lx, ly);
			}
		}
	} else if (sscanf(l, "px %31s %d %d", o, &a, &b) == 3) {
		if (take_shot(o) && a >= 0 && b >= 0 && a < shot_w() && b < shot_h()) {
			printf("px %s %d %d %06x\n", o, a, b, shot_px(a, b) & 0xffffff);
		} else {
			printf("px %s %d %d none\n", o, a, b);
		}
	} else if (sscanf(l, "dump %31s %255s", o, file) == 2) {
		FILE *f = take_shot(o) ? fopen(file, "wb") : NULL;
		if (f) {
			fwrite(&shot.w, 4, 1, f);
			fwrite(&shot.h, 4, 1, f);
			for (uint32_t y = 0; y < shot.h; y++) {
				for (uint32_t x = 0; x < shot.w; x++) {
					uint32_t v = raw_px(x, y);
					fwrite(&v, 4, 1, f);
				}
			}
			fclose(f);
			printf("dump %s %ux%u\n", o, shot.w, shot.h);
		} else {
			printf("dump %s failed\n", o);
		}
	} else if (sscanf(l, "ptr %d %d %d %d", &a, &b, &c, &e) == 4) {
		zwlr_virtual_pointer_v1_motion_absolute(vp, now(), a, b, c, e);
		zwlr_virtual_pointer_v1_frame(vp);
	} else if (!strcmp(l, "down") || !strcmp(l, "up")) {
		zwlr_virtual_pointer_v1_button(vp, now(), BTN_LEFT, l[0] == 'd' ? WL_POINTER_BUTTON_STATE_PRESSED :
			WL_POINTER_BUTTON_STATE_RELEASED);
		zwlr_virtual_pointer_v1_frame(vp);
	} else if (!strncmp(l, "ipc ", 4) || !strncmp(l, "ipcw ", 5)) {
		char cmd[600];
		if (l[3] == 'w') snprintf(cmd, sizeof cmd, "[app_id=\"ui-scale-test\"] %s", l + 5);
		else snprintf(cmd, sizeof cmd, "%s", l + 4);
		char *r = ipc(0, cmd);
		printf("ipc %s\n", r && strstr(r, "\"success\": true") && !strstr(r, "\"success\": false") ? "ok" : r ? r : "none");
		free(r);
	} else if (sscanf(l, "layout %31s", o) == 1) {
		/* the output's box in the layout: the "rect" before its name */
		char *r = ipc(3, ""), key[64], *at, *rect = NULL;
		snprintf(key, sizeof key, "\"name\": \"%s\"", o);
		at = r ? strstr(r, key) : NULL;
		for (char *p = r; at && (p = strstr(p, "\"rect\": {")) && p < at; p++) rect = p;
		if (rect && sscanf(rect, "\"rect\": { \"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d", &a, &b, &c, &e) == 4) {
			printf("layout %s %d %d %dx%d\n", o, a, b, c, e);
		} else {
			printf("layout %s none\n", o);
		}
		free(r);
	} else if (!strcmp(l, "outputs")) {
		char *r = ipc(3, "");
		for (char *p = r; p && *p; p++) {
			if (*p == '\n') *p = ' ';
		}
		printf("outputs %s\n", r ? r : "none");
		free(r);
	} else {
		printf("unknown %s\n", l);
		return;
	}
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	printf("ok %s\n", l);
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "no display\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_l, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	if (!comp || !subcomp || !shm || !seat || !wm || !viewporter || !fsm || !vpm || !scm || !nouts) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm, &wm_l, NULL);
	vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vpm, seat);
	wl_display_roundtrip(dpy);
	pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(pointer, &p_l, NULL);
	wl_display_roundtrip(dpy);
	printf("ready\n");

	char line[512];
	size_t have = 0;
	struct pollfd fds[2] = { { .fd = wl_display_get_fd(dpy), .events = POLLIN }, { .fd = 0, .events = POLLIN } };
	for (;;) {
		wl_display_flush(dpy);
		if (poll(fds, 2, -1) < 0) break;
		if (fds[0].revents & POLLIN) {
			if (wl_display_dispatch(dpy) < 0) break;
		} else if (fds[0].revents) {
			break;
		}
		if (fds[1].revents & (POLLIN | POLLHUP)) {
			ssize_t n = read(0, line + have, sizeof line - 1 - have);
			if (n <= 0) break;
			have += n;
			line[have] = 0;
			char *nl;
			while ((nl = strchr(line, '\n'))) {
				*nl = 0;
				command(line);
				have -= nl + 1 - line;
				memmove(line, nl + 1, have + 1);
			}
		}
	}
	return 0;
}
