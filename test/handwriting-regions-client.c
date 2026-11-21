/* handwriting-regions-client.c — a client of glassine_handwriting_v1's writing regions
 * (version 2) for handwriting-regions-test.py. Commands on stdin, one per line; what the
 * compositor sends on stdout, one line per event:
 *
 *   surface                          a surface (the stand-in's surface 0) and its object
 *   region NAME X Y W H [LAYER]      a region with that rectangle
 *   rect NAME X Y W H                set_rect
 *   clip NAME X Y W H | clip NAME -  set_clip with one rectangle, or null
 *   ctx NAME PURPOSE HINTS [LANGS]   set_context
 *   reading NAME 0|1                 set_reading
 *   neaten NAME 0|1 STRENGTH         set_neaten
 *   layer NAME L                     set_layer
 *   mode NAME 0|1                    set_mode (0 ink, 1 text); modeall 0|1: the object's
 *   edit NAME TEXT                   the text, then edit; long NAME N: edit with N bytes
 *   report NAME SERIAL ACTION RANK [TEXT]
 *   restore NAME K,K… VALUE|- STATE|-  stored strokes K (3 points each) and restore
 *   destroy NAME                     the region's destroy
 *   sync                             a round trip, then "synced"
 *
 * Run with -1 it binds the manager at version 1, prints its capabilities and stops. */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "glassine-handwriting-v1-client-protocol.h"

#define MAXR 32

static struct wl_display *dpy;
static struct wl_compositor *comp;
static struct glassine_handwriting_manager_v1 *mgr;
static struct glassine_handwriting_v1 *hw;
static uint32_t caps, version = 2;

struct reg {
	char name[64];
	struct glassine_handwriting_region_v1 *r;
	struct wl_array text, state, strokes, shape;    /* data parts gathered */
	struct wl_array pts;                            /* points parts */
};
static struct reg regs[MAXR];
static int nregs;

static struct reg *find(const char *name)
{
	for (int i = 0; i < nregs; i++) if (regs[i].r && !strcmp(regs[i].name, name)) return &regs[i];
	return NULL;
}

static void m_caps(void *d, struct glassine_handwriting_manager_v1 *m, uint32_t c) { caps = c; }
static void m_lang(void *d, struct glassine_handwriting_manager_v1 *m, const char *l) {}
static void m_done(void *d, struct glassine_handwriting_manager_v1 *m) { printf("caps %u\n", caps); fflush(stdout); }
static const struct glassine_handwriting_manager_v1_listener ml = { m_caps, m_lang, m_done };

static void take(struct wl_array *a, struct wl_array *out)
{
	*out = *a;
	wl_array_init(a);
}

static void print_strokes(struct wl_array *a)
{
	uint32_t *k;
	int first = 1;
	printf(" strokes=");
	wl_array_for_each(k, a) { printf(first ? "%u" : ",%u", *k); first = 0; }
}

static void r_points(void *d, struct glassine_handwriting_region_v1 *r, uint32_t k, uint32_t kind, struct wl_array *a)
{
	struct reg *g = d;
	void *p = wl_array_add(&g->pts, a->size);
	if (p) memcpy(p, a->data, a->size);
}

static void end_points(struct reg *g, const char *what, uint32_t k, int how)
{
	int32_t *p = g->pts.data;
	size_t n = g->pts.size / (6 * sizeof *p);
	printf("%s %s %u", what, g->name, k);
	if (how >= 0) printf(" %d", how);
	printf(" %zu %.2f %.2f\n", n, n ? wl_fixed_to_double(p[0]) : 0, n ? wl_fixed_to_double(p[1]) : 0);
	fflush(stdout);
	wl_array_release(&g->pts);
	wl_array_init(&g->pts);
}

static void r_stroke(void *d, struct glassine_handwriting_region_v1 *r, uint32_t k) { end_points(d, "stroke", k, -1); }
static void r_neat(void *d, struct glassine_handwriting_region_v1 *r, uint32_t k, uint32_t how) { end_points(d, "neat", k, (int)how); }

static void r_data(void *d, struct glassine_handwriting_region_v1 *r, uint32_t kind, struct wl_array *a)
{
	struct reg *g = d;
	struct wl_array *to = kind == 0 ? &g->text : kind == 1 ? &g->state : kind == 2 ? &g->strokes : &g->shape;
	if (a->size > 2048) printf("oversized part %zu\n", a->size);
	void *p = wl_array_add(to, a->size);
	if (p) memcpy(p, a->data, a->size);
}

static void r_word(void *d, struct glassine_handwriting_region_v1 *r, uint32_t serial, uint32_t start, uint32_t len,
		   wl_fixed_t dx, wl_fixed_t dy)
{
	struct reg *g = d;
	struct wl_array s;
	take(&g->strokes, &s);
	printf("word %s %u %u %u %.2f %.2f", g->name, serial, start, len, wl_fixed_to_double(dx), wl_fixed_to_double(dy));
	print_strokes(&s);
	printf("\n");
	wl_array_release(&s);
}

static void r_set_word(void *d, struct glassine_handwriting_region_v1 *r, uint32_t serial, uint32_t start,
		       uint32_t len, wl_fixed_t x, wl_fixed_t y, wl_fixed_t w, wl_fixed_t h)
{
	struct reg *g = d;
	struct wl_array s;
	take(&g->shape, &s);
	int32_t *q = s.data;
	printf("setword %s %u %u %u %.2f %.2f %.2f %.2f shape=%zu op0=%d\n", g->name, serial, start, len,
	       wl_fixed_to_double(x), wl_fixed_to_double(y), wl_fixed_to_double(w), wl_fixed_to_double(h),
	       s.size / 4, s.size ? q[0] : -1);
	wl_array_release(&s);
}

static void r_gone(void *d, struct glassine_handwriting_region_v1 *r, uint32_t serial)
{
	struct reg *g = d;
	struct wl_array s;
	take(&g->strokes, &s);
	printf("gone %s %u", g->name, serial);
	print_strokes(&s);
	printf("\n");
	wl_array_release(&s);
}

static void r_value(void *d, struct glassine_handwriting_region_v1 *r, uint32_t serial)
{
	struct reg *g = d;
	struct wl_array t, st;
	take(&g->text, &t);
	take(&g->state, &st);
	printf("value %s %u len=%zu state=%.*s text=%.*s\n", g->name, serial, t.size, (int)(st.size < 40 ? st.size : 40),
	       st.size ? (char *)st.data : "", (int)(t.size < 60 ? t.size : 60), t.size ? (char *)t.data : "");
	fflush(stdout);
	wl_array_release(&t);
	wl_array_release(&st);
}

static void r_cand(void *d, struct glassine_handwriting_region_v1 *r, uint32_t serial, uint32_t rank, uint32_t origin,
		   uint32_t fit)
{
	struct reg *g = d;
	struct wl_array t;
	take(&g->text, &t);
	printf("cand %s %u %u %u %u %.*s\n", g->name, serial, rank, origin, fit, (int)t.size, t.size ? (char *)t.data : "");
	wl_array_release(&t);
}

static void r_cands_done(void *d, struct glassine_handwriting_region_v1 *r, uint32_t serial, uint32_t n)
{
	printf("cands_done %s %u %u\n", ((struct reg *)d)->name, serial, n);
	fflush(stdout);
}

static void r_gesture(void *d, struct glassine_handwriting_region_v1 *r, uint32_t serial, uint32_t kind)
{
	struct reg *g = d;
	struct wl_array s;
	take(&g->strokes, &s);
	printf("gesture %s %u %u", g->name, serial, kind);
	print_strokes(&s);
	printf("\n");
	wl_array_release(&s);
}

static void r_report_done(void *d, struct glassine_handwriting_region_v1 *r, uint32_t serial, uint32_t result)
{
	printf("report_done %s %u %u\n", ((struct reg *)d)->name, serial, result);
	fflush(stdout);
}

static const struct glassine_handwriting_region_v1_listener rl_ = {
	.points = r_points, .stroke = r_stroke, .neat_stroke = r_neat, .data_part = r_data, .word = r_word,
	.set_word = r_set_word, .gone = r_gone, .value = r_value, .candidate = r_cand,
	.candidates_done = r_cands_done, .gesture = r_gesture, .report_done = r_report_done,
};

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	if (!strcmp(iface, glassine_handwriting_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name, &glassine_handwriting_manager_v1_interface, version < v ? version : v);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener rl = { global, global_remove };

static wl_fixed_t F(const char *s) { return wl_fixed_from_double(atof(s)); }

static void text_parts(struct glassine_handwriting_region_v1 *r, uint32_t kind, const char *t, size_t n)
{
	for (size_t i = 0; i < n; i += 2048) {
		struct wl_array a = { .size = n - i < 2048 ? n - i : 2048, .alloc = 0, .data = (char *)t + i };
		glassine_handwriting_region_v1_add_data(r, kind, &a);
	}
}

static void command(char *line)
{
	char *w[8] = { 0 }, *rest = NULL;
	int n = 0;
	if (!strncmp(line, "edit ", 5)) {       /* edit NAME TEXT: the text to the end of the line */
		char *sp = strchr(line + 5, ' ');
		if (sp) { *sp = 0; rest = sp + 1; }
	}
	for (char *p = line; *p && n < 8;) {
		while (*p == ' ') p++;
		if (!*p) break;
		w[n++] = p;
		while (*p && *p != ' ') p++;
		if (*p) *p++ = 0;
	}
	if (!n) return;
	struct reg *g = n > 1 ? find(w[1]) : NULL;
	if (!strcmp(w[0], "surface")) {
		struct wl_surface *s = wl_compositor_create_surface(comp);
		hw = glassine_handwriting_manager_v1_get_handwriting(mgr, s);
	} else if (!strcmp(w[0], "region") && n >= 6 && nregs < MAXR) {
		g = &regs[nregs++];
		snprintf(g->name, sizeof g->name, "%s", w[1]);
		g->r = glassine_handwriting_v1_get_region(hw, w[1]);
		glassine_handwriting_region_v1_add_listener(g->r, &rl_, g);
		glassine_handwriting_region_v1_set_rect(g->r, F(w[2]), F(w[3]), F(w[4]), F(w[5]));
		if (n >= 7) glassine_handwriting_region_v1_set_layer(g->r, atoi(w[6]));
	} else if (!strcmp(w[0], "modeall") && n >= 2) {
		glassine_handwriting_v1_set_mode(hw, (uint32_t)atoi(w[1]));
	} else if (!strcmp(w[0], "sync")) {
		wl_display_roundtrip(dpy);
		printf("synced\n");
	} else if (!g) {
		printf("unknown %s\n", w[0]);
	} else if (!strcmp(w[0], "rect") && n >= 6) {
		glassine_handwriting_region_v1_set_rect(g->r, F(w[2]), F(w[3]), F(w[4]), F(w[5]));
	} else if (!strcmp(w[0], "clip")) {
		struct wl_region *c = NULL;
		if (n >= 6) {
			c = wl_compositor_create_region(comp);
			wl_region_add(c, atoi(w[2]), atoi(w[3]), atoi(w[4]), atoi(w[5]));
		}
		glassine_handwriting_region_v1_set_clip(g->r, c);
		if (c) wl_region_destroy(c);
	} else if (!strcmp(w[0], "ctx") && n >= 4) {
		glassine_handwriting_region_v1_set_context(g->r, (uint32_t)atoi(w[2]), (uint32_t)atoi(w[3]), n >= 5 ? w[4] : "");
	} else if (!strcmp(w[0], "reading") && n >= 3) {
		glassine_handwriting_region_v1_set_reading(g->r, (uint32_t)atoi(w[2]));
	} else if (!strcmp(w[0], "neaten") && n >= 4) {
		glassine_handwriting_region_v1_set_neaten(g->r, (uint32_t)atoi(w[2]), F(w[3]));
	} else if (!strcmp(w[0], "layer") && n >= 3) {
		glassine_handwriting_region_v1_set_layer(g->r, atoi(w[2]));
	} else if (!strcmp(w[0], "mode") && n >= 3) {
		glassine_handwriting_region_v1_set_mode(g->r, (uint32_t)atoi(w[2]));
	} else if (!strcmp(w[0], "edit") && rest) {
		text_parts(g->r, 0, rest, strlen(rest));
		glassine_handwriting_region_v1_edit(g->r);
	} else if (!strcmp(w[0], "long") && n >= 3) {
		size_t k = (size_t)atoi(w[2]);
		char *t = malloc(k);
		for (size_t i = 0; i < k; i++) t[i] = i % 7 == 6 ? ' ' : 'a' + (char)(i % 7);
		text_parts(g->r, 0, t, k);
		free(t);
		glassine_handwriting_region_v1_edit(g->r);
	} else if (!strcmp(w[0], "report") && n >= 5) {
		if (n >= 6) text_parts(g->r, 0, w[5], strlen(w[5]));
		glassine_handwriting_region_v1_report(g->r, (uint32_t)atoi(w[2]), (uint32_t)atoi(w[3]), (uint32_t)atoi(w[4]));
	} else if (!strcmp(w[0], "restore") && n >= 5) {
		for (char *k = strtok(w[2], ","); k; k = strtok(NULL, ",")) {
			int32_t p[18];
			for (int i = 0; i < 3; i++) {
				p[6 * i] = wl_fixed_from_int(10 + i);
				p[6 * i + 1] = wl_fixed_from_int(20);
				p[6 * i + 2] = 1000 + i;
				p[6 * i + 3] = 30000;
				p[6 * i + 4] = p[6 * i + 5] = 0;
			}
			struct wl_array a = { .size = sizeof p, .alloc = 0, .data = p };
			glassine_handwriting_region_v1_restore_points(g->r, &a);
			glassine_handwriting_region_v1_restore_stroke(g->r, (uint32_t)atoi(k));
		}
		if (strcmp(w[3], "-")) text_parts(g->r, 0, w[3], strlen(w[3]));
		if (strcmp(w[4], "-")) text_parts(g->r, 1, w[4], strlen(w[4]));
		glassine_handwriting_region_v1_restore(g->r);
	} else if (!strcmp(w[0], "destroy")) {
		glassine_handwriting_region_v1_destroy(g->r);
		g->r = NULL;
	} else {
		printf("bad %s\n", w[0]);
	}
	fflush(stdout);
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "-1")) version = 1;
	dpy = wl_display_connect(NULL);
	if (!dpy) return 1;
	wl_registry_add_listener(wl_display_get_registry(dpy), &rl, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !mgr) { printf("no global\n"); return 2; }
	glassine_handwriting_manager_v1_add_listener(mgr, &ml, NULL);
	wl_display_roundtrip(dpy);
	if (version == 1) return 0;
	printf("ready\n");
	fflush(stdout);
	char buf[65536];
	size_t have = 0;
	for (;;) {
		wl_display_flush(dpy);
		struct pollfd p[2] = { { wl_display_get_fd(dpy), POLLIN, 0 }, { 0, POLLIN, 0 } };
		if (poll(p, 2, -1) < 0) break;
		if (p[0].revents & POLLIN && wl_display_dispatch(dpy) < 0) { printf("disconnected\n"); break; }
		if (p[0].revents & (POLLHUP | POLLERR)) break;
		if (p[1].revents) {
			ssize_t r = read(0, buf + have, sizeof buf - 1 - have);
			if (r <= 0) break;
			have += (size_t)r;
			buf[have] = 0;
			char *s = buf, *e;
			while ((e = strchr(s, '\n'))) {
				*e = 0;
				command(s);
				s = e + 1;
			}
			have -= (size_t)(s - buf);
			memmove(buf, s, have);
		}
	}
	return 0;
}
