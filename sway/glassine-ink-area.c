/* glassine-ink-area.c — the pen's ink areas for glassine-epdd-desktop.
 * Built into the patched sway (0007). After each output commit sway walks the
 * scene top to bottom: every window's visible region is its buffers minus every
 * buffer above them (menus, the shell's layer surfaces, the keyboard, other
 * windows). glassine-epdd-desktop inks the pen only inside the regions of apps with ink=on.
 * Sent to glassine-epdd-desktop's FIFO when it changes (and again every 2 s on a commit, so a
 * restarted epdd gets them), in portrait panel px (layout px x the output scale,
 * turned back by glassine-rotd's /run/glassine-rotation, as the popup hints):
 * ink begin / ink brush K ... / ink r K S X Y W H APP (ink S X Y W H APP
 * without a brush) / ink end
 * (S 1 = the app registered areas; K: the window's brush, 0 = the per-app
 * one; brush lines: K TKIND TMIN TMAX TEXP RKIND RMIN RMAX REXP, tip and
 * eraser end, widths in panel px, see glassine-epdd-desktop's ink_line_cmd)
 * APP as glassine-swayd names it: the app_id, else "class instance", lower case.
 * An app narrows its window to its canvas with the glassine_ink_v1 protocol
 * (glassine-ink-v1.xml): areas in surface coordinates, intersected with what is
 * visible; such a window is sent with S 1 (it inks without a per-app ink=on).
 * For apps without it, a helper (glassine-ink-discover: the accessibility tree)
 * sets a window's areas with the sway command `[con_id=N] glassine_ink X Y W H ...`
 * (window coordinates) or `glassine_ink none`; an app's own areas win.
 * v3 brush objects (create_brush + set_brush_object): the brush line gets
 * tip=<id> / rub=<id> after its 9 numbers, the brush written once as
 * /run/glassine-ink/<id>.brush (epd-brush.h; GLASSINE_INK_DIR overrides the dir, for
 * the host test). KIND is then 2/3/1 by the brush's ink kind, the exponent 1.
 * Nothing happens when glassine-epdd-desktop isn't there (O_NONBLOCK open fails). */
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wlr/util/transform.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include "log.h"
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/output.h"
#include "sway/tree/container.h"
#include "sway/scene_descriptor.h"
#include "sway/server.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "glassine-ink-area.h"
#include "glassine-ink-v1-protocol.h"
#include "na1-ink-v1-protocol.h"
#include "epd-brush.h"
#ifndef M_PI                    /* -std=c11 */
#define M_PI 3.14159265358979323846
#endif

#define EPDD_CTL "/run/glassine-epdd-desktop.ctl"
#define MAXB 256                /* scene buffers looked at */
#define MAXV 32                 /* windows */
#define MSG 3800                /* < PIPE_BUF: one atomic write */

/* b: the buffer as shown (layout px); ox/oy: its surface's origin -- sway clips
 * a CSD window to its geometry and wlroots then puts the buffer at the clip's
 * origin, so the surface origin is b minus the clip (the app's areas are in
 * surface coordinates: GTK's shadow would offset them otherwise) */
struct item { struct wlr_box b; int ox, oy; struct sway_view *v; struct wlr_surface *s; };

/* ---- glassine_ink_brush_v1 (v3): a declarative brush ----
 * Refcounted: its resource holds one ref, every surface tool selecting it
 * one more; a destroyed brush that is still selected lives on unchanged
 * (the snapshot the protocol asks for). */
struct ink_brush {
	struct wl_list link;            /* all_brushes */
	struct wl_resource *res;        /* NULL once destroyed */
	struct wl_client *client;       /* compared only (the per-client limit) */
	int refs, uses;                 /* uses: selected on surface tools */
	epdb_desc d;                    /* d.tip = tip */
	uint8_t *tip;
	bool dirty;                     /* d changed since id was computed */
	int enc_rot;                    /* the rotation id was encoded for (fixed angles turn with it) */
	bool ok;                        /* id names a written file */
	char id[17];
};
static struct wl_list all_brushes;
#define MAXBRUSH 8                      /* in use per client */

/* ---- glassine_ink_v1: the areas apps register ---- */
struct ink_surf {
	struct wl_list link;
	struct wl_resource *res;
	struct wlr_surface *surface;
	pixman_region32_t areas;
	bool set;                       /* areas given (else the window's default) */
	struct { uint32_t kind; float wmin, wmax, exp; struct ink_brush *obj; } br[2];     /* set_brush: tip, eraser (kind 0: unset); obj: set_brush_object */
	/* res NULL: set by the glassine_ink command (discovered), else by the app */
	struct wl_listener surface_destroy;
};
static struct wl_list ink_surfs;        /* ink_surf.link */
static struct wl_event_source *ink_idle;

static void update_soon_cb(void *data)
{
	ink_idle = NULL;
	if (root->outputs->length > 0) {
		glassine_ink_update(root->outputs->items[0]);
	}
}

static void update_soon(void)
{
	if (!ink_idle) {
		ink_idle = wl_event_loop_add_idle(server.wl_event_loop, update_soon_cb, NULL);
	}
}

static struct ink_surf *ink_surf_for(struct wlr_surface *s)
{
	struct ink_surf *i;
	wl_list_for_each(i, &ink_surfs, link) {
		if (i->surface == s) {
			return i;
		}
	}
	return NULL;
}

static void brush_unuse(struct ink_brush *b);

static void ink_surf_free(struct ink_surf *i)
{
	brush_unuse(i->br[0].obj);
	brush_unuse(i->br[1].obj);
	wl_list_remove(&i->link);
	wl_list_remove(&i->surface_destroy.link);
	pixman_region32_fini(&i->areas);
	if (i->res) {
		wl_resource_set_user_data(i->res, NULL);
	}
	free(i);
	update_soon();
}

static void handle_surface_destroy(struct wl_listener *l, void *data)
{
	struct ink_surf *i = wl_container_of(l, i, surface_destroy);
	ink_surf_free(i);
}

static void surf_set_areas(struct wl_client *c, struct wl_resource *res, struct wl_resource *region)
{
	struct ink_surf *i = wl_resource_get_user_data(res);
	if (!i) {
		return;
	}
	i->set = region != NULL;
	if (region) {
		pixman_region32_copy(&i->areas, wlr_region_from_resource(region));
	} else {
		pixman_region32_clear(&i->areas);
	}
	update_soon();
}

static void surf_set_brush(struct wl_client *c, struct wl_resource *res, uint32_t tool, uint32_t kind,
		wl_fixed_t wmin, wl_fixed_t wmax, wl_fixed_t exp)
{
	struct ink_surf *i = wl_resource_get_user_data(res);
	if (!i || tool > 1 || kind > 3) {
		return;
	}
	float lo = wl_fixed_to_double(wmin), hi = wl_fixed_to_double(wmax), e = wl_fixed_to_double(exp);
	if (hi < lo) {
		hi = lo;
	}
	if (!(e > 0.05f && e < 20)) {
		e = 1;
	}
	brush_unuse(i->br[tool].obj);
	i->br[tool].obj = NULL;
	i->br[tool].kind = kind;
	i->br[tool].wmin = lo;
	i->br[tool].wmax = hi;
	i->br[tool].exp = e;
	update_soon();
}

/* ---- brush objects ---- */
static void brush_unref(struct ink_brush *b)
{
	if (b && --b->refs == 0) {
		wl_list_remove(&b->link);
		free(b->tip);
		free(b);
	}
}

static void brush_unuse(struct ink_brush *b)
{
	if (b) {
		b->uses--;
		brush_unref(b);
	}
}

static struct ink_brush *brush_from(struct wl_resource *res)
{
	return wl_resource_get_user_data(res);
}

static void brush_changed(struct ink_brush *b)
{
	b->dirty = true;
	if (b->uses > 0) {
		update_soon();
	}
}

static float clampf(float v, float lo, float hi)
{
	return !(v >= lo) ? lo : v > hi ? hi : v;     /* NaN -> lo */
}

static void brush_set_shape(struct wl_client *c, struct wl_resource *res, wl_fixed_t hardness,
		wl_fixed_t spacing, wl_fixed_t elongation, wl_fixed_t angle, uint32_t mode)
{
	struct ink_brush *b = brush_from(res);
	b->d.hardness = clampf(wl_fixed_to_double(hardness), 0, 1);
	b->d.spacing = clampf(wl_fixed_to_double(spacing), 0.02f, 4);
	b->d.elongation = clampf(wl_fixed_to_double(elongation), 0.05f, 1);
	float a = fmodf(wl_fixed_to_double(angle), 360);
	b->d.angle_deg = a > 180 ? a - 360 : a < -180 ? a + 360 : a;     /* int16 x 1/64 in the file */
	b->d.angle_mode = mode <= NB_ANG_TILT ? mode : NB_ANG_FIXED;
	brush_changed(b);
}

static void brush_set_tip(struct wl_client *c, struct wl_resource *res, int32_t fd, uint32_t size, uint32_t variants)
{
	struct ink_brush *b = brush_from(res);
	if (size == 0) {
		close(fd);
		free(b->tip);
		b->tip = NULL;
		b->d.tip = NULL;
		b->d.tip_size = 0;
		b->d.tip_variants = 1;
		brush_changed(b);
		return;
	}
	size_t need = (size_t)size * size * variants;
	struct stat st;
	if (size > NB_MAXTIP || variants < 1 || variants > NB_MAXVAR || fstat(fd, &st) < 0 ||
			st.st_size != (off_t)need) {
		close(fd);
		wl_resource_post_error(res, GLASSINE_INK_BRUSH_V1_ERROR_BAD_TIP,
			"tip %ux%u x %u variants: size 1..%d, variants 1..%d, fd exactly size*size*variants bytes",
			size, size, variants, NB_MAXTIP, NB_MAXVAR);
		return;
	}
	void *p = mmap(NULL, need, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	uint8_t *t = p != MAP_FAILED ? malloc(need) : NULL;
	if (!t) {
		if (p != MAP_FAILED) {
			munmap(p, need);
		}
		wl_resource_post_error(res, GLASSINE_INK_BRUSH_V1_ERROR_BAD_TIP, "tip: can't map the fd");
		return;
	}
	memcpy(t, p, need);
	munmap(p, need);
	free(b->tip);
	b->tip = t;
	b->d.tip = t;
	b->d.tip_size = size;
	b->d.tip_variants = variants;
	brush_changed(b);
}

static void brush_set_ink(struct wl_client *c, struct wl_resource *res, uint32_t kind, uint32_t grey,
		uint32_t opacity, uint32_t blend)
{
	struct ink_brush *b = brush_from(res);
	b->d.kind = kind <= NB_NONE ? kind : NB_INK;
	b->d.grey = grey > 255 ? 255 : grey;
	b->d.opacity = opacity > 255 ? 255 : opacity;
	b->d.blend = blend <= NB_REPLACE ? blend : NB_DARKEN;
	brush_changed(b);
}

static void brush_set_curve(struct wl_client *c, struct wl_resource *res, uint32_t curve, struct wl_array *points)
{
	struct ink_brush *b = brush_from(res);
	if (curve >= NB_NCURVE || (points->size != 0 && points->size != 16)) {
		wl_resource_post_error(res, GLASSINE_INK_BRUSH_V1_ERROR_BAD_CURVE,
			"curve %u: %zu bytes (16, or 0 to remove)", curve, points->size);
		return;
	}
	if (points->size) {
		memcpy(b->d.curve[curve], points->data, 16);
		b->d.curves_present |= 1u << curve;
	} else {
		memset(b->d.curve[curve], 0, 16);
		b->d.curves_present &= ~(1u << curve);
	}
	brush_changed(b);
}

static void brush_set_scatter(struct wl_client *c, struct wl_resource *res, uint32_t count,
		wl_fixed_t pos, wl_fixed_t size, wl_fixed_t op)
{
	struct ink_brush *b = brush_from(res);
	b->d.scatter = count > 8 ? 8 : count;
	b->d.pos_jitter = clampf(wl_fixed_to_double(pos), 0, 2);
	b->d.size_jitter = clampf(wl_fixed_to_double(size), 0, 1);
	b->d.op_jitter = clampf(wl_fixed_to_double(op), 0, 1);
	brush_changed(b);
}

static void brush_destroy(struct wl_client *c, struct wl_resource *res)
{
	wl_resource_destroy(res);
}

static const struct glassine_ink_brush_v1_interface brush_impl = {
	.destroy = brush_destroy,
	.set_shape = brush_set_shape,
	.set_tip = brush_set_tip,
	.set_ink = brush_set_ink,
	.set_curve = brush_set_curve,
	.set_scatter = brush_set_scatter,
};

static void brush_resource_destroy(struct wl_resource *res)
{
	struct ink_brush *b = brush_from(res);
	b->res = NULL;
	brush_unref(b);
}

/*
 * Both protocol names share this code: their requests and events are the same, and
 * only the interface tables differ. Children take the name their manager was bound
 * under, as libwayland compares object arguments by interface name.
 */
static const struct wl_interface *child_iface(struct wl_resource *mgr,
		const struct wl_interface *current, const struct wl_interface *old)
{
	return strcmp(wl_resource_get_class(mgr), na1_ink_manager_v1_interface.name) == 0 ? old : current;
}

static void mgr_create_brush(struct wl_client *c, struct wl_resource *res, uint32_t id)
{
	struct ink_brush *b = calloc(1, sizeof *b);
	if (!b) {
		wl_client_post_no_memory(c);
		return;
	}
	b->res = wl_resource_create(c, child_iface(res, &glassine_ink_brush_v1_interface, &na1_ink_brush_v1_interface),
		wl_resource_get_version(res), id);
	if (!b->res) {
		free(b);
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(b->res, &brush_impl, b, brush_resource_destroy);
	b->client = c;
	b->refs = 1;
	epdb_desc_default(&b->d);
	b->dirty = true;
	wl_list_insert(&all_brushes, &b->link);
}

static void surf_set_brush_object(struct wl_client *c, struct wl_resource *res, uint32_t tool,
		struct wl_resource *brush_res, wl_fixed_t wmin, wl_fixed_t wmax)
{
	struct ink_surf *i = wl_resource_get_user_data(res);
	if (!i || tool > 1) {
		return;
	}
	struct ink_brush *b = brush_res ? brush_from(brush_res) : NULL;
	if (b && b->uses == 0 && i->br[tool].obj != b) {
		int n = 0;
		struct ink_brush *o;
		wl_list_for_each(o, &all_brushes, link) {
			n += o->client == c && o->uses > 0 && o != i->br[tool].obj;
		}
		if (n >= MAXBRUSH) {
			wl_resource_post_error(brush_res, GLASSINE_INK_BRUSH_V1_ERROR_TOO_MANY,
				"more than %d brushes in use", MAXBRUSH);
			return;
		}
	}
	if (b) {
		b->refs++;
		b->uses++;
		b->dirty = true;        /* snapshot again at the next frame */
	}
	brush_unuse(i->br[tool].obj);
	i->br[tool].obj = b;
	if (!b) {
		i->br[tool].kind = 0;   /* null: set_brush(tool, default) */
		update_soon();
		return;
	}
	float lo = wl_fixed_to_double(wmin), hi = wl_fixed_to_double(wmax);
	i->br[tool].wmin = lo;
	i->br[tool].wmax = hi < lo ? lo : hi;
	i->br[tool].exp = 1;
	i->br[tool].kind = 2;           /* non-zero: the window has a brush; the line's KIND comes from b */
	update_soon();
}

/* the brush files: written once per id; a small table of what we wrote */
#define MAXFILES 32
static struct { char id[17]; long long used; } files[MAXFILES];

static const char *ink_dir(void)
{
	static const char *dir;
	if (!dir) {
		dir = getenv("GLASSINE_INK_DIR");
		if (!dir || !*dir) {
			dir = NB_DIR;
		}
	}
	return dir;
}

static long long now_ms(void);

static bool brush_write(const uint8_t *buf, size_t len, const char *id)
{
	const char *dir = ink_dir();
	char path[256], tmp[256];
	snprintf(path, sizeof path, "%s/%s.brush", dir, id);
	snprintf(tmp, sizeof tmp, "%s/.%s.%d.tmp", dir, id, (int)getpid());
	if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
		sway_log(SWAY_ERROR, "glassine-ink: %s: %s", dir, strerror(errno));
		return false;
	}
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		sway_log(SWAY_ERROR, "glassine-ink: %s: %s", tmp, strerror(errno));
		return false;
	}
	bool ok = write(fd, buf, len) == (ssize_t)len && fchmod(fd, 0644) == 0;
	ok = close(fd) == 0 && ok;
	if (!ok || rename(tmp, path) < 0) {
		sway_log(SWAY_ERROR, "glassine-ink: %s: %s", path, strerror(errno));
		unlink(tmp);
		return false;
	}
	return true;
}

static bool id_live(const char *id)
{
	struct ink_brush *b;
	wl_list_for_each(b, &all_brushes, link) {
		if (b->ok && !strcmp(b->id, id)) {
			return true;
		}
	}
	return false;
}

/* the brush's file id for the brush line (encodes + writes when it changed);
 * NULL: no file (the line goes out without tip=, a plain disc) */
/* a fixed angle (surface orientation = layout px, y down) in panel-buffer px,
 * what epd-brush draws in: the unit vector through the rects' turn back to
 * portrait px (rotation, no flips) and then portrait -> panel buffer
 * (bx = PW-1-py, by = px) */
static float panel_angle(float deg, int rot)
{
	double a = deg * M_PI / 180, x = cos(a), y = sin(a), px, py;
	switch (rot) {
	case 270: px = y; py = -x; break;
	case 90: px = -y; py = x; break;
	case 180: px = -x; py = -y; break;
	default: px = x; py = y;
	}
	return (float)(atan2(px, -py) * 180 / M_PI);   /* (bx, by) = (-py, px) */
}

/* the angle matters (an elongated dab or a mask tip at a fixed angle): it
 * turns with the panel; a round disc keeps its bytes (and its file) */
static bool angle_turns(const epdb_desc *d)
{
	return d->angle_mode == NB_ANG_FIXED && (d->elongation < 1 || d->tip);
}

static const char *brush_prepare(struct ink_brush *b, int rot)
{
	static uint8_t buf[NB_HDR + NB_MAXTIP * NB_MAXTIP * NB_MAXVAR];
	static long long failed = -1000000;     /* the last write failure (ms) */
	long long t = now_ms();
	if (b->enc_rot != rot && angle_turns(&b->d)) {
		b->dirty = true;                /* turned: a new file (the angle is in the bytes) */
	}
	if (b->dirty && t - failed < 2000) {
		return NULL;                    /* the dir isn't writable: retry in 2 s, not every frame */
	}
	if (b->dirty) {
		epdb_desc e = b->d;
		if (angle_turns(&e)) {
			e.angle_deg = panel_angle(e.angle_deg, rot);
		}           /* direction/tilt: an offset, rotations keep it */
		b->enc_rot = rot;
		size_t len = epdb_encode(&e, buf);
		b->ok = false;
		b->dirty = false;
		if (!len) {
			return NULL;
		}
		epdb_id(buf, len, b->id);
		int k, old = 0;
		for (k = 0; k < MAXFILES && strcmp(files[k].id, b->id); k++) {
			if (files[k].used < files[old].used) {
				old = k;
			}
		}
		if (k == MAXFILES) {                    /* not written yet */
			if (!brush_write(buf, len, b->id)) {
				failed = t;
				b->dirty = true;
				return NULL;
			}
			if (files[old].id[0] && !id_live(files[old].id)) {     /* the oldest goes */
				char path[256];
				snprintf(path, sizeof path, "%s/%s.brush", ink_dir(), files[old].id);
				unlink(path);
			}
			memcpy(files[old].id, b->id, 17);
			k = old;
		}
		files[k].used = t;
		b->ok = true;
	}
	return b->ok ? b->id : NULL;
}

static void surf_destroy(struct wl_client *c, struct wl_resource *res)
{
	wl_resource_destroy(res);
}

static const struct glassine_ink_surface_v1_interface surf_impl = {
	.destroy = surf_destroy,
	.set_areas = surf_set_areas,
	.set_brush = surf_set_brush,
	.set_brush_object = surf_set_brush_object,
};

static void surf_resource_destroy(struct wl_resource *res)
{
	struct ink_surf *i = wl_resource_get_user_data(res);
	if (i) {
		ink_surf_free(i);
	}
}

static void mgr_get_ink_surface(struct wl_client *c, struct wl_resource *res, uint32_t id,
		struct wl_resource *surface_res)
{
	struct wlr_surface *s = wlr_surface_from_resource(surface_res);
	struct ink_surf *old = ink_surf_for(s);
	if (old && !old->res) {
		ink_surf_free(old);     /* discovered areas: the app's own replace them */
		old = NULL;
	}
	if (old) {
		wl_resource_post_error(res, GLASSINE_INK_MANAGER_V1_ERROR_ALREADY_EXISTS,
			"the surface already has an ink object");
		return;
	}
	struct ink_surf *i = calloc(1, sizeof *i);
	if (!i) {
		wl_client_post_no_memory(c);
		return;
	}
	i->res = wl_resource_create(c, child_iface(res, &glassine_ink_surface_v1_interface, &na1_ink_surface_v1_interface),
		wl_resource_get_version(res), id);
	if (!i->res) {
		free(i);
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(i->res, &surf_impl, i, surf_resource_destroy);
	i->surface = s;
	pixman_region32_init(&i->areas);
	i->surface_destroy.notify = handle_surface_destroy;
	wl_signal_add(&s->events.destroy, &i->surface_destroy);
	wl_list_insert(&ink_surfs, &i->link);
}

static void mgr_destroy(struct wl_client *c, struct wl_resource *res)
{
	wl_resource_destroy(res);
}

static const struct glassine_ink_manager_v1_interface mgr_impl = {
	.destroy = mgr_destroy,
	.get_ink_surface = mgr_get_ink_surface,
	.create_brush = mgr_create_brush,
};

static void mgr_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	const struct wl_interface *iface = data;
	struct wl_resource *res = wl_resource_create(c, iface, version, id);
	if (!res) {
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(res, &mgr_impl, NULL, NULL);
}

/* `glassine_ink X Y W H ...` / `glassine_ink none` on the matched window (window
 * coordinates: from its xdg geometry's origin) */
struct cmd_results *cmd_glassine_ink(int argc, char **argv)
{
	struct sway_container *con = config->handler_context.container;
	if (!con || !con->view || !con->view->surface) {
		return cmd_results_new(CMD_FAILURE, "glassine_ink: no mapped window");
	}
	struct wlr_surface *s = con->view->surface;
	struct ink_surf *i = ink_surf_for(s);
	if (i && i->res) {
		return cmd_results_new(CMD_SUCCESS, NULL);      /* the app's own areas win */
	}
	if (argc == 1 && !strcmp(argv[0], "none")) {
		if (i) {
			ink_surf_free(i);
		}
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	if (argc % 4) {
		return cmd_results_new(CMD_INVALID, "usage: glassine_ink none | glassine_ink X Y W H [X Y W H ...]");
	}
	pixman_region32_t r;
	pixman_region32_init(&r);
	for (int k = 0; k < argc; k += 4) {
		int v[4];
		for (int j = 0; j < 4; j++) {
			char *end;
			v[j] = (int)strtol(argv[k + j], &end, 10);
			if (*end) {
				pixman_region32_fini(&r);
				return cmd_results_new(CMD_INVALID, "glassine_ink: not a number: %s", argv[k + j]);
			}
		}
		if (v[2] > 0 && v[3] > 0) {
			pixman_region32_union_rect(&r, &r, v[0] + con->view->geometry.x, v[1] + con->view->geometry.y, v[2], v[3]);
		}
	}
	if (!i) {
		i = calloc(1, sizeof *i);
		if (!i) {
			pixman_region32_fini(&r);
			return cmd_results_new(CMD_FAILURE, "glassine_ink: out of memory");
		}
		i->surface = s;
		pixman_region32_init(&i->areas);
		i->surface_destroy.notify = handle_surface_destroy;
		wl_signal_add(&s->events.destroy, &i->surface_destroy);
		wl_list_insert(&ink_surfs, &i->link);
	}
	pixman_region32_copy(&i->areas, &r);
	pixman_region32_fini(&r);
	i->set = true;
	update_soon();
	return cmd_results_new(CMD_SUCCESS, NULL);
}

void glassine_ink_init(struct wl_display *display)
{
	wl_list_init(&ink_surfs);
	wl_list_init(&all_brushes);
	/* binds at the client's version (<= 3) */
	wl_global_create(display, &glassine_ink_manager_v1_interface, 3,
		(void *)&glassine_ink_manager_v1_interface, mgr_bind);
	/* the old name, for clients built before the rename (protocols/na1-ink-v1.xml) */
	wl_global_create(display, &na1_ink_manager_v1_interface, 3,
		(void *)&na1_ink_manager_v1_interface, mgr_bind);
}
struct walk { struct item it[MAXB]; int n; };

static struct sway_view *view_of(struct wlr_scene_node *n)
{
	for (; n; n = n->parent ? &n->parent->node : NULL) {
		struct sway_view *v = scene_descriptor_try_get(n, SWAY_SCENE_DESC_VIEW);
		if (v) {
			/* unmapped (closing: sway still shows its saved buffers, but its
			 * toplevel and so its app_id may be gone): covers, never inks */
			return v->surface ? v : NULL;
		}
		if (scene_descriptor_try_get(n, SWAY_SCENE_DESC_POPUP)) {
			return NULL;        /* a menu: above its window, not part of it */
		}
	}
	return NULL;
}

static void collect(struct wlr_scene_buffer *sb, int lx, int ly, void *data)
{
	struct walk *w = data;
	if (w->n >= MAXB || !sb->buffer || sb->opacity <= 0) {
		return;
	}
	int bw, bh;
	if (sb->dst_width > 0 && sb->dst_height > 0) {
		bw = sb->dst_width, bh = sb->dst_height;
	} else {
		bw = sb->buffer_width, bh = sb->buffer_height;
		wlr_output_transform_coords(sb->transform, &bw, &bh);
	}
	if (bw <= 0 || bh <= 0) {
		return;
	}
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
	int ox = ss ? lx - ss->clip.x : lx, oy = ss ? ly - ss->clip.y : ly;
	w->it[w->n++] = (struct item){ { lx, ly, bw, bh }, ox, oy, view_of(&sb->node), ss ? ss->surface : NULL };
}

static void app_name(struct sway_view *v, char *c, size_t len)
{
	const char *id = view_get_app_id(v);
	c[0] = 0;
	if (id && *id) {
		snprintf(c, len, "%s", id);
	} else {
		const char *cl = view_get_class(v), *in = view_get_instance(v);
		snprintf(c, len, "%s %s", cl ? cl : "", in ? in : "");
	}
	for (char *p = c; *p; p++) {
		*p = tolower((unsigned char)*p);
		if (*p == '\n' || *p == '\r') {
			*p = ' ';
		}
	}
}

static int rotation(void)
{
	static struct timespec mt;
	static int rot;
	struct stat st;
	if (stat("/run/glassine-rotation", &st) < 0) {
		return rot = 0;
	}
	if (st.st_mtim.tv_sec != mt.tv_sec || st.st_mtim.tv_nsec != mt.tv_nsec) {
		mt = st.st_mtim;
		FILE *f = fopen("/run/glassine-rotation", "r");
		if (!f || fscanf(f, "%d", &rot) != 1) {
			rot = 0;
		}
		if (f) {
			fclose(f);
		}
	}
	return rot;
}

static long long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

void glassine_ink_update(struct sway_output *output)
{
	static char last[MSG];
	static long long sent;
	struct walk w = { .n = 0 };
	wlr_scene_node_for_each_buffer(&root->layer_tree->node, collect, &w);

	struct wlr_box ob;
	wlr_output_layout_get_box(root->output_layout, output->wlr_output, &ob);
	float sc = output->wlr_output->scale;
	int ow = (int)(ob.width * sc + 0.5), oh = (int)(ob.height * sc + 0.5), rot = rotation();

	struct sway_view *vs[MAXV];
	struct ink_surf *bs[MAXV];      /* the window's brush (an ink_surf with set_brush) */
	pixman_region32_t vis[MAXV], sdk[MAXV], occl;
	bool has_sdk[MAXV];
	int nv = 0;
	pixman_region32_init_rect(&occl, 0, 0, 0, 0);
	for (int i = w.n - 1; i >= 0; i--) {                /* top to bottom */
		struct wlr_box b = w.it[i].b;
		if (!wlr_box_intersection(&b, &b, &ob)) {
			continue;
		}
		if (w.it[i].v) {
			int k = 0;
			while (k < nv && vs[k] != w.it[i].v) {
				k++;
			}
			if (k == nv && nv < MAXV) {
				vs[nv] = w.it[i].v;
				has_sdk[nv] = false;
				bs[nv] = NULL;
				pixman_region32_init(&sdk[nv]);
				pixman_region32_init(&vis[nv++]);
			}
			if (k < nv) {
				pixman_region32_t r;
				pixman_region32_init_rect(&r, b.x, b.y, b.width, b.height);
				pixman_region32_subtract(&r, &r, &occl);
				pixman_region32_union(&vis[k], &vis[k], &r);
				pixman_region32_fini(&r);
				struct ink_surf *is = w.it[i].s ? ink_surf_for(w.it[i].s) : NULL;
				if (is && (is->br[0].kind || is->br[1].kind) && !bs[k]) {
					bs[k] = is;
				}
				if (is && is->set) {        /* the app's areas, from its surface's origin */
					pixman_region32_t a;
					pixman_region32_init(&a);
					pixman_region32_copy(&a, &is->areas);
					pixman_region32_translate(&a, w.it[i].ox, w.it[i].oy);
					pixman_region32_union(&sdk[k], &sdk[k], &a);
					pixman_region32_fini(&a);
					has_sdk[k] = true;
				}
			}
		}
		pixman_region32_union_rect(&occl, &occl, b.x, b.y, b.width, b.height);
	}
	pixman_region32_fini(&occl);

	char msg[MSG];
	int n = snprintf(msg, sizeof msg, "ink begin\n"), nb = 0, bk[MAXV];
	for (int k = 0; k < nv; k++) {              /* the brushes first: widths x the output scale */
		bk[k] = 0;
		if (bs[k] && n < (int)sizeof msg - 400) {
			bk[k] = ++nb;
			unsigned kind[2];
			const char *id[2];
			for (int t = 0; t < 2; t++) {  /* a brush object: KIND by its ink, its file named */
				struct ink_brush *o = bs[k]->br[t].obj;
				id[t] = o ? brush_prepare(o, rot) : NULL;
				kind[t] = o ? (o->d.kind == NB_ERASE ? 3 : o->d.kind == NB_NONE ? 1 : 2) : bs[k]->br[t].kind;
			}
			n += snprintf(msg + n, sizeof msg - n, "ink brush %d %u %.2f %.2f %.3f %u %.2f %.2f %.3f%s%s%s%s\n", nb,
				kind[0], bs[k]->br[0].wmin * sc, bs[k]->br[0].wmax * sc, bs[k]->br[0].exp,
				kind[1], bs[k]->br[1].wmin * sc, bs[k]->br[1].wmax * sc, bs[k]->br[1].exp,
				id[0] ? " tip=" : "", id[0] ? id[0] : "", id[1] ? " rub=" : "", id[1] ? id[1] : "");
		}
	}
	for (int k = 0; k < nv; k++) {
		char app[128], line[MSG];
		int ln = 0, nr;
		app_name(vs[k], app, sizeof app);
		if (has_sdk[k]) {
			pixman_region32_intersect(&vis[k], &vis[k], &sdk[k]);
		}
		pixman_region32_fini(&sdk[k]);
		pixman_box32_t *r = pixman_region32_rectangles(&vis[k], &nr);
		for (int j = 0; j < nr && ln < (int)sizeof line - 200; j++) {
			int b[4] = { (int)((r[j].x1 - ob.x) * sc), (int)((r[j].y1 - ob.y) * sc),
				(int)((r[j].x2 - r[j].x1) * sc + 0.999), (int)((r[j].y2 - r[j].y1) * sc + 0.999) }, o[4];
			switch (rot) {          /* back to portrait px (glassine-popup-hint.c) */
			case 270: o[0] = b[1]; o[1] = ow - b[0] - b[2]; o[2] = b[3]; o[3] = b[2]; break;
			case 90: o[0] = oh - b[1] - b[3]; o[1] = b[0]; o[2] = b[3]; o[3] = b[2]; break;
			case 180: o[0] = ow - b[0] - b[2]; o[1] = oh - b[1] - b[3]; o[2] = b[2]; o[3] = b[3]; break;
			default: memcpy(o, b, sizeof o);
			}
			if (bk[k]) {            /* (the plain form without a brush: an older glassine-epdd-desktop still reads it) */
				ln += snprintf(line + ln, sizeof line - ln, "ink r %d %d %d %d %d %d %s\n", bk[k], has_sdk[k], o[0], o[1], o[2], o[3], app);
			} else {
				ln += snprintf(line + ln, sizeof line - ln, "ink %d %d %d %d %d %s\n", has_sdk[k], o[0], o[1], o[2], o[3], app);
			}
		}
		pixman_region32_fini(&vis[k]);
		/* a window whose rects don't fit is left out (no ink there), never cut */
		if (n + ln < (int)sizeof msg - 16) {
			memcpy(msg + n, line, ln);
			n += ln;
		}
	}
	n += snprintf(msg + n, sizeof msg - n, "ink end\n");

	long long t = now_ms();
	if (!strcmp(msg, last) && t - sent < 2000) {
		return;
	}
	int fd = open(EPDD_CTL, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		last[0] = 0;
		return;
	}
	if (write(fd, msg, n) == n) {
		memcpy(last, msg, n + 1);
		sent = t;
	} else {
		last[0] = 0;            /* a full FIFO: again on the next commit */
	}
	close(fd);
}
