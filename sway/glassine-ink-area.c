/* glassine-ink-area.c — the pen's ink areas for glassine-epdd-desktop.
 * Built into the patched sway (0007). After each output commit sway walks the
 * scene top to bottom: every window's visible region is its buffers minus every
 * buffer above them (menus, the shell's layer surfaces, the keyboard, other
 * windows). glassine-epdd-desktop inks the pen only inside the regions of apps with ink=on.
 * Sent to glassine-epdd-desktop's FIFO when it changes (and again every 2 s on a commit, so a
 * restarted epdd gets them), in portrait panel px (layout px x the output scale,
 * turned back by glassine-rotd's /run/glassine-rotation, as the popup hints):
 * ink begin / ink brush K ... / per owner: ink win KEY, [ink vis X Y W H ...],
 * ink r K S X Y W H APP (ink S X Y W H APP without a brush) ... / ink end
 * (S 1 = the app registered areas, S 2 = a writing region's; K: the window's brush,
 * 0 = the per-app one; brush lines: K TKIND TMIN TMAX TEXP RKIND RMIN RMAX REXP, tip
 * and eraser end, widths in panel px, see glassine-epdd-desktop's ink_line_cmd)
 * APP as glassine-swayd names it: the app_id, else "class instance", lower case.
 * KEY names the owner (a window, popup or layer surface) for as long as it lives: its
 * client's pid and its root surface's object id, "PID.ID". The panel daemon gives a
 * stroke, as sway does (the tablet's implicit grab, a region takes what starts in it),
 * to where it starts: a stroke starting outside every ink area never inks, one started
 * in an area inks only on its owner's part of the panel (GlassineOS/epd epd-inkarea.h).
 * ink vis lines, the owner's whole visible region, go out for owners with writing
 * regions: a stroke a region takes inks over the window as far as it is visible, also
 * past the region's edge (sway hands the app the whole stroke), never over what covers
 * it. When they don't fit in the message, they are left out (the ink then stops at the
 * region's edge), before the owner's rects are.
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
 * Besides windows, two kinds of surfaces without a view ink when their client
 * registered areas: input-method popups (the on-screen keyboard's handwriting
 * paper) and layer-shell surfaces, each with its subsurfaces, at its current
 * position and cut by everything above it like a window. They have no per-app
 * default: without areas of their own (or with null areas) they only cover.
 * APP: the layer surface's namespace, "input-method" for a popup. Menus
 * (xdg popups) always only cover.
 * Writing regions (glassine_handwriting_v1 version 2, glassine-handwriting.c) are ink
 * areas too: a region in ink mode is where the pen writes, so its rectangle and clip, less
 * every region stacked above it that is in text mode, join its surface's areas as the
 * app's own (S 1), whether reading is on or off and whether the field is private. A
 * surface in region mode inks only in its regions (and areas it registered itself), never
 * in the rest of its window: a stroke starting outside every region isn't delivered. The
 * region's rects carry the surface's brush when its client set one, else a brush with the
 * eraser end off (sway gives handwriting no eraser strokes, so the panel would show an
 * erasure the app never gets). Where a region overlaps an area the app registered, the
 * region's rect wins (a stroke starting there is the region's). Layer surfaces and
 * input-method popups with regions ink as with areas.
 * Version 4, the content hint (set_content): what the app says its next commit is, a
 * new page or a step of a movement it drives. It is latched when that commit is applied
 * and goes out with the next frame of the output the surface is on
 * (glassine_ink_content_take, sway/desktop/output.c -> the glassine-epd backend's FRAME),
 * with the surface's place in the frame's buffer; then it is gone. A surface that is
 * not shown loses it.
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
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_layer_shell_v1.h>
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
#include "glassine-handwriting.h"
#include "glassine-ink-area.h"
#include "glassine-ink-v1-protocol.h"
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
/* v: the window it belongs to; own: else the input-method popup or layer
 * surface it belongs to (its root surface), NULL for anything else (menus,
 * drag icons...: those only cover) */
struct item { struct wlr_box b; int ox, oy; struct sway_view *v; struct wlr_surface *s, *own; };

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
	uint32_t content_next;          /* set_content: for the next commit */
	uint32_t content;               /* latched at that commit, until the output's next frame takes it */
	struct wl_listener surface_commit;
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

void glassine_ink_changed(void)
{
	update_soon();
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

/* a surface's writing regions: what of them takes strokes, topmost first */
struct hw_acc { pixman_region32_t ink, taken; };

static void hw_region(void *data, const double rect[4], void *clip, bool ink)
{
	struct hw_acc *h = data;
	int x0 = (int)floor(rect[0]), y0 = (int)floor(rect[1]);
	int x1 = (int)ceil(rect[0] + rect[2]), y1 = (int)ceil(rect[1] + rect[3]);
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	pixman_region32_t a;
	pixman_region32_init_rect(&a, x0, y0, x1 - x0, y1 - y0);
	if (clip) {
		pixman_region32_intersect(&a, &a, clip);
	}
	if (ink) {
		pixman_region32_t f;
		pixman_region32_init(&f);
		pixman_region32_subtract(&f, &a, &h->taken);
		pixman_region32_union(&h->ink, &h->ink, &f);
		pixman_region32_fini(&f);
	}
	pixman_region32_union(&h->taken, &h->taken, &a);
	pixman_region32_fini(&a);
}

/* the surface's writing regions in ink mode into out (surface coordinates, initialised
 * by the caller): false when it has no regions */
static bool hw_areas(struct wlr_surface *s, pixman_region32_t *out)
{
	struct hw_acc h;
	pixman_region32_init(&h.ink);
	pixman_region32_init(&h.taken);
	bool on = s && s->resource && glassine_hw_regions(s->resource, hw_region, &h);
	pixman_region32_copy(out, &h.ink);
	pixman_region32_fini(&h.ink);
	pixman_region32_fini(&h.taken);
	return on;
}

bool glassine_ink_has_areas(struct wlr_surface *s)
{
	struct ink_surf *i = s ? ink_surf_for(s) : NULL;
	if (i && i->set && pixman_region32_not_empty(&i->areas)) {
		return true;
	}
	pixman_region32_t r;
	pixman_region32_init(&r);
	bool on = hw_areas(s, &r) && pixman_region32_not_empty(&r);
	pixman_region32_fini(&r);
	return on;
}

static void brush_unuse(struct ink_brush *b);

static void ink_surf_free(struct ink_surf *i)
{
	brush_unuse(i->br[0].obj);
	brush_unuse(i->br[1].obj);
	wl_list_remove(&i->link);
	wl_list_remove(&i->surface_destroy.link);
	wl_list_remove(&i->surface_commit.link);
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

/* the commit set_content was for is applied: its kind waits for the output's frame. A
 * commit without one says nothing, and takes back what an older commit said, if no
 * frame showed that one yet (the frame shows the newer picture) */
static void handle_surface_commit(struct wl_listener *l, void *data)
{
	struct ink_surf *i = wl_container_of(l, i, surface_commit);
	i->content = i->content_next;
	i->content_next = 0;
}

static void surf_set_content(struct wl_client *c, struct wl_resource *res, uint32_t content)
{
	struct ink_surf *i = wl_resource_get_user_data(res);
	if (!i) {
		return;
	}
	i->content_next = content == GLASSINE_INK_SURFACE_V1_CONTENT_PAGE ||
		content == GLASSINE_INK_SURFACE_V1_CONTENT_MOTION ? content : 0;
}

struct said { struct wlr_surface *s; struct wlr_box b; bool found; };

static void said_find(struct wlr_scene_buffer *sb, int lx, int ly, void *data)
{
	struct said *f = data;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
	if (f->found || !ss || ss->surface != f->s || !sb->buffer) {
		return;
	}
	int bw = sb->dst_width, bh = sb->dst_height;
	if (bw <= 0 || bh <= 0) {
		bw = sb->buffer_width, bh = sb->buffer_height;
		wlr_output_transform_coords(sb->transform, &bw, &bh);
	}
	f->b = (struct wlr_box){ lx, ly, bw, bh };
	f->found = bw > 0 && bh > 0;
}

uint32_t glassine_ink_content_take(struct sway_output *output, struct wlr_box *box)
{
	uint32_t kind = 0;
	struct wlr_box ob, un = { 0 };
	wlr_output_layout_get_box(root->output_layout, output->wlr_output, &ob);
	struct ink_surf *i;
	wl_list_for_each(i, &ink_surfs, link) {
		if (!i->content) {
			continue;
		}
		struct said f = { .s = i->surface };
		wlr_scene_node_for_each_buffer(&root->root_scene->tree.node, said_find, &f);
		struct wlr_box in;
		if (f.found && !wlr_box_intersection(&in, &f.b, &ob)) {
			continue;       /* on another output: that one's frame takes it */
		}
		uint32_t k = i->content;
		i->content = 0;         /* not shown at all: lost */
		if (!f.found) {
			continue;
		}
		/* several surfaces in one frame: a page wins (the frame is then exact), over the area of all */
		if (!kind) {
			un = in;
		} else {
			int x1 = un.x + un.width > in.x + in.width ? un.x + un.width : in.x + in.width;
			int y1 = un.y + un.height > in.y + in.height ? un.y + un.height : in.y + in.height;
			un.x = un.x < in.x ? un.x : in.x;
			un.y = un.y < in.y ? un.y : in.y;
			un.width = x1 - un.x, un.height = y1 - un.y;
		}
		if (!kind || k == GLASSINE_INK_SURFACE_V1_CONTENT_PAGE) {
			kind = k;
		}
	}
	if (!kind) {
		return 0;
	}
	/* layout px -> the frame's buffer: the output's scale, then its transform undone */
	float sc = output->wlr_output->scale;
	int x0 = (int)floorf((un.x - ob.x) * sc), y0 = (int)floorf((un.y - ob.y) * sc);
	int x1 = (int)ceilf((un.x - ob.x + un.width) * sc), y1 = (int)ceilf((un.y - ob.y + un.height) * sc);
	struct wlr_box b = { x0, y0, x1 - x0, y1 - y0 };
	int tw, th;
	wlr_output_transformed_resolution(output->wlr_output, &tw, &th);
	wlr_box_transform(box, &b, wlr_output_transform_invert(output->wlr_output->transform), tw, th);
	return kind;
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

static void mgr_create_brush(struct wl_client *c, struct wl_resource *res, uint32_t id)
{
	struct ink_brush *b = calloc(1, sizeof *b);
	if (!b) {
		wl_client_post_no_memory(c);
		return;
	}
	b->res = wl_resource_create(c, &glassine_ink_brush_v1_interface, wl_resource_get_version(res), id);
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
	.set_content = surf_set_content,
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
	i->res = wl_resource_create(c, &glassine_ink_surface_v1_interface, wl_resource_get_version(res), id);
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
	i->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&s->events.commit, &i->surface_commit);
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
	struct wl_resource *res = wl_resource_create(c, &glassine_ink_manager_v1_interface, version, id);
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
		wl_list_init(&i->surface_commit.link);  /* (discovered areas: nobody says what a commit is) */
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
	wl_global_create(display, &glassine_ink_manager_v1_interface, 4, NULL, mgr_bind);     /* binds at the client's version (<= 4) */
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

/* the surface's view-less ink owner: its root surface when that is an
 * input-method popup or a layer surface (an xdg popup's root is itself) and
 * its client set areas on it or a subsurface; else NULL (it only covers, and
 * takes none of the MAXV slots) */
static void has_regions(struct wlr_surface *s, int sx, int sy, void *data)
{
	if (s->resource && glassine_hw_regions(s->resource, NULL, NULL)) {
		*(bool *)data = true;
	}
}

static struct wlr_surface *owner_of(struct wlr_surface *s)
{
	struct wlr_surface *r = s ? wlr_surface_get_root_surface(s) : NULL;
	if (!r || !(wlr_input_popup_surface_v2_try_from_wlr_surface(r) ||
			wlr_layer_surface_v1_try_from_wlr_surface(r))) {
		return NULL;
	}
	struct ink_surf *i;
	wl_list_for_each(i, &ink_surfs, link) {
		if (i->res && i->set && wlr_surface_get_root_surface(i->surface) == r) {
			return r;
		}
	}
	bool hw = false;        /* writing regions on it or a subsurface */
	wlr_surface_for_each_surface(r, has_regions, &hw);
	return hw ? r : NULL;
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
	struct sway_view *v = view_of(&sb->node);
	struct wlr_surface *s = ss ? ss->surface : NULL;
	w->it[w->n++] = (struct item){ { lx, ly, bw, bh }, ox, oy, v, s, v ? NULL : owner_of(s) };
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

/* a view-less owner's APP: the layer namespace, else "input-method" */
static void owner_name(struct wlr_surface *own, char *c, size_t len)
{
	struct wlr_layer_surface_v1 *l = wlr_layer_surface_v1_try_from_wlr_surface(own);
	snprintf(c, len, "%s", l && l->namespace && *l->namespace ? l->namespace : l ? "layer-shell" : "input-method");
	for (char *p = c; *p; p++) {
		*p = tolower((unsigned char)*p);
		if (isspace((unsigned char)*p)) {
			*p = '-';       /* one token, as the line's last field */
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

/* a region's box (layout px) -> portrait panel px o[4] (glassine-popup-hint.c) */
static void panel_rect(const pixman_box32_t *r, const struct wlr_box *ob, float sc, int ow, int oh, int rot, int o[4])
{
	int b[4] = { (int)((r->x1 - ob->x) * sc), (int)((r->y1 - ob->y) * sc),
		(int)((r->x2 - r->x1) * sc + 0.999), (int)((r->y2 - r->y1) * sc + 0.999) };
	switch (rot) {
	case 270: o[0] = b[1]; o[1] = ow - b[0] - b[2]; o[2] = b[3]; o[3] = b[2]; break;
	case 90: o[0] = oh - b[1] - b[3]; o[1] = b[0]; o[2] = b[3]; o[3] = b[2]; break;
	case 180: o[0] = ow - b[0] - b[2]; o[1] = oh - b[1] - b[3]; o[2] = b[2]; o[3] = b[3]; break;
	default: memcpy(o, b, 4 * sizeof *o);
	}
}

/* the owner's KEY: its client's pid and its root surface's object id */
static void owner_key(struct wlr_surface *s, char *c, size_t len)
{
	pid_t pid = 0;
	uint32_t id = 0;
	if (s && s->resource) {
		wl_client_get_credentials(wl_resource_get_client(s->resource), &pid, NULL, NULL);
		id = wl_resource_get_id(s->resource);
	}
	snprintf(c, len, "%d.%u", (int)pid, id);
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

	struct sway_view *vs[MAXV];     /* the windows, or NULL: */
	struct wlr_surface *os[MAXV];   /* a view-less owner (popup, layer surface) */
	struct ink_surf *bs[MAXV];      /* the window's brush (an ink_surf with set_brush) */
	pixman_region32_t vis[MAXV], sdk[MAXV], hwa[MAXV], occl;
	bool has_sdk[MAXV], has_hw[MAXV];
	int nv = 0;
	pixman_region32_init_rect(&occl, 0, 0, 0, 0);
	for (int i = w.n - 1; i >= 0; i--) {                /* top to bottom */
		struct wlr_box b = w.it[i].b;
		if (!wlr_box_intersection(&b, &b, &ob)) {
			continue;
		}
		if (w.it[i].v || w.it[i].own) {
			int k = 0;
			while (k < nv && (vs[k] != w.it[i].v || os[k] != w.it[i].own)) {
				k++;
			}
			if (k == nv && nv < MAXV) {
				vs[nv] = w.it[i].v;
				os[nv] = w.it[i].own;
				has_sdk[nv] = false;
				has_hw[nv] = false;
				bs[nv] = NULL;
				pixman_region32_init(&sdk[nv]);
				pixman_region32_init(&hwa[nv]);
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
				/* the app's areas, from its surface's origin (a view-less
				 * owner's only from its client, never from the command) */
				if (is && is->set && (w.it[i].v || is->res)) {
					pixman_region32_t a;
					pixman_region32_init(&a);
					pixman_region32_copy(&a, &is->areas);
					pixman_region32_translate(&a, w.it[i].ox, w.it[i].oy);
					pixman_region32_union(&sdk[k], &sdk[k], &a);
					pixman_region32_fini(&a);
					has_sdk[k] = true;
				}
				/* its writing regions in ink mode, from the same origin */
				pixman_region32_t a;
				pixman_region32_init(&a);
				if (w.it[i].s && hw_areas(w.it[i].s, &a)) {
					pixman_region32_translate(&a, w.it[i].ox, w.it[i].oy);
					pixman_region32_union(&hwa[k], &hwa[k], &a);
					has_hw[k] = true;
				}
				pixman_region32_fini(&a);
			}
		}
		pixman_region32_union_rect(&occl, &occl, b.x, b.y, b.width, b.height);
	}
	pixman_region32_fini(&occl);

	char msg[MSG];
	int n = snprintf(msg, sizeof msg, "ink begin\n"), nb = 0, bk[MAXV], hb = 0;
	for (int k = 0; k < nv; k++) {              /* the brushes first: widths x the output scale */
		bk[k] = 0;
		if (has_hw[k]) {
			has_sdk[k] = true;      /* a surface in region mode inks only in its areas */
			pixman_region32_subtract(&sdk[k], &sdk[k], &hwa[k]);    /* a region wins over an area */
		}
		if (bs[k] && (vs[k] || has_sdk[k]) && n < (int)sizeof msg - 400) {
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
		if (has_hw[k] && !bk[k] && !hb && n < (int)sizeof msg - 400) {
			/* the regions' brush without one of the surface's: the tip as the app's
			 * profile has it (kind 0), the eraser end off (kind 1) */
			hb = ++nb;
			n += snprintf(msg + n, sizeof msg - n, "ink brush %d 0 0.00 0.00 1.000 1 0.00 0.00 1.000\n", hb);
		}
	}
	for (int k = 0; k < nv; k++) {
		char app[128], key[32], line[MSG], vl[MSG];
		int ln = 0, vn = 0, nr, o[4];
		if (!vs[k] && !has_sdk[k]) {    /* a popup or layer surface without areas: covers only */
			pixman_region32_fini(&sdk[k]);
			pixman_region32_fini(&hwa[k]);
			pixman_region32_fini(&vis[k]);
			continue;
		}
		if (vs[k]) {
			app_name(vs[k], app, sizeof app);
		} else {
			owner_name(os[k], app, sizeof app);
		}
		owner_key(vs[k] ? vs[k]->surface : os[k], key, sizeof key);
		/* a surface with regions: its whole visible region, where a stroke a region takes inks */
		pixman_box32_t *r = pixman_region32_rectangles(&vis[k], &nr);
		for (int j = 0; has_hw[k] && j < nr && vn < (int)sizeof vl - 200; j++) {
			panel_rect(&r[j], &ob, sc, ow, oh, rot, o);
			vn += snprintf(vl + vn, sizeof vl - vn, "ink vis %d %d %d %d\n", o[0], o[1], o[2], o[3]);
		}
		/* the regions' part with their brush (S 2), then the rest */
		pixman_region32_t hv;
		pixman_region32_init(&hv);
		pixman_region32_intersect(&hv, &vis[k], &hwa[k]);
		pixman_region32_fini(&hwa[k]);
		if (has_sdk[k]) {
			pixman_region32_intersect(&vis[k], &vis[k], &sdk[k]);
		}
		pixman_region32_fini(&sdk[k]);
		for (int pass = 0; pass < 2; pass++) {
			pixman_region32_t *rg = pass ? &hv : &vis[k];
			int kb = pass && !bk[k] ? hb : bk[k], sk = pass ? 2 : has_sdk[k];
			r = pixman_region32_rectangles(rg, &nr);
			for (int j = 0; j < nr && ln < (int)sizeof line - 200; j++) {
				panel_rect(&r[j], &ob, sc, ow, oh, rot, o);
				if (kb) {               /* (the plain form without a brush: an older glassine-epdd-desktop still reads it) */
					ln += snprintf(line + ln, sizeof line - ln, "ink r %d %d %d %d %d %d %s\n", kb, sk, o[0], o[1], o[2], o[3], app);
				} else {
					ln += snprintf(line + ln, sizeof line - ln, "ink %d %d %d %d %d %s\n", sk, o[0], o[1], o[2], o[3], app);
				}
			}
		}
		pixman_region32_fini(&hv);
		pixman_region32_fini(&vis[k]);
		if (!ln) {
			continue;       /* all of it covered: nothing to send */
		}
		/* a window whose rects don't fit is left out (no ink there), never cut; its
		 * visible region goes first (the ink then stops at its regions' edges) */
		int wn = snprintf(msg + n, sizeof msg - n, "ink win %s\n", key);
		if (n + wn + vn + ln < (int)sizeof msg - 16) {
			memcpy(msg + n + wn, vl, vn);
			n += wn + vn;
		} else if (n + wn + ln < (int)sizeof msg - 16) {
			n += wn;
		} else {
			msg[n] = 0;
			continue;
		}
		memcpy(msg + n, line, ln);
		n += ln;
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
