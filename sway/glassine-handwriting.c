/* glassine-handwriting.c — the pen interface of handwriting (glassine_handwriting_v1,
 * protocols/glassine-handwriting-v1.xml; GlassineOS/handwriting spec 004 and the neat ink
 * layer of spec 003).
 *
 * An app turns it on for a wl_surface. Every pen stroke that starts in the object's area
 * goes to the app as a numbered vector stroke when the pen lifts (stroke event), sent from
 * here without any round trip, so it costs the raw ink nothing. With neatening or text on,
 * the stroke also goes to the handwriting service, which answers with neatened strokes,
 * readings, candidates and the results of reports; those are passed on as events.
 *
 * The service (glassine-handwriting-service) is started from here with one end of a
 * socket pair (--pen-fd 3): only this compositor can speak on that channel, so the app
 * identity it sends can't be claimed by anyone else. Identity: the sandbox's application
 * id (security-context, through the identify hook), else the client's pid, which the
 * service turns into the program it runs. The channel is JSON lines (docs/service.md in
 * GlassineOS/handwriting); writes never block (queued, sent when the socket is writable).
 * Without the service, raw strokes still flow and the capabilities say so.
 *
 * Version 2 adds writing regions (GlassineOS/handwriting spec 005): an object that made
 * one is in region mode, and each stroke goes to the topmost region it starts in, in the
 * region's coordinates, or nowhere. A region reaches the service only once it holds
 * writing (its first stroke, edit or restore) and wants it (reading or neatening on, not
 * private), and only when the service's hello says it reads regions; a move sends nothing.
 * The raw strokes a region sent, its last value and saved state are kept here, so that a
 * restarted service gets them back (region_restore) without the app.
 *
 * A region in ink mode is where the pen writes, so it is also an ink area of its surface:
 * sway's ink areas (glassine-ink-area.c) ask for a surface's regions (glassine_hw_regions)
 * and hear of every change to where they lie (the regions_changed hook), so the pen's raw
 * ink shows under the pen there without the app registering anything else (spec 005). The
 * panel daemon applies the same start rule: a stroke a region takes inks over the visible
 * window, past the region's edge; one starting outside every region doesn't ink.
 *
 * This file uses libwayland-server, json-c and libc only (no wlroots types): sway's glue
 * (cursor.c, server.c) feeds it the pen's tool events and regions through hooks, and the
 * host test's stand-in compositor (test/handwriting-standin.c) does the same. */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <json.h>
#include <wayland-server-core.h>
#include "glassine-handwriting.h"
#include "glassine-handwriting-v1-protocol.h"

#define MAX_POINTS 100000       /* per stroke, as the recognizer takes them */
#define MAX_TEXT 4000
#define RESPAWN_MS 3000
#define MAX_QUEUE (8 << 20)     /* bytes waiting for the service; past it the service is restarted */
#define PART 128                /* points per points event (the wire takes 4096 bytes a message) */
#define MAX_EVENT_STR 1024      /* bytes of a string in an event */
#define MAX_READING 512         /* stroke numbers in a reading */
#define DATA_PART 2048          /* bytes of a data part (regions) */
#define MAX_DATA_TEXT 65536     /* bytes of text gathered from add_data */
#define MAX_DATA_STATE (1 << 20)        /* bytes of saved state gathered from add_data */
#define MAX_NAME 256

struct held { uint32_t k; struct wl_array pts; };
struct hw_region;

struct hw_obj {
	struct wl_list link;
	struct wl_resource *res;
	struct wl_resource *surface;
	struct wl_listener surface_destroy;
	uint32_t sid;                   /* the service's name for it */
	uint32_t next;                  /* the next stroke number */
	void *area;                     /* region copy (hooks), NULL: the whole surface */
	bool neaten, text;
	double strength;
	double spacing, first;          /* the guides (set_guides); spacing 0: none */
	uint32_t purpose, hints;
	char *before, *after, *langs;
	bool announced;                 /* the service knows it (surface + set sent) */
	struct wl_event_source *idle;   /* the choices go to the service once a client's batch is in */
	/* the strokes held (for neat_stroke: the raw points with x, y replaced), by number */
	struct held *held;
	size_t nheld;
	/* version 2: writing regions */
	bool region_mode;               /* made a region once: strokes go to regions only */
	uint32_t mode_all;              /* the mode new regions start in (set_mode) */
	uint32_t region_seq;            /* creation order, for equal layers */
	struct wl_list regions;         /* hw_region.link */
};

struct hw_region {
	struct wl_list link;            /* hw_obj.regions */
	struct wl_resource *res;
	struct hw_obj *obj;             /* NULL once the object is gone: the region is inert */
	uint32_t rid;                   /* the service's name for it */
	uint32_t seq;
	char *name;
	double x, y, w, h;              /* surface coordinates */
	void *clip;                     /* region copy (hooks), NULL: the whole rectangle */
	double spacing, first;          /* region coordinates */
	uint32_t purpose, hints;
	char *langs;
	bool reading, neaten;
	double strength;
	int32_t layer;
	uint32_t mode;
	uint32_t display;               /* version 3: its words with ink shown as ink or set */
	uint32_t next;                  /* the next live stroke number */
	bool announced;                 /* the service knows it */
	bool holds;                     /* it holds writing: the service may hear of it */
	bool dirty;                     /* choices changed since the last region_set */
	struct wl_event_source *idle;
	struct held *held;              /* raw strokes the service has, for neat and a restart */
	size_t nheld;
	char *value, *state;            /* the last value's text, its saved state (base64) */
	char *edit;                     /* an edit made while the service wasn't there */
	struct wl_array in_text, in_state;      /* add_data gathered */
	struct wl_array rp;             /* restore_points gathered */
	struct held *rs;                /* restore_stroke gathered */
	size_t nrs;
};

static struct {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct glassine_hw_hooks hooks;
	struct wl_list objs;            /* hw_obj.link */
	struct wl_list managers;        /* manager resources (wl_resource links) */
	uint32_t next_sid;
	/* the service */
	int fd;
	pid_t pid;
	struct wl_event_source *src, *timer;
	char *in; size_t nin, cin;
	char *out; size_t nout, cout;
	bool up;                        /* said hello */
	int fails;                      /* starts in a row that never said hello: 5 and it stays off */
	uint32_t caps;
	char **langs; size_t nlangs;
	char *cmd;
	bool regions;                   /* the service reads regions (hello) */
	uint32_t next_rid;
	/* the stroke being written */
	struct hw_obj *cur;
	struct hw_region *cur_r;        /* in region mode: its region */
	double rx, ry;                  /* that region's corner on the surface at pen-down */
	struct wl_array pts;
	double ox, oy;                  /* the surface's origin in the caller's coordinates */
	double pressure, tilt_x, tilt_y;
	double per_mm;
} H = { .fd = -1 };

/* ---- the service's channel ---- */

static void svc_close(void);
struct hw_region;

/* where the regions lie changed: the ink areas follow */
static void areas_changed(void)
{
	if (H.hooks.regions_changed) H.hooks.regions_changed();
}
static void region_replay(struct hw_region *r);

static void svc_flush(void)
{
	while (H.nout) {
		ssize_t w = send(H.fd, H.out, H.nout, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (w < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) break;
			svc_close();
			return;
		}
		memmove(H.out, H.out + w, H.nout - (size_t)w);
		H.nout -= (size_t)w;
	}
	if (H.src) wl_event_source_fd_update(H.src, WL_EVENT_READABLE | (H.nout ? WL_EVENT_WRITABLE : 0));
}

static void svc_send(json_object *m)
{
	if (H.fd < 0) { json_object_put(m); return; }
	size_t len;
	const char *s = json_object_to_json_string_length(m, JSON_C_TO_STRING_PLAIN, &len);
	if (H.nout + len + 1 > MAX_QUEUE) { json_object_put(m); svc_close(); return; }
	if (H.nout + len + 1 > H.cout) {
		size_t c = H.cout ? H.cout : 4096;
		while (c < H.nout + len + 1) c *= 2;
		char *o = realloc(H.out, c);
		if (!o) { json_object_put(m); return; }
		H.out = o; H.cout = c;
	}
	memcpy(H.out + H.nout, s, len);
	H.out[H.nout + len] = '\n';
	H.nout += len + 1;
	json_object_put(m);
	svc_flush();
}

static json_object *msg(const char *op, uint32_t sid)
{
	json_object *m = json_object_new_object();
	json_object_object_add(m, "op", json_object_new_string(op));
	if (sid) json_object_object_add(m, "s", json_object_new_int64(sid));
	return m;
}

static bool wants_service(const struct hw_obj *o)
{
	/* nothing goes to the service for a private field or with both off (FR-010) */
	bool private = o->purpose == GLASSINE_HANDWRITING_V1_PURPOSE_PASSWORD ||
		       o->purpose == GLASSINE_HANDWRITING_V1_PURPOSE_PIN || (o->hints & (0x40 | 0x80));
	return (o->neaten || o->text) && !private;
}

static void units(char *u, size_t n)
{
	if (H.per_mm > 0) snprintf(u, n, "device:%.4g", H.per_mm);
	else snprintf(u, n, "device");
}

static void send_set(struct hw_obj *o)
{
	json_object *m = msg("set", o->sid);
	json_object_object_add(m, "neaten", json_object_new_boolean(o->neaten));
	json_object_object_add(m, "strength", json_object_new_double(o->strength));
	json_object_object_add(m, "text", json_object_new_boolean(o->text));
	json_object_object_add(m, "purpose", json_object_new_int64(o->purpose));
	json_object_object_add(m, "hints", json_object_new_int64(o->hints));
	json_object_object_add(m, "before", json_object_new_string(o->before ? o->before : ""));
	json_object_object_add(m, "after", json_object_new_string(o->after ? o->after : ""));
	json_object_object_add(m, "languages", json_object_new_string(o->langs ? o->langs : ""));
	json_object *g = json_object_new_array();
	json_object_array_add(g, json_object_new_double(o->spacing));
	json_object_array_add(g, json_object_new_double(o->first));
	json_object_object_add(m, "guides", g);
	if (o->area && H.hooks.region_box) {
		int b[4] = { 0 };
		H.hooks.region_box(o->area, b);
		json_object *a = json_object_new_array();
		for (int i = 0; i < 4; i++) json_object_array_add(a, json_object_new_int(b[i]));
		json_object_object_add(m, "box", a);
	}
	char u[64];
	units(u, sizeof u);
	json_object_object_add(m, "units", json_object_new_string(u));
	svc_send(m);
}

/* the service learns whose the surface is */
static void send_surface(struct hw_obj *o)
{
	json_object *m = msg("surface", o->sid);
	struct wl_client *c = wl_resource_get_client(o->res);
	char sandbox[256] = "";
	if (H.hooks.identify) H.hooks.identify(c, sandbox, sizeof sandbox);
	pid_t pid; uid_t uid; gid_t gid;
	wl_client_get_credentials(c, &pid, &uid, &gid);
	if (sandbox[0]) json_object_object_add(m, "sandbox", json_object_new_string(sandbox));
	json_object_object_add(m, "pid", json_object_new_int64(pid));
	svc_send(m);
	o->announced = true;
}

/* the service learns of a surface once it is there and the surface wants it */
static void announce(struct hw_obj *o)
{
	if (H.fd < 0 || !H.up || o->region_mode) return;   /* regions announce the surface themselves */
	if (o->announced && !wants_service(o)) {
		/* now private or off: the service forgets the surface and all it held */
		svc_send(msg("destroy", o->sid));
		o->announced = false;
		return;
	}
	if (!o->announced) {
		if (!wants_service(o)) return;
		send_surface(o);
	}
	send_set(o);
}

static void send_caps(struct wl_resource *mgr)
{
	uint32_t caps = H.caps | GLASSINE_HANDWRITING_MANAGER_V1_CAPABILITY_STROKES;
	if (wl_resource_get_version(mgr) < 2) caps &= ~(uint32_t)GLASSINE_HANDWRITING_MANAGER_V1_CAPABILITY_REGIONS;
	glassine_handwriting_manager_v1_send_capabilities(mgr, caps);
	for (size_t i = 0; i < H.nlangs; i++) glassine_handwriting_manager_v1_send_language(mgr, H.langs[i]);
	glassine_handwriting_manager_v1_send_done(mgr);
}

static void caps_changed(void)
{
	struct wl_resource *r;
	wl_resource_for_each(r, &H.managers) send_caps(r);
}

/* a stroke's points in parts of PART points, then the event that ends them */
static void send_points(struct hw_obj *o, uint32_t k, uint32_t kind, const struct wl_array *pts)
{
	const size_t point = 6 * sizeof(int32_t);
	size_t n = pts->size / point;
	for (size_t i = 0; i < n; i += PART) {
		struct wl_array a = { .size = (n - i < PART ? n - i : PART) * point, .alloc = 0,
				      .data = (char *)pts->data + i * point };
		glassine_handwriting_v1_send_points(o->res, k, kind, &a);
	}
}

/* s cut to at most MAX_EVENT_STR bytes, on a character's start, into buf */
static const char *cut(const char *s, char *buf)
{
	size_t n = strlen(s);
	if (n <= MAX_EVENT_STR) return s;
	n = MAX_EVENT_STR;
	while (n && ((unsigned char)s[n] & 0xc0) == 0x80) n--;
	memcpy(buf, s, n);
	buf[n] = 0;
	return buf;
}

static struct hw_obj *obj_of(uint32_t sid)
{
	struct hw_obj *o;
	wl_list_for_each(o, &H.objs, link) if (o->sid == sid) return o;
	return NULL;
}

static struct held *held_of(struct hw_obj *o, uint32_t k)
{
	for (size_t i = 0; i < o->nheld; i++) if (o->held[i].k == k) return &o->held[i];
	return NULL;
}

static void held_clear(struct hw_obj *o)
{
	for (size_t i = 0; i < o->nheld; i++) wl_array_release(&o->held[i].pts);
	free(o->held);
	o->held = NULL;
	o->nheld = 0;
}

static void got_hello(json_object *m)
{
	json_object *v;
	H.caps = 0;
	if (json_object_object_get_ex(m, "neaten", &v) && json_object_get_boolean(v))
		H.caps |= GLASSINE_HANDWRITING_MANAGER_V1_CAPABILITY_NEATEN;
	if (json_object_object_get_ex(m, "text", &v) && json_object_get_boolean(v))
		H.caps |= GLASSINE_HANDWRITING_MANAGER_V1_CAPABILITY_TEXT;
	if (json_object_object_get_ex(m, "letters", &v) && json_object_get_boolean(v))
		H.caps |= GLASSINE_HANDWRITING_MANAGER_V1_CAPABILITY_LETTERS;
	H.regions = json_object_object_get_ex(m, "regions", &v) && json_object_get_boolean(v);
	if (H.regions) H.caps |= GLASSINE_HANDWRITING_MANAGER_V1_CAPABILITY_REGIONS;
	for (size_t i = 0; i < H.nlangs; i++) free(H.langs[i]);
	free(H.langs);
	H.langs = NULL; H.nlangs = 0;
	if (json_object_object_get_ex(m, "languages", &v) && json_object_is_type(v, json_type_array)) {
		size_t n = json_object_array_length(v);
		H.langs = calloc(n ? n : 1, sizeof *H.langs);
		for (size_t i = 0; H.langs && i < n; i++) {
			const char *s = json_object_get_string(json_object_array_get_idx(v, i));
			if (s && (H.langs[H.nlangs] = strdup(s))) H.nlangs++;
		}
	}
	H.up = true;
	H.fails = 0;
	caps_changed();
	struct hw_obj *o;
	wl_list_for_each(o, &H.objs, link) {
		announce(o);
		struct hw_region *r;
		wl_list_for_each(r, &o->regions, link) region_replay(r);
	}
}

static void got_neat(struct hw_obj *o, json_object *m)
{
	json_object *v, *pts;
	if (!json_object_object_get_ex(m, "k", &v)) return;
	struct held *h = held_of(o, (uint32_t)json_object_get_int64(v));
	if (!h || !json_object_object_get_ex(m, "points", &pts) || !json_object_is_type(pts, json_type_array)) return;
	size_t n = h->pts.size / (6 * sizeof(int32_t));
	if (json_object_array_length(pts) != n) return;
	uint32_t how = GLASSINE_HANDWRITING_V1_HOW_KEPT;
	if (json_object_object_get_ex(m, "how", &v)) {
		const char *s = json_object_get_string(v);
		if (s && !strcmp(s, "line")) how = GLASSINE_HANDWRITING_V1_HOW_LINE;
		else if (s && !strcmp(s, "pulled")) how = GLASSINE_HANDWRITING_V1_HOW_PULLED;
	}
	struct wl_array a;
	wl_array_init(&a);
	int32_t *q = wl_array_add(&a, h->pts.size);
	if (!q) { wl_array_release(&a); return; }
	memcpy(q, h->pts.data, h->pts.size);
	for (size_t i = 0; i < n; i++) {
		json_object *p = json_object_array_get_idx(pts, i);
		if (!json_object_is_type(p, json_type_array) || json_object_array_length(p) < 2) { wl_array_release(&a); return; }
		q[6 * i] = wl_fixed_from_double(json_object_get_double(json_object_array_get_idx(p, 0)));
		q[6 * i + 1] = wl_fixed_from_double(json_object_get_double(json_object_array_get_idx(p, 1)));
	}
	send_points(o, h->k, GLASSINE_HANDWRITING_V1_KIND_NEAT, &a);
	glassine_handwriting_v1_send_neat_stroke(o->res, h->k, how);
	wl_array_release(&a);
}

static void uint_array(json_object *v, struct wl_array *a)
{
	wl_array_init(a);
	if (!v || !json_object_is_type(v, json_type_array)) return;
	for (size_t i = 0; i < json_object_array_length(v); i++) {
		uint32_t *p = wl_array_add(a, sizeof *p);
		if (p) *p = (uint32_t)json_object_get_int64(json_object_array_get_idx(v, i));
	}
}

static const char *str_of(json_object *m, const char *key)
{
	json_object *v;
	const char *s = json_object_object_get_ex(m, key, &v) ? json_object_get_string(v) : NULL;
	return s ? s : "";
}

static uint32_t origin_of(const char *s)
{
	if (!strcmp(s, "beam")) return GLASSINE_HANDWRITING_V1_ORIGIN_BEAM;
	if (!strcmp(s, "words")) return GLASSINE_HANDWRITING_V1_ORIGIN_WORDS;
	if (!strcmp(s, "guided")) return GLASSINE_HANDWRITING_V1_ORIGIN_GUIDED;
	return GLASSINE_HANDWRITING_V1_ORIGIN_INK;
}

static uint32_t result_of(const char *s)
{
	if (!strcmp(s, "stored")) return GLASSINE_HANDWRITING_V1_RESULT_STORED;
	if (!strcmp(s, "not_allowed")) return GLASSINE_HANDWRITING_V1_RESULT_NOT_ALLOWED;
	if (!strcmp(s, "stale")) return GLASSINE_HANDWRITING_V1_RESULT_STALE;
	return GLASSINE_HANDWRITING_V1_RESULT_NOT_KEPT;
}

/* ---- writing regions: the channel ---- */

static void held_free(struct held *h, size_t n)
{
	for (size_t i = 0; i < n; i++) wl_array_release(&h[i].pts);
	free(h);
}

/* a raw stroke's points as the channel takes them: [x, y, t, pressure, tilt_x, tilt_y] */
static json_object *points_json(const struct wl_array *pts)
{
	json_object *a = json_object_new_array();
	const int32_t *p = pts->data;
	size_t n = pts->size / (6 * sizeof *p);
	for (size_t i = 0; i < n; i++, p += 6) {
		json_object *q = json_object_new_array();
		json_object_array_add(q, json_object_new_double(wl_fixed_to_double(p[0])));
		json_object_array_add(q, json_object_new_double(wl_fixed_to_double(p[1])));
		json_object_array_add(q, json_object_new_int64((uint32_t)p[2]));
		json_object_array_add(q, json_object_new_double(p[3] / 65535.0));
		json_object_array_add(q, json_object_new_double(wl_fixed_to_double(p[4])));
		json_object_array_add(q, json_object_new_double(wl_fixed_to_double(p[5])));
		json_object_array_add(a, q);
	}
	return a;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const unsigned char *d, size_t n)
{
	char *o = malloc((n + 2) / 3 * 4 + 1), *q = o;
	if (!o) return NULL;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
		*q++ = B64[v >> 18 & 63];
		*q++ = B64[v >> 12 & 63];
		*q++ = i + 1 < n ? B64[v >> 6 & 63] : '=';
		*q++ = i + 2 < n ? B64[v & 63] : '=';
	}
	*q = 0;
	return o;
}

/* base64 into a (false: not base64) */
static bool b64_decode(const char *s, struct wl_array *a)
{
	uint32_t v = 0;
	int bits = 0;
	for (; *s && *s != '='; s++) {
		const char *c = strchr(B64, *s);
		if (!c || !*s) return false;
		v = v << 6 | (uint32_t)(c - B64);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			unsigned char *b = wl_array_add(a, 1);
			if (!b) return false;
			*b = (unsigned char)(v >> bits);
		}
	}
	return true;
}

static bool utf8_ok(const unsigned char *s, size_t n)
{
	for (size_t i = 0; i < n;) {
		unsigned char c = s[i];
		size_t k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 9;
		if (!c || k == 9 || (k == 1 && c < 0xc2) || i + k >= n + (k ? 0 : 1)) return false;
		for (size_t j = 1; j <= k; j++) if ((s[i + j] & 0xc0) != 0x80) return false;
		i += k + 1;
	}
	return true;
}

/* bytes as data_part events of kind, in parts of DATA_PART */
static void send_data(struct hw_region *r, uint32_t kind, const void *d, size_t n)
{
	for (size_t i = 0; i < n; i += DATA_PART) {
		struct wl_array a = { .size = n - i < DATA_PART ? n - i : DATA_PART, .alloc = 0, .data = (char *)d + i };
		glassine_handwriting_region_v1_send_data_part(r->res, kind, &a);
	}
}

static json_object *rmsg(const char *op, struct hw_region *r)
{
	json_object *m = msg(op, r->obj->sid);
	json_object_object_add(m, "r", json_object_new_int64(r->rid));
	return m;
}

static bool region_private(const struct hw_region *r)
{
	return r->purpose == GLASSINE_HANDWRITING_V1_PURPOSE_PASSWORD || r->purpose == GLASSINE_HANDWRITING_V1_PURPOSE_PIN ||
	       (r->hints & (0x40 | 0x80));
}

/* the service may hear of the region: reading or neatening on, not private (FR-010) */
static bool region_wants(const struct hw_region *r)
{
	return r->obj && (r->reading || r->neaten) && !region_private(r);
}

static bool region_up(void)
{
	return H.fd >= 0 && H.up && H.regions;
}

/* a service that may read regions is running or on its way: writing in a region that wants
 * it is held until it says hello, and handed to it then */
static bool region_keep(const struct hw_region *r)
{
	return region_wants(r) && H.cmd && *H.cmd && H.fails < 5 && (!H.up || H.regions);
}

static void region_send_set(struct hw_region *r)
{
	json_object *m = rmsg("region_set", r);
	json_object_object_add(m, "reading", json_object_new_boolean(r->reading));
	json_object_object_add(m, "neaten", json_object_new_boolean(r->neaten));
	json_object_object_add(m, "strength", json_object_new_double(r->strength));
	json_object_object_add(m, "purpose", json_object_new_int64(r->purpose));
	json_object_object_add(m, "hints", json_object_new_int64(r->hints));
	json_object_object_add(m, "languages", json_object_new_string(r->langs ? r->langs : ""));
	json_object *g = json_object_new_array();
	json_object_array_add(g, json_object_new_double(r->spacing));
	json_object_array_add(g, json_object_new_double(r->first));
	json_object_object_add(m, "guides", g);
	json_object *z = json_object_new_array();
	json_object_array_add(z, json_object_new_double(r->w));
	json_object_array_add(z, json_object_new_double(r->h));
	json_object_object_add(m, "size", z);
	char u[64];
	units(u, sizeof u);
	json_object_object_add(m, "units", json_object_new_string(u));
	if (r->display == GLASSINE_HANDWRITING_REGION_V1_DISPLAY_SET)
		json_object_object_add(m, "display", json_object_new_string("set"));
	svc_send(m);
	r->dirty = false;
}

static void region_forget(struct hw_region *r)
{
	held_free(r->held, r->nheld);
	r->held = NULL;
	r->nheld = 0;
	free(r->value); free(r->state); free(r->edit);
	r->value = r->state = r->edit = NULL;
}

/* the region's announcement and choices, as they now stand: the service hears of a region
 * that holds writing and wants it, and forgets one that no longer wants it */
static void region_sync(struct hw_region *r)
{
	if (r->announced && (!region_wants(r) || !region_up())) {
		if (region_up() && r->obj) svc_send(rmsg("region_destroy", r));
		r->announced = false;
		region_forget(r);       /* never sent again: private or off from here */
		return;
	}
	if (!r->announced) {
		if (!region_wants(r)) region_forget(r);         /* private or off: nothing held is ever sent */
		if (!r->holds || !region_wants(r) || !region_up()) return;
		if (!r->obj->announced) send_surface(r->obj);
		json_object *m = rmsg("region", r);
		json_object_object_add(m, "name", json_object_new_string(r->name ? r->name : ""));
		svc_send(m);
		r->announced = true;
		region_send_set(r);
		if (r->mode != GLASSINE_HANDWRITING_V1_MODE_INK) {
			json_object *q = rmsg("region_mode", r);
			json_object_object_add(q, "mode", json_object_new_string("text"));
			svc_send(q);
		}
		return;
	}
	if (r->dirty) region_send_set(r);
}

static json_object *restore_msg(struct hw_region *r, const struct held *h, size_t n, const char *value, const char *state)
{
	json_object *m = rmsg("region_restore", r), *a = json_object_new_array();
	for (size_t i = 0; i < n; i++) {
		json_object *e = json_object_new_object();
		json_object_object_add(e, "k", json_object_new_int64(h[i].k));
		json_object_object_add(e, "points", points_json(&h[i].pts));
		json_object_array_add(a, e);
	}
	json_object_object_add(m, "strokes", a);
	if (value) json_object_object_add(m, "value", json_object_new_string(value));
	if (state) json_object_object_add(m, "state", json_object_new_string(state));
	return m;
}

/* after a new hello: the region's writing back to the restarted service */
static void region_replay(struct hw_region *r)
{
	if (!H.regions) { region_forget(r); return; }   /* this service doesn't read regions */
	if (r->announced || !(r->nheld || r->value || r->state || r->edit)) return;
	region_sync(r);
	if (!r->announced) return;
	if (r->nheld || r->value || r->state) svc_send(restore_msg(r, r->held, r->nheld, r->value, r->state));
	if (r->edit) {
		json_object *m = rmsg("region_edit", r);
		json_object_object_add(m, "text", json_object_new_string(r->edit));
		svc_send(m);
		free(r->edit);
		r->edit = NULL;
	}
}

static struct hw_region *region_of(json_object *m)
{
	json_object *v;
	if (!json_object_object_get_ex(m, "r", &v)) return NULL;
	uint32_t rid = (uint32_t)json_object_get_int64(v);
	struct hw_obj *o;
	struct hw_region *r;
	wl_list_for_each(o, &H.objs, link) wl_list_for_each(r, &o->regions, link) if (r->rid == rid) return r->announced ? r : NULL;
	return NULL;
}

static struct held *region_held(struct hw_region *r, uint32_t k)
{
	for (size_t i = 0; i < r->nheld; i++) if (r->held[i].k == k) return &r->held[i];
	return NULL;
}

/* the stroke numbers of key in m as strokes data; also, with drop, out of what is held */
static void send_strokes(struct hw_region *r, json_object *m, const char *key, bool drop)
{
	json_object *v;
	struct wl_array a;
	uint_array(json_object_object_get_ex(m, key, &v) ? v : NULL, &a);
	send_data(r, GLASSINE_HANDWRITING_REGION_V1_DATA_KIND_STROKES, a.data, a.size);
	if (drop) {
		const uint32_t *k = a.data;
		for (size_t i = 0; i < a.size / sizeof *k; i++) {
			struct held *h = region_held(r, k[i]);
			if (!h) continue;
			wl_array_release(&h->pts);
			size_t at = (size_t)(h - r->held);
			memmove(h, h + 1, (r->nheld - at - 1) * sizeof *h);
			r->nheld--;
		}
	}
	wl_array_release(&a);
}

static void send_text(struct hw_region *r, const char *t)
{
	send_data(r, GLASSINE_HANDWRITING_REGION_V1_DATA_KIND_TEXT, t, strlen(t));
}

static int32_t fx(json_object *v)
{
	return wl_fixed_from_double(json_object_get_double(v));
}

/* a set word's path ("M", x, y, "L", …) as shape data; false: not a path */
static bool send_shape(struct hw_region *r, json_object *path)
{
	if (!path || !json_object_is_type(path, json_type_array)) return false;
	struct wl_array a;
	wl_array_init(&a);
	size_t n = json_object_array_length(path);
	for (size_t i = 0; i < n;) {
		const char *op = json_object_get_string(json_object_array_get_idx(path, i++));
		static const char ops[] = "MLQCZ";
		static const int ncoord[] = { 2, 2, 4, 6, 0 };
		const char *c = op && op[0] && !op[1] ? strchr(ops, op[0]) : NULL;
		if (!c || i + (size_t)ncoord[c - ops] > n) { wl_array_release(&a); return false; }
		int32_t *q = wl_array_add(&a, (size_t)(1 + ncoord[c - ops]) * sizeof *q);
		if (!q) { wl_array_release(&a); return false; }
		*q++ = (int32_t)(c - ops);
		for (int j = 0; j < ncoord[c - ops]; j++) *q++ = fx(json_object_array_get_idx(path, i++));
	}
	send_data(r, GLASSINE_HANDWRITING_REGION_V1_DATA_KIND_SHAPE, a.data, a.size);
	wl_array_release(&a);
	return true;
}

static void got_region_value(struct hw_region *r, json_object *m)
{
	json_object *v, *words;
	uint32_t serial = json_object_object_get_ex(m, "serial", &v) ? (uint32_t)json_object_get_int64(v) : 0;
	const char *text = str_of(m, "text");
	size_t tlen = strlen(text);
	if (!utf8_ok((const unsigned char *)text, tlen)) return;
	struct wl_array state;
	wl_array_init(&state);
	const char *st = json_object_object_get_ex(m, "state", &v) ? json_object_get_string(v) : NULL;
	if (st && !b64_decode(st, &state)) { wl_array_release(&state); return; }
	if (json_object_object_get_ex(m, "words", &words) && json_object_is_type(words, json_type_array)) {
		for (size_t i = 0; i < json_object_array_length(words); i++) {
			json_object *w = json_object_array_get_idx(words, i), *a, *b;
			uint32_t start = json_object_object_get_ex(w, "start", &a) ? (uint32_t)json_object_get_int64(a) : 0;
			uint32_t len = json_object_object_get_ex(w, "length", &a) ? (uint32_t)json_object_get_int64(a) : 0;
			if ((size_t)start + len > tlen) continue;
			/* a word with a path: set (no strokes), or an ink word shown set (display set,
			 * version 3; an older client gets it as its ink) */
			json_object *path = NULL;
			bool ink = json_object_object_get_ex(w, "strokes", &a);
			bool has_path = json_object_object_get_ex(w, "path", &path);
			bool shown = ink && has_path && wl_resource_get_version(r->res) >= 3;
			/* a path that doesn't parse: a set word is dropped, a shown one comes as its ink */
			if (has_path && (!ink || shown) && send_shape(r, path)) {
				int32_t box[4] = { 0 };
				if (json_object_object_get_ex(w, "box", &b) && json_object_is_type(b, json_type_array) &&
				    json_object_array_length(b) == 4)
					for (int j = 0; j < 4; j++) box[j] = fx(json_object_array_get_idx(b, j));
				if (shown) {
					send_strokes(r, w, "strokes", false);
					glassine_handwriting_region_v1_send_word_shown_set(r->res, serial, start, len, box[0], box[1],
											   box[2] - box[0], box[3] - box[1]);
				} else
					glassine_handwriting_region_v1_send_set_word(r->res, serial, start, len, box[0], box[1],
										     box[2] - box[0], box[3] - box[1]);
			} else if (ink || !has_path) {
				send_strokes(r, w, "strokes", false);
				int32_t dx = json_object_object_get_ex(w, "dx", &a) ? fx(a) : 0;
				int32_t dy = json_object_object_get_ex(w, "dy", &a) ? fx(a) : 0;
				glassine_handwriting_region_v1_send_word(r->res, serial, start, len, dx, dy);
			}
		}
	}
	if (json_object_object_get_ex(m, "gone", &v)) {
		send_strokes(r, m, "gone", true);
		glassine_handwriting_region_v1_send_gone(r->res, serial);
	}
	send_text(r, text);
	send_data(r, GLASSINE_HANDWRITING_REGION_V1_DATA_KIND_STATE, state.data, state.size);
	glassine_handwriting_region_v1_send_value(r->res, serial);
	wl_array_release(&state);
	free(r->value); free(r->state);
	r->value = strdup(text);
	r->state = st ? strdup(st) : NULL;
}

static void got_region(const char *op, json_object *m)
{
	struct hw_region *r = region_of(m);
	if (!r || !r->obj) return;
	json_object *v;
	uint32_t serial = json_object_object_get_ex(m, "serial", &v) ? (uint32_t)json_object_get_int64(v) : 0;
	if (!strcmp(op, "neat")) {
		if (!r->neaten || !json_object_object_get_ex(m, "k", &v)) return;
		struct held *h = region_held(r, (uint32_t)json_object_get_int64(v));
		json_object *pts;
		if (!h || !json_object_object_get_ex(m, "points", &pts) || !json_object_is_type(pts, json_type_array)) return;
		size_t n = h->pts.size / (6 * sizeof(int32_t));
		if (json_object_array_length(pts) != n) return;
		struct wl_array a;
		wl_array_init(&a);
		int32_t *q = wl_array_add(&a, h->pts.size);
		if (!q) { wl_array_release(&a); return; }
		memcpy(q, h->pts.data, h->pts.size);
		for (size_t i = 0; i < n; i++) {
			json_object *p = json_object_array_get_idx(pts, i);
			if (!json_object_is_type(p, json_type_array) || json_object_array_length(p) < 2) { wl_array_release(&a); return; }
			q[6 * i] = fx(json_object_array_get_idx(p, 0));
			q[6 * i + 1] = fx(json_object_array_get_idx(p, 1));
		}
		const char *how = str_of(m, "how");
		const size_t point = 6 * sizeof(int32_t);
		for (size_t i = 0; i < n; i += PART) {
			struct wl_array part = { .size = (n - i < PART ? n - i : PART) * point, .alloc = 0,
						 .data = (char *)a.data + i * point };
			glassine_handwriting_region_v1_send_points(r->res, h->k, GLASSINE_HANDWRITING_V1_KIND_NEAT, &part);
		}
		glassine_handwriting_region_v1_send_neat_stroke(r->res, h->k,
			!strcmp(how, "line") ? GLASSINE_HANDWRITING_V1_HOW_LINE :
			!strcmp(how, "pulled") ? GLASSINE_HANDWRITING_V1_HOW_PULLED : GLASSINE_HANDWRITING_V1_HOW_KEPT);
		wl_array_release(&a);
	} else if (!strcmp(op, "value")) {
		got_region_value(r, m);
	} else if (!strcmp(op, "candidates")) {
		json_object *items;
		uint32_t n = 0;
		if (json_object_object_get_ex(m, "items", &items) && json_object_is_type(items, json_type_array)) {
			for (size_t i = 0; i < json_object_array_length(items); i++) {
				json_object *c = json_object_array_get_idx(items, i), *fit;
				const char *t = str_of(c, "text");
				if (!utf8_ok((const unsigned char *)t, strlen(t))) continue;
				bool ok = json_object_object_get_ex(c, "fit", &fit) && json_object_get_boolean(fit);
				send_text(r, t);
				glassine_handwriting_region_v1_send_candidate(r->res, serial, n++, origin_of(str_of(c, "from")), ok);
			}
		}
		glassine_handwriting_region_v1_send_candidates_done(r->res, serial, n);
	} else if (!strcmp(op, "gesture")) {
		const char *k = str_of(m, "kind");
		uint32_t kind = !strcmp(k, "strike") ? GLASSINE_HANDWRITING_REGION_V1_GESTURE_KIND_STRIKE :
				!strcmp(k, "scratch") ? GLASSINE_HANDWRITING_REGION_V1_GESTURE_KIND_SCRATCH :
				!strcmp(k, "caret") ? GLASSINE_HANDWRITING_REGION_V1_GESTURE_KIND_CARET : 99;
		if (kind == 99) return;
		send_strokes(r, m, "strokes", false);
		glassine_handwriting_region_v1_send_gesture(r->res, serial, kind);
	} else if (!strcmp(op, "report")) {
		glassine_handwriting_region_v1_send_report_done(r->res, serial, result_of(str_of(m, "result")));
	}
}

static void got(json_object *m)
{
	const char *op = str_of(m, "op");
	if (!strcmp(op, "hello")) { got_hello(m); return; }
	if (!strncmp(op, "region_", 7)) { got_region(op + 7, m); return; }
	json_object *v;
	struct hw_obj *o = json_object_object_get_ex(m, "s", &v) ? obj_of((uint32_t)json_object_get_int64(v)) : NULL;
	if (!o) return;
	if (!strcmp(op, "neat")) {
		if (o->neaten) got_neat(o, m);
	} else if (!strcmp(op, "reading")) {
		if (!o->text) return;
		struct wl_array a;
		char buf[MAX_EVENT_STR + 1];
		uint_array(json_object_object_get_ex(m, "strokes", &v) ? v : NULL, &a);
		if (a.size > MAX_READING * sizeof(uint32_t)) {        /* the last ones */
			memmove(a.data, (char *)a.data + a.size - MAX_READING * sizeof(uint32_t), MAX_READING * sizeof(uint32_t));
			a.size = MAX_READING * sizeof(uint32_t);
		}
		glassine_handwriting_v1_send_reading(o->res, cut(str_of(m, "text"), buf), &a);
		wl_array_release(&a);
	} else if (!strcmp(op, "candidates")) {
		if (!o->text) return;
		uint32_t serial = json_object_object_get_ex(m, "serial", &v) ? (uint32_t)json_object_get_int64(v) : 0;
		json_object *items;
		uint32_t n = 0;
		if (json_object_object_get_ex(m, "items", &items) && json_object_is_type(items, json_type_array)) {
			for (size_t i = 0; i < json_object_array_length(items); i++) {
				json_object *c = json_object_array_get_idx(items, i), *fit;
				bool ok = json_object_object_get_ex(c, "fit", &fit) && json_object_get_boolean(fit);
				char buf[MAX_EVENT_STR + 1];
				const char *chars = str_of(c, "chars");
				if (strlen(chars) > MAX_EVENT_STR) chars = "";
				glassine_handwriting_v1_send_candidate(o->res, serial, n++, cut(str_of(c, "text"), buf),
								       origin_of(str_of(c, "from")), ok, chars);
			}
		}
		glassine_handwriting_v1_send_candidates_done(o->res, serial, n);
	} else if (!strcmp(op, "report")) {
		uint32_t serial = json_object_object_get_ex(m, "serial", &v) ? (uint32_t)json_object_get_int64(v) : 0;
		glassine_handwriting_v1_send_report_done(o->res, serial, result_of(str_of(m, "result")));
	}
}

static int svc_event(int fd, uint32_t mask, void *data)
{
	if (mask & WL_EVENT_WRITABLE) svc_flush();
	if (H.fd < 0) return 0;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR) && !(mask & WL_EVENT_READABLE)) { svc_close(); return 0; }
	if (!(mask & WL_EVENT_READABLE)) return 0;
	for (;;) {
		if (H.cin - H.nin < 65536) {
			size_t c = H.cin ? H.cin * 2 : 131072;
			if (c > (64u << 20)) { svc_close(); return 0; }
			char *b = realloc(H.in, c);
			if (!b) { svc_close(); return 0; }
			H.in = b; H.cin = c;
		}
		ssize_t r = recv(fd, H.in + H.nin, H.cin - H.nin, MSG_DONTWAIT);
		if (r == 0) { svc_close(); return 0; }
		if (r < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) break;
			svc_close();
			return 0;
		}
		H.nin += (size_t)r;
	}
	char *s = H.in, *e;
	while ((e = memchr(s, '\n', H.nin - (size_t)(s - H.in)))) {
		*e = 0;
		json_object *m = json_tokener_parse(s);
		if (m && json_object_is_type(m, json_type_object)) got(m);
		if (m) json_object_put(m);
		if (H.fd < 0) return 0;     /* closed while handling */
		s = e + 1;
	}
	H.nin -= (size_t)(s - H.in);
	memmove(H.in, s, H.nin);
	return 0;
}

static void svc_start(void);

static int respawn(void *data)
{
	svc_start();
	return 0;
}

static void svc_close(void)
{
	if (H.src) { wl_event_source_remove(H.src); H.src = NULL; }
	if (H.fd >= 0) close(H.fd);
	H.fd = -1;
	H.nin = H.nout = 0;
	bool was = H.up || H.caps;
	H.up = false;
	H.caps = 0;
	H.regions = false;
	struct hw_obj *o;
	wl_list_for_each(o, &H.objs, link) {
		o->announced = false;
		struct hw_region *r;
		wl_list_for_each(r, &o->regions, link) r->announced = false;
	}
	if (was) caps_changed();
	if (!was && ++H.fails >= 5) {           /* it can't start: raw strokes only, until the next login */
		wl_list_for_each(o, &H.objs, link) {
			struct hw_region *r;
			wl_list_for_each(r, &o->regions, link) region_forget(r);
		}
		return;
	}
	if (!H.timer) H.timer = wl_event_loop_add_timer(H.loop, respawn, NULL);
	if (H.timer) wl_event_source_timer_update(H.timer, RESPAWN_MS);
}

/* the service, started with one end of a socket pair as fd 3; a double fork, so no zombie
 * is left for the compositor to reap */
static void svc_start(void)
{
	if (H.fd >= 0 || !H.cmd || !*H.cmd) return;
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) return;
	pid_t p = fork();
	if (p < 0) { close(sv[0]); close(sv[1]); return; }
	if (p == 0) {
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		if (fork() != 0) _exit(0);
		if (dup2(sv[1], 3) < 0) _exit(127);  /* dup2 clears CLOEXEC on 3 */
		if (sv[1] == 3) fcntl(3, F_SETFD, 0);
		execlp(H.cmd, H.cmd, "--pen-fd", "3", (char *)NULL);
		_exit(127);
	}
	close(sv[1]);
	waitpid(p, NULL, 0);
	H.fd = sv[0];
	H.src = wl_event_loop_add_fd(H.loop, H.fd, WL_EVENT_READABLE, svc_event, NULL);
}

/* ---- glassine_handwriting_v1 ---- */

static void announce_idle(void *data)
{
	struct hw_obj *o = data;
	o->idle = NULL;                 /* an idle source goes away by itself once it ran */
	announce(o);
}

/* the choices changed: told to the service after the client's requests in hand are all
 * in, so a field turned on and marked private in one go never reaches it */
static void changed(struct hw_obj *o)
{
	if (!o->idle) o->idle = wl_event_loop_add_idle(H.loop, announce_idle, o);
	if (!o->idle) announce(o);
}

static struct hw_obj *obj_from(struct wl_resource *r)
{
	return wl_resource_get_user_data(r);
}

static void region_detach(struct hw_region *r);

static void obj_free(struct hw_obj *o)
{
	while (!wl_list_empty(&o->regions)) {
		struct hw_region *r = wl_container_of(o->regions.next, r, link);
		region_detach(r);       /* before the surface's destroy below */
	}
	if (H.cur == o) { H.cur = NULL; H.cur_r = NULL; wl_array_release(&H.pts); wl_array_init(&H.pts); }
	if (o->idle) wl_event_source_remove(o->idle);
	if (o->announced) svc_send(msg("destroy", o->sid));
	wl_list_remove(&o->link);
	if (o->surface) wl_list_remove(&o->surface_destroy.link);
	if (o->area && H.hooks.region_free) H.hooks.region_free(o->area);
	held_clear(o);
	free(o->before); free(o->after); free(o->langs);
	free(o);
}

static void obj_resource_destroy(struct wl_resource *r)
{
	struct hw_obj *o = obj_from(r);
	if (o) obj_free(o);
}

static void obj_destroy(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void surface_gone(struct wl_listener *l, void *data)
{
	struct hw_obj *o = wl_container_of(l, o, surface_destroy);
	wl_list_remove(&o->surface_destroy.link);
	wl_list_init(&o->surface_destroy.link);
	o->surface = NULL;
	if (H.cur == o) { H.cur = NULL; H.cur_r = NULL; wl_array_release(&H.pts); wl_array_init(&H.pts); }
	if (o->region_mode) areas_changed();
}

static void set_area(struct wl_client *c, struct wl_resource *r, struct wl_resource *region)
{
	struct hw_obj *o = obj_from(r);
	if (o->area && H.hooks.region_free) H.hooks.region_free(o->area);
	o->area = region && H.hooks.region_copy ? H.hooks.region_copy(region) : NULL;
	changed(o);
}

static void set_guides(struct wl_client *c, struct wl_resource *r, wl_fixed_t spacing, wl_fixed_t first)
{
	struct hw_obj *o = obj_from(r);
	double s = wl_fixed_to_double(spacing);
	o->spacing = s > 0 ? s : 0;
	o->first = wl_fixed_to_double(first);
	changed(o);
}

static void set_neaten(struct wl_client *c, struct wl_resource *r, uint32_t on, wl_fixed_t strength)
{
	struct hw_obj *o = obj_from(r);
	double s = wl_fixed_to_double(strength);
	o->neaten = on != 0;
	o->strength = s < 0 ? 0 : s > 1 ? 1 : s;
	changed(o);
}

static void set_text(struct wl_client *c, struct wl_resource *r, uint32_t on)
{
	struct hw_obj *o = obj_from(r);
	o->text = on != 0;
	changed(o);
}

static char *dup_tail(const char *s, bool tail)
{
	size_t n = strlen(s);
	if (n <= MAX_TEXT) return strdup(s);
	/* keep the end nearest the cursor, on a character's start */
	const char *p = tail ? s + n - MAX_TEXT : s;
	size_t len = MAX_TEXT;
	if (tail) while (*p && ((unsigned char)*p & 0xc0) == 0x80) { p++; len--; }
	else while (len && ((unsigned char)p[len] & 0xc0) == 0x80) len--;
	return strndup(p, len);
}

static void set_context(struct wl_client *c, struct wl_resource *r, uint32_t purpose, uint32_t hints,
			const char *before, const char *after, const char *langs)
{
	struct hw_obj *o = obj_from(r);
	o->purpose = purpose;
	o->hints = hints;
	free(o->before); free(o->after); free(o->langs);
	o->before = dup_tail(before, true);
	o->after = dup_tail(after, false);
	o->langs = strndup(langs, 256);
	changed(o);
}

static void obj_clear(struct wl_client *c, struct wl_resource *r)
{
	struct hw_obj *o = obj_from(r);
	held_clear(o);
	if (o->announced && !o->region_mode) svc_send(msg("clear", o->sid));
}

static void report(struct wl_client *c, struct wl_resource *r, uint32_t serial, uint32_t action, uint32_t rank,
		   const char *text)
{
	struct hw_obj *o = obj_from(r);
	static const char *names[] = { "picked", "accepted", "corrected", "discarded" };
	if (!o->announced || !o->text || action > 3 || o->region_mode) {
		glassine_handwriting_v1_send_report_done(r, serial, H.fd < 0 || !H.up ?
			GLASSINE_HANDWRITING_V1_RESULT_UNAVAILABLE : GLASSINE_HANDWRITING_V1_RESULT_STALE);
		return;
	}
	json_object *m = msg("report", o->sid);
	json_object_object_add(m, "serial", json_object_new_int64(serial));
	json_object_object_add(m, "action", json_object_new_string(names[action]));
	json_object_object_add(m, "rank", json_object_new_int64(rank));
	json_object_object_add(m, "text", json_object_new_string_len(text, (int)strnlen(text, MAX_TEXT)));
	svc_send(m);
	held_clear(o);                          /* the service forgets the writing too */
}

/* ---- glassine_handwriting_region_v1 ---- */

static void region_idle(void *data)
{
	struct hw_region *r = data;
	r->idle = NULL;
	region_sync(r);
}

/* as changed(): told once the client's requests in hand are all in */
static void region_changed(struct hw_region *r, bool set)
{
	if (set) r->dirty = true;
	if (!r->idle) r->idle = wl_event_loop_add_idle(H.loop, region_idle, r);
	if (!r->idle) region_sync(r);
}

/* the choices in hand go out now (before a stroke, an edit or a restore that follows them) */
static void region_now(struct hw_region *r)
{
	if (r->idle) { wl_event_source_remove(r->idle); r->idle = NULL; }
	region_sync(r);
}

static struct hw_region *region_from(struct wl_resource *res)
{
	return wl_resource_get_user_data(res);
}

static void region_drop_stroke(struct hw_region *r)
{
	if (H.cur_r != r) return;
	H.cur = NULL;
	H.cur_r = NULL;
	wl_array_release(&H.pts);
	wl_array_init(&H.pts);
}

/* the region leaves its object (the object or the region is destroyed): inert from here */
static void region_detach(struct hw_region *r)
{
	region_drop_stroke(r);
	if (r->idle) { wl_event_source_remove(r->idle); r->idle = NULL; }
	if (r->announced && r->obj && region_up()) svc_send(rmsg("region_destroy", r));
	r->announced = false;
	region_forget(r);
	if (r->obj) {
		wl_list_remove(&r->link);
		wl_list_init(&r->link);
		r->obj = NULL;
		areas_changed();
	}
}

static void region_free(struct hw_region *r)
{
	region_detach(r);
	wl_list_remove(&r->link);
	if (r->clip && H.hooks.region_free) H.hooks.region_free(r->clip);
	free(r->name); free(r->langs);
	wl_array_release(&r->in_text);
	wl_array_release(&r->in_state);
	wl_array_release(&r->rp);
	held_free(r->rs, r->nrs);
	free(r);
}

static void region_resource_destroy(struct wl_resource *res)
{
	struct hw_region *r = region_from(res);
	if (r) region_free(r);
}

static void r_destroy(struct wl_client *c, struct wl_resource *res)
{
	wl_resource_destroy(res);
}

static void r_set_rect(struct wl_client *c, struct wl_resource *res, wl_fixed_t x, wl_fixed_t y, wl_fixed_t w,
		       wl_fixed_t h)
{
	struct hw_region *r = region_from(res);
	double nw = wl_fixed_to_double(w), nh = wl_fixed_to_double(h);
	r->x = wl_fixed_to_double(x);
	r->y = wl_fixed_to_double(y);
	if (nw < 0) nw = 0;
	if (nh < 0) nh = 0;
	bool resized = nw != r->w || nh != r->h;
	r->w = nw;
	r->h = nh;
	areas_changed();
	if (resized) region_changed(r, true);         /* a move alone: nothing for the service */
}

static void r_set_clip(struct wl_client *c, struct wl_resource *res, struct wl_resource *region)
{
	struct hw_region *r = region_from(res);
	if (r->clip && H.hooks.region_free) H.hooks.region_free(r->clip);
	r->clip = region && H.hooks.region_copy ? H.hooks.region_copy(region) : NULL;
	areas_changed();
}

static void r_set_guides(struct wl_client *c, struct wl_resource *res, wl_fixed_t spacing, wl_fixed_t first)
{
	struct hw_region *r = region_from(res);
	double s = wl_fixed_to_double(spacing);
	r->spacing = s > 0 ? s : 0;
	r->first = wl_fixed_to_double(first);
	region_changed(r, true);
}

static void r_set_context(struct wl_client *c, struct wl_resource *res, uint32_t purpose, uint32_t hints,
			  const char *langs)
{
	struct hw_region *r = region_from(res);
	r->purpose = purpose;
	r->hints = hints;
	free(r->langs);
	r->langs = strndup(langs, 256);
	region_changed(r, true);
}

static void r_set_reading(struct wl_client *c, struct wl_resource *res, uint32_t on)
{
	struct hw_region *r = region_from(res);
	r->reading = on != 0;
	region_changed(r, true);
}

static void r_set_neaten(struct wl_client *c, struct wl_resource *res, uint32_t on, wl_fixed_t strength)
{
	struct hw_region *r = region_from(res);
	double s = wl_fixed_to_double(strength);
	r->neaten = on != 0;
	r->strength = s < 0 ? 0 : s > 1 ? 1 : s;
	region_changed(r, true);
}

static void r_set_layer(struct wl_client *c, struct wl_resource *res, int32_t layer)
{
	region_from(res)->layer = layer;
	areas_changed();
}

static void region_set_mode(struct hw_region *r, uint32_t mode)
{
	mode = mode == GLASSINE_HANDWRITING_V1_MODE_TEXT ? mode : GLASSINE_HANDWRITING_V1_MODE_INK;
	if (mode == r->mode) return;
	r->mode = mode;
	areas_changed();
	region_now(r);
	if (!r->announced) return;
	json_object *m = rmsg("region_mode", r);
	json_object_object_add(m, "mode", json_object_new_string(mode == GLASSINE_HANDWRITING_V1_MODE_TEXT ? "text" : "ink"));
	svc_send(m);
}

static void r_set_mode(struct wl_client *c, struct wl_resource *res, uint32_t mode)
{
	region_set_mode(region_from(res), mode);
}

/* version 3: the words with ink shown as ink or set in the person's font; a choice for the
 * service (region_set), nothing changes here */
static void r_set_display(struct wl_client *c, struct wl_resource *res, uint32_t display)
{
	struct hw_region *r = region_from(res);
	display = display == GLASSINE_HANDWRITING_REGION_V1_DISPLAY_SET ? display : GLASSINE_HANDWRITING_REGION_V1_DISPLAY_INK;
	if (display == r->display) return;
	r->display = display;
	region_changed(r, true);
}

static void r_add_data(struct wl_client *c, struct wl_resource *res, uint32_t kind, struct wl_array *a)
{
	struct hw_region *r = region_from(res);
	struct wl_array *to = kind == GLASSINE_HANDWRITING_REGION_V1_DATA_KIND_TEXT ? &r->in_text :
			      kind == GLASSINE_HANDWRITING_REGION_V1_DATA_KIND_STATE ? &r->in_state : NULL;
	size_t max = kind == GLASSINE_HANDWRITING_REGION_V1_DATA_KIND_TEXT ? MAX_DATA_TEXT : MAX_DATA_STATE;
	if (!to || to->size + a->size > max) return;            /* past the limit: the rest is dropped */
	void *p = wl_array_add(to, a->size);
	if (p) memcpy(p, a->data, a->size);
}

/* the text gathered, as a string (NULL: not UTF-8), and the gathering emptied */
static char *take_text(struct hw_region *r)
{
	char *t = NULL;
	if (utf8_ok(r->in_text.data, r->in_text.size)) t = strndup(r->in_text.data ? r->in_text.data : "", r->in_text.size);
	wl_array_release(&r->in_text);
	wl_array_init(&r->in_text);
	return t;
}

static void r_edit(struct wl_client *c, struct wl_resource *res)
{
	struct hw_region *r = region_from(res);
	char *t = take_text(r);
	if (!t || !r->obj) { free(t); return; }
	r->holds = true;
	region_now(r);
	if (r->announced) {
		json_object *m = rmsg("region_edit", r);
		json_object_object_add(m, "text", json_object_new_string(t));
		svc_send(m);
	} else if (region_keep(r)) {
		free(r->edit);          /* the latest edit is the value */
		r->edit = t;
		return;
	}
	free(t);
}

static void r_report(struct wl_client *c, struct wl_resource *res, uint32_t serial, uint32_t action, uint32_t rank)
{
	struct hw_region *r = region_from(res);
	static const char *names[] = { "picked", "accepted", "corrected", "discarded" };
	char *t = take_text(r);
	region_now(r);
	uint32_t result = UINT32_MAX;
	if (!r->obj || region_private(r) || action > 2) result = GLASSINE_HANDWRITING_V1_RESULT_NOT_KEPT;
	else if (!region_up()) result = GLASSINE_HANDWRITING_V1_RESULT_UNAVAILABLE;
	else if (!r->reading) result = GLASSINE_HANDWRITING_V1_RESULT_NOT_KEPT;
	else if (!r->announced) result = GLASSINE_HANDWRITING_V1_RESULT_STALE;     /* no writing it was read from */
	if (result != UINT32_MAX) {
		glassine_handwriting_region_v1_send_report_done(res, serial, result);
		free(t);
		return;
	}
	json_object *m = rmsg("region_report", r);
	json_object_object_add(m, "serial", json_object_new_int64(serial));
	json_object_object_add(m, "action", json_object_new_string(names[action]));
	json_object_object_add(m, "rank", json_object_new_int64(rank));
	json_object_object_add(m, "text", json_object_new_string(t ? t : ""));
	svc_send(m);
	free(t);
}

static void r_restore_points(struct wl_client *c, struct wl_resource *res, struct wl_array *a)
{
	struct hw_region *r = region_from(res);
	const size_t point = 6 * sizeof(int32_t);
	if (a->size % point || r->rp.size + a->size > MAX_POINTS * point) return;
	void *p = wl_array_add(&r->rp, a->size);
	if (p) memcpy(p, a->data, a->size);
}

static void r_restore_stroke(struct wl_client *c, struct wl_resource *res, uint32_t k)
{
	struct hw_region *r = region_from(res);
	struct held *h = NULL;
	for (size_t i = 0; i < r->nrs; i++) if (r->rs[i].k == k) { h = &r->rs[i]; wl_array_release(&h->pts); }
	if (!h) {
		h = realloc(r->rs, (r->nrs + 1) * sizeof *h);
		if (!h) { wl_array_release(&r->rp); wl_array_init(&r->rp); return; }
		r->rs = h;
		h = &r->rs[r->nrs++];
	}
	h->k = k;
	h->pts = r->rp;                 /* the points gathered move into it */
	wl_array_init(&r->rp);
	if (k >= r->next) r->next = k == UINT32_MAX ? k : k + 1;
}

static void r_restore(struct wl_client *c, struct wl_resource *res)
{
	struct hw_region *r = region_from(res);
	char *value = r->in_text.size ? take_text(r) : NULL;
	wl_array_release(&r->in_text);
	wl_array_init(&r->in_text);
	char *state = r->in_state.size ? b64_encode(r->in_state.data, r->in_state.size) : NULL;
	wl_array_release(&r->in_state);
	wl_array_init(&r->in_state);
	wl_array_release(&r->rp);
	wl_array_init(&r->rp);
	struct held *rs = r->rs;
	size_t nrs = r->nrs;
	r->rs = NULL;
	r->nrs = 0;
	region_drop_stroke(r);
	if (!r->obj) { held_free(rs, nrs); free(value); free(state); return; }
	r->holds = true;
	region_now(r);
	if (!r->announced && !region_keep(r)) {        /* no service, or private: kept nowhere */
		held_free(rs, nrs); free(value); free(state);
		return;
	}
	if (r->announced) svc_send(restore_msg(r, rs, nrs, value, state));
	region_forget(r);
	r->held = rs;
	r->nheld = nrs;
	r->value = value;
	r->state = state;
}

static const struct glassine_handwriting_region_v1_interface region_impl = {
	.destroy = r_destroy,
	.set_rect = r_set_rect,
	.set_clip = r_set_clip,
	.set_guides = r_set_guides,
	.set_context = r_set_context,
	.set_reading = r_set_reading,
	.set_neaten = r_set_neaten,
	.set_layer = r_set_layer,
	.set_mode = r_set_mode,
	.set_display = r_set_display,
	.add_data = r_add_data,
	.edit = r_edit,
	.report = r_report,
	.restore_points = r_restore_points,
	.restore_stroke = r_restore_stroke,
	.restore = r_restore,
};

static void get_region(struct wl_client *c, struct wl_resource *res, uint32_t id, const char *name)
{
	struct hw_obj *o = obj_from(res);
	struct hw_region *r;
	wl_list_for_each(r, &o->regions, link) {
		if (!strcmp(r->name, name)) {
			wl_resource_post_error(res, GLASSINE_HANDWRITING_V1_ERROR_REGION_NAME,
					       "a live region of this object already has that name");
			return;
		}
	}
	r = calloc(1, sizeof *r);
	if (!r || !(r->name = strndup(name, MAX_NAME))) { free(r); wl_client_post_no_memory(c); return; }
	r->res = wl_resource_create(c, &glassine_handwriting_region_v1_interface, wl_resource_get_version(res), id);
	if (!r->res) { free(r->name); free(r); wl_client_post_no_memory(c); return; }
	if (!o->region_mode) {
		/* region mode for good: what the object held as one surface is forgotten */
		if (o->idle) { wl_event_source_remove(o->idle); o->idle = NULL; }
		if (o->announced) svc_send(msg("destroy", o->sid));
		o->announced = false;
		held_clear(o);
		if (H.cur == o && !H.cur_r) { H.cur = NULL; wl_array_release(&H.pts); wl_array_init(&H.pts); }
		o->region_mode = true;
	}
	r->obj = o;
	r->rid = ++H.next_rid;
	r->seq = ++o->region_seq;
	r->mode = o->mode_all;
	r->reading = true;
	wl_array_init(&r->in_text);
	wl_array_init(&r->in_state);
	wl_array_init(&r->rp);
	wl_list_insert(o->regions.prev, &r->link);
	wl_resource_set_implementation(r->res, &region_impl, r, region_resource_destroy);
	areas_changed();
}

static void obj_set_mode(struct wl_client *c, struct wl_resource *res, uint32_t mode)
{
	struct hw_obj *o = obj_from(res);
	o->mode_all = mode == GLASSINE_HANDWRITING_V1_MODE_TEXT ? mode : GLASSINE_HANDWRITING_V1_MODE_INK;
	struct hw_region *r;
	wl_list_for_each(r, &o->regions, link) region_set_mode(r, mode);
}

static const struct glassine_handwriting_v1_interface obj_impl = {
	.destroy = obj_destroy,
	.set_area = set_area,
	.set_guides = set_guides,
	.set_neaten = set_neaten,
	.set_text = set_text,
	.set_context = set_context,
	.clear = obj_clear,
	.report = report,
	.get_region = get_region,
	.set_mode = obj_set_mode,
};

/* ---- glassine_handwriting_manager_v1 ---- */

static void mgr_destroy(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void get_handwriting(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface)
{
	struct hw_obj *o;
	wl_list_for_each(o, &H.objs, link) {
		if (o->surface == surface) {
			wl_resource_post_error(r, GLASSINE_HANDWRITING_MANAGER_V1_ERROR_ALREADY_EXISTS,
					       "the surface has a handwriting object");
			return;
		}
	}
	o = calloc(1, sizeof *o);
	if (!o) { wl_client_post_no_memory(c); return; }
	o->res = wl_resource_create(c, &glassine_handwriting_v1_interface, wl_resource_get_version(r), id);
	if (!o->res) { free(o); wl_client_post_no_memory(c); return; }
	o->surface = surface;
	o->sid = ++H.next_sid;
	wl_list_init(&o->regions);
	o->surface_destroy.notify = surface_gone;
	wl_resource_add_destroy_listener(surface, &o->surface_destroy);
	wl_list_insert(&H.objs, &o->link);
	wl_resource_set_implementation(o->res, &obj_impl, o, obj_resource_destroy);
}

static const struct glassine_handwriting_manager_v1_interface mgr_impl = {
	.destroy = mgr_destroy,
	.get_handwriting = get_handwriting,
};

static void mgr_unbind(struct wl_resource *r)
{
	wl_list_remove(wl_resource_get_link(r));
}

static void mgr_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &glassine_handwriting_manager_v1_interface, version, id);
	if (!r) { wl_client_post_no_memory(c); return; }
	wl_resource_set_implementation(r, &mgr_impl, NULL, mgr_unbind);
	wl_list_insert(&H.managers, wl_resource_get_link(r));
	send_caps(r);
	if (H.fd < 0 && !H.timer) svc_start();
}

/* ---- the pen ---- */

void glassine_hw_init(struct wl_display *display, const struct glassine_hw_hooks *hooks, const char *service)
{
	H.display = display;
	H.loop = wl_display_get_event_loop(display);
	if (hooks) H.hooks = *hooks;
	wl_list_init(&H.objs);
	wl_list_init(&H.managers);
	wl_array_init(&H.pts);
	H.cmd = service ? strdup(service) : NULL;
	wl_global_create(display, &glassine_handwriting_manager_v1_interface, 3, NULL, mgr_bind);
	svc_start();
}

void glassine_hw_set_per_mm(double per_mm)
{
	if (per_mm == H.per_mm) return;
	H.per_mm = per_mm;
	struct hw_obj *o;
	wl_list_for_each(o, &H.objs, link) {
		if (o->announced && !o->region_mode) send_set(o);
		struct hw_region *r;
		wl_list_for_each(r, &o->regions, link) if (r->announced) region_send_set(r);
	}
}

/* the object that takes the surface's strokes (as glassine_hw_pen_down picks it) */
static struct hw_obj *obj_on(struct wl_resource *surface)
{
	struct hw_obj *o, *hit = NULL;
	if (!surface) return NULL;
	wl_list_for_each(o, &H.objs, link) if (o->surface == surface) hit = o;
	return hit;
}

static int region_above(const void *a, const void *b)
{
	const struct hw_region *x = *(struct hw_region *const *)a, *y = *(struct hw_region *const *)b;
	if (x->layer != y->layer) return x->layer > y->layer ? -1 : 1;
	return x->seq > y->seq ? -1 : x->seq < y->seq ? 1 : 0;
}

bool glassine_hw_regions(struct wl_resource *surface, glassine_hw_region_cb cb, void *data)
{
	struct hw_obj *o = obj_on(surface);
	if (!o || !o->region_mode) return false;
	if (!cb) return true;
	size_t n = 0, k = 0;
	struct hw_region *r, **all;
	wl_list_for_each(r, &o->regions, link) n++;
	if (!n) return true;
	if (!(all = malloc(n * sizeof *all))) return true;
	wl_list_for_each(r, &o->regions, link) all[k++] = r;
	qsort(all, n, sizeof *all, region_above);
	for (k = 0; k < n; k++) {
		r = all[k];
		if (r->w <= 0 || r->h <= 0) continue;
		const double rect[4] = { r->x, r->y, r->w, r->h };
		cb(data, rect, r->clip, r->mode == GLASSINE_HANDWRITING_V1_MODE_INK);
	}
	free(all);
	return true;
}

void glassine_hw_axis(double pressure, double tilt_x, double tilt_y)
{
	if (!isnan(pressure)) H.pressure = pressure;
	if (!isnan(tilt_x)) H.tilt_x = tilt_x;
	if (!isnan(tilt_y)) H.tilt_y = tilt_y;
}

static void add_point(double sx, double sy, uint32_t t)
{
	if (H.pts.size >= MAX_POINTS * 6 * sizeof(int32_t)) return;
	int32_t *p = wl_array_add(&H.pts, 6 * sizeof *p);
	if (!p) return;
	double pr = H.pressure < 0 ? 0 : H.pressure > 1 ? 1 : H.pressure;
	p[0] = wl_fixed_from_double(sx);
	p[1] = wl_fixed_from_double(sy);
	p[2] = (int32_t)t;
	p[3] = (int32_t)lround(pr * 65535);
	p[4] = wl_fixed_from_double(H.tilt_x);
	p[5] = wl_fixed_from_double(H.tilt_y);
}

bool glassine_hw_pen_down(struct wl_resource *surface, double sx, double sy, double lx, double ly, uint32_t t)
{
	glassine_hw_pen_up(t);
	if (!surface) return false;
	struct hw_obj *hit = obj_on(surface);
	if (!hit) return false;
	struct hw_region *top = NULL;
	if (hit->region_mode) {
		/* the topmost region the stroke starts in: the highest layer, then the newest */
		struct hw_region *r;
		wl_list_for_each(r, &hit->regions, link) {
			if (sx < r->x || sy < r->y || sx >= r->x + r->w || sy >= r->y + r->h) continue;
			if (r->clip && H.hooks.region_contains && !H.hooks.region_contains(r->clip, sx, sy)) continue;
			if (!top || r->layer > top->layer || (r->layer == top->layer && r->seq > top->seq)) top = r;
		}
		if (!top || top->mode != GLASSINE_HANDWRITING_V1_MODE_INK) return false;
	} else if (hit->area && H.hooks.region_contains && !H.hooks.region_contains(hit->area, sx, sy)) {
		return false;
	}
	H.cur = hit;
	H.cur_r = top;
	H.rx = top ? top->x : 0;
	H.ry = top ? top->y : 0;
	H.ox = lx - sx;
	H.oy = ly - sy;
	wl_array_release(&H.pts);
	wl_array_init(&H.pts);
	add_point(sx - H.rx, sy - H.ry, t);
	return true;
}

void glassine_hw_pen_motion(double lx, double ly, uint32_t t)
{
	if (H.cur) add_point(lx - H.ox - H.rx, ly - H.oy - H.ry, t);
}

/* a region's stroke at pen-up: to the app, and to the service when it wants it */
static void region_pen_up(struct hw_region *r)
{
	uint32_t k = r->next;
	if (r->next < UINT32_MAX) r->next++;
	const size_t point = 6 * sizeof(int32_t);
	size_t n = H.pts.size / point;
	for (size_t i = 0; i < n; i += PART) {
		struct wl_array a = { .size = (n - i < PART ? n - i : PART) * point, .alloc = 0,
				      .data = (char *)H.pts.data + i * point };
		glassine_handwriting_region_v1_send_points(r->res, k, GLASSINE_HANDWRITING_V1_KIND_RAW, &a);
	}
	glassine_handwriting_region_v1_send_stroke(r->res, k);
	r->holds = true;
	region_now(r);
	if (!r->announced && !region_keep(r)) return;
	struct held *h = realloc(r->held, (r->nheld + 1) * sizeof *h);
	if (h) {
		r->held = h;
		h = &r->held[r->nheld++];
		h->k = k;
		wl_array_init(&h->pts);
		wl_array_copy(&h->pts, &H.pts);
	}
	if (!r->announced) return;              /* held for the service's hello */
	json_object *m = rmsg("region_stroke", r);
	json_object_object_add(m, "k", json_object_new_int64(k));
	json_object_object_add(m, "points", points_json(&H.pts));
	svc_send(m);
}

void glassine_hw_pen_up(uint32_t t)
{
	struct hw_obj *o = H.cur;
	struct hw_region *cr = H.cur_r;
	if (!o) return;
	H.cur = NULL;
	H.cur_r = NULL;
	(void)t;
	if (cr) {
		region_pen_up(cr);
		wl_array_release(&H.pts);
		wl_array_init(&H.pts);
		return;
	}
	uint32_t k = o->next++;
	send_points(o, k, GLASSINE_HANDWRITING_V1_KIND_RAW, &H.pts);
	glassine_handwriting_v1_send_stroke(o->res, k);
	if (o->idle) { wl_event_source_remove(o->idle); o->idle = NULL; announce(o); }
	if (wants_service(o) && H.fd >= 0 && H.up) {
		if (!o->announced) announce(o);
		/* held for its neat_stroke: the raw points, only x and y replaced */
		struct held *h = realloc(o->held, (o->nheld + 1) * sizeof *h);
		if (h) {
			o->held = h;
			h = &o->held[o->nheld++];
			h->k = k;
			wl_array_init(&h->pts);
			wl_array_copy(&h->pts, &H.pts);
		}
		json_object *m = msg("stroke", o->sid);
		json_object_object_add(m, "k", json_object_new_int64(k));
		json_object_object_add(m, "points", points_json(&H.pts));
		svc_send(m);
	}
	wl_array_release(&H.pts);
	wl_array_init(&H.pts);
}

bool glassine_hw_pen_active(void)
{
	return H.cur != NULL;
}
