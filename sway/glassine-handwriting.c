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
	struct held { uint32_t k; struct wl_array pts; } *held;
	size_t nheld;
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
	/* the stroke being written */
	struct hw_obj *cur;
	struct wl_array pts;
	double ox, oy;                  /* the surface's origin in the caller's coordinates */
	double pressure, tilt_x, tilt_y;
	double per_mm;
} H = { .fd = -1 };

/* ---- the service's channel ---- */

static void svc_close(void);

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
	if (H.per_mm > 0) snprintf(u, sizeof u, "device:%.4g", H.per_mm);
	else snprintf(u, sizeof u, "device");
	json_object_object_add(m, "units", json_object_new_string(u));
	svc_send(m);
}

/* the service learns of a surface once it is there and the surface wants it */
static void announce(struct hw_obj *o)
{
	if (H.fd < 0 || !H.up) return;
	if (o->announced && !wants_service(o)) {
		/* now private or off: the service forgets the surface and all it held */
		svc_send(msg("destroy", o->sid));
		o->announced = false;
		return;
	}
	if (!o->announced) {
		if (!wants_service(o)) return;
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
	send_set(o);
}

static void send_caps(struct wl_resource *mgr)
{
	glassine_handwriting_manager_v1_send_capabilities(mgr, H.caps | GLASSINE_HANDWRITING_MANAGER_V1_CAPABILITY_STROKES);
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
	wl_list_for_each(o, &H.objs, link) announce(o);
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

static void got(json_object *m)
{
	const char *op = str_of(m, "op");
	if (!strcmp(op, "hello")) { got_hello(m); return; }
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
	struct hw_obj *o;
	wl_list_for_each(o, &H.objs, link) o->announced = false;
	if (was) caps_changed();
	if (!was && ++H.fails >= 5) return;     /* it can't start: raw strokes only, until the next login */
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

static void obj_free(struct hw_obj *o)
{
	if (H.cur == o) { H.cur = NULL; wl_array_release(&H.pts); wl_array_init(&H.pts); }
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
	if (H.cur == o) { H.cur = NULL; wl_array_release(&H.pts); wl_array_init(&H.pts); }
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
	if (o->announced) svc_send(msg("clear", o->sid));
}

static void report(struct wl_client *c, struct wl_resource *r, uint32_t serial, uint32_t action, uint32_t rank,
		   const char *text)
{
	struct hw_obj *o = obj_from(r);
	static const char *names[] = { "picked", "accepted", "corrected", "discarded" };
	if (!o->announced || !o->text || action > 3) {
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

static const struct glassine_handwriting_v1_interface obj_impl = {
	.destroy = obj_destroy,
	.set_area = set_area,
	.set_guides = set_guides,
	.set_neaten = set_neaten,
	.set_text = set_text,
	.set_context = set_context,
	.clear = obj_clear,
	.report = report,
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
	wl_global_create(display, &glassine_handwriting_manager_v1_interface, 1, NULL, mgr_bind);
	svc_start();
}

void glassine_hw_set_per_mm(double per_mm)
{
	if (per_mm == H.per_mm) return;
	H.per_mm = per_mm;
	struct hw_obj *o;
	wl_list_for_each(o, &H.objs, link) if (o->announced) send_set(o);
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
	struct hw_obj *o, *hit = NULL;
	wl_list_for_each(o, &H.objs, link) if (o->surface == surface) hit = o;
	if (!hit) return false;
	if (hit->area && H.hooks.region_contains && !H.hooks.region_contains(hit->area, sx, sy)) return false;
	H.cur = hit;
	H.ox = lx - sx;
	H.oy = ly - sy;
	wl_array_release(&H.pts);
	wl_array_init(&H.pts);
	add_point(sx, sy, t);
	return true;
}

void glassine_hw_pen_motion(double lx, double ly, uint32_t t)
{
	if (H.cur) add_point(lx - H.ox, ly - H.oy, t);
}

void glassine_hw_pen_up(uint32_t t)
{
	struct hw_obj *o = H.cur;
	if (!o) return;
	H.cur = NULL;
	(void)t;
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
		json_object *m = msg("stroke", o->sid), *pts = json_object_new_array();
		json_object_object_add(m, "k", json_object_new_int64(k));
		const int32_t *p = H.pts.data;
		size_t n = H.pts.size / (6 * sizeof *p);
		for (size_t i = 0; i < n; i++, p += 6) {
			json_object *q = json_object_new_array();
			json_object_array_add(q, json_object_new_double(wl_fixed_to_double(p[0])));
			json_object_array_add(q, json_object_new_double(wl_fixed_to_double(p[1])));
			json_object_array_add(q, json_object_new_int64((uint32_t)p[2]));
			json_object_array_add(q, json_object_new_double(p[3] / 65535.0));
			json_object_array_add(q, json_object_new_double(wl_fixed_to_double(p[4])));
			json_object_array_add(q, json_object_new_double(wl_fixed_to_double(p[5])));
			json_object_array_add(pts, q);
		}
		json_object_object_add(m, "points", pts);
		svc_send(m);
	}
	wl_array_release(&H.pts);
	wl_array_init(&H.pts);
}

bool glassine_hw_pen_active(void)
{
	return H.cur != NULL;
}
