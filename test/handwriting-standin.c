/* handwriting-standin.c — a stand-in compositor for the pen interface's host tests: a
 * Wayland server with wl_compositor (surfaces that do nothing, regions of rectangles) and
 * sway's glassine-handwriting.c, driven by a stand-in pen on stdin:
 *
 *   down N X Y T P TX TY     the tip on surface N (0 = the first one created) at X, Y
 *   move X Y T P TX TY       on the same surface
 *   up T
 *   per-mm V                 the output's px per mm
 *
 * Each line is answered with "ok" on stdout once done. Clients get the sandbox ids of
 * $STANDIN_SANDBOX (comma-separated, in the order they connect; "-": not sandboxed). The
 * service is $GLASSINE_HANDWRITING_SERVICE (empty: none). The socket is $WAYLAND_DISPLAY in
 * $XDG_RUNTIME_DIR. Built by test/handwriting-standin.sh. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server.h>
#include "glassine-handwriting.h"

#define MAXS 64

static struct wl_resource *surfaces[MAXS];
static int nsurf;
static int nclients;
static struct wl_display *display;

/* ---- wl_region: rectangles added and subtracted, in order ---- */
struct rect { int x, y, w, h, add; };
struct region { struct rect *r; int n; };

static void region_destroy_res(struct wl_resource *res)
{
	struct region *g = wl_resource_get_user_data(res);
	free(g->r);
	free(g);
}

static void region_op(struct wl_resource *res, int x, int y, int w, int h, int add)
{
	struct region *g = wl_resource_get_user_data(res);
	struct rect *r = realloc(g->r, (g->n + 1) * sizeof *r);
	if (!r) return;
	g->r = r;
	g->r[g->n++] = (struct rect){ x, y, w, h, add };
}

static void region_add(struct wl_client *c, struct wl_resource *r, int x, int y, int w, int h) { region_op(r, x, y, w, h, 1); }
static void region_sub(struct wl_client *c, struct wl_resource *r, int x, int y, int w, int h) { region_op(r, x, y, w, h, 0); }
static void res_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }

static const struct wl_region_interface region_impl = { .destroy = res_destroy, .add = region_add, .subtract = region_sub };

static void *region_copy(struct wl_resource *res)
{
	struct region *g = wl_resource_get_user_data(res), *c = calloc(1, sizeof *c);
	if (!c) return NULL;
	c->r = malloc((g->n ? g->n : 1) * sizeof *c->r);
	if (c->r) { memcpy(c->r, g->r, g->n * sizeof *c->r); c->n = g->n; }
	return c;
}

static bool region_contains(void *p, double x, double y)
{
	struct region *g = p;
	bool in = false;
	for (int i = 0; i < g->n; i++) {
		struct rect *r = &g->r[i];
		if (x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h) in = r->add;
	}
	return in;
}

static void region_free(void *p)
{
	struct region *g = p;
	free(g->r);
	free(g);
}

static void region_box(void *p, int box[4])
{
	struct region *g = p;
	box[0] = box[1] = 1 << 30;
	box[2] = box[3] = -(1 << 30);
	for (int i = 0; i < g->n; i++) {
		struct rect *r = &g->r[i];
		if (!r->add) continue;
		if (r->x < box[0]) box[0] = r->x;
		if (r->y < box[1]) box[1] = r->y;
		if (r->x + r->w > box[2]) box[2] = r->x + r->w;
		if (r->y + r->h > box[3]) box[3] = r->y + r->h;
	}
	if (box[2] < box[0]) box[0] = box[1] = box[2] = box[3] = 0;
}

/* ---- wl_surface: nothing but its existence ---- */
static void s_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *b, int x, int y) {}
static void s_damage(struct wl_client *c, struct wl_resource *r, int x, int y, int w, int h) {}
static void s_frame(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
	if (cb) { wl_callback_send_done(cb, 0); wl_resource_destroy(cb); }
}
static void s_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *g) {}
static void s_commit(struct wl_client *c, struct wl_resource *r) {}
static void s_int(struct wl_client *c, struct wl_resource *r, int32_t v) {}

static const struct wl_surface_interface surface_impl = {
	.destroy = res_destroy, .attach = s_attach, .damage = s_damage, .frame = s_frame,
	.set_opaque_region = s_region, .set_input_region = s_region, .commit = s_commit,
	.set_buffer_transform = s_int, .set_buffer_scale = s_int, .damage_buffer = s_damage,
};

static void surface_gone(struct wl_resource *r)
{
	for (int i = 0; i < nsurf; i++) if (surfaces[i] == r) surfaces[i] = NULL;
}

static void create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct wl_resource *s = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
	if (!s) { wl_client_post_no_memory(c); return; }
	wl_resource_set_implementation(s, &surface_impl, NULL, surface_gone);
	if (nsurf < MAXS) surfaces[nsurf++] = s;
}

static void create_region(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct wl_resource *g = wl_resource_create(c, &wl_region_interface, 1, id);
	struct region *p = calloc(1, sizeof *p);
	if (!g || !p) { free(p); wl_client_post_no_memory(c); return; }
	wl_resource_set_implementation(g, &region_impl, p, region_destroy_res);
}

static const struct wl_compositor_interface compositor_impl = { .create_surface = create_surface,
								 .create_region = create_region };

static void bind_compositor(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, version < 4 ? version : 4, id);
	if (r) wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}

/* ---- identity: the clients' sandbox ids by the order they connect ---- */
struct who { struct wl_listener destroy; struct wl_client *c; char id[128]; struct who *next; };
static struct who *whos;

static void client_gone(struct wl_listener *l, void *data)
{
	struct who *w = wl_container_of(l, w, destroy);
	w->c = NULL;
}

static void client_created(struct wl_listener *l, void *data)
{
	struct wl_client *c = data;
	struct who *w = calloc(1, sizeof *w);
	if (!w) return;
	w->c = c;
	const char *s = getenv("STANDIN_SANDBOX");
	int k = nclients++;
	while (s && k > 0 && (s = strchr(s, ','))) { s++; k--; }
	if (s && k == 0) {
		size_t n = strcspn(s, ",");
		if (n >= sizeof w->id) n = sizeof w->id - 1;
		memcpy(w->id, s, n);
		if (!strcmp(w->id, "-")) w->id[0] = 0;
	}
	w->destroy.notify = client_gone;
	wl_client_add_destroy_listener(c, &w->destroy);
	w->next = whos;
	whos = w;
}

static void identify(struct wl_client *c, char *buf, size_t size)
{
	buf[0] = 0;
	for (struct who *w = whos; w; w = w->next) if (w->c == c) { snprintf(buf, size, "%s", w->id); return; }
}

/* ---- the stand-in pen ---- */
static int on_stdin(int fd, uint32_t mask, void *data)
{
	static char line[4096];
	static size_t n;
	ssize_t r = read(fd, line + n, sizeof line - 1 - n);
	if (r <= 0) { wl_display_terminate(display); return 0; }
	n += (size_t)r;
	char *s = line, *e;
	line[n] = 0;
	while ((e = strchr(s, '\n'))) {
		*e = 0;
		int k;
		double x, y, p, tx, ty, v;
		unsigned t;
		if (sscanf(s, "down %d %lf %lf %u %lf %lf %lf", &k, &x, &y, &t, &p, &tx, &ty) == 7) {
			glassine_hw_axis(p, tx, ty);
			/* the caller's coordinates: the surface's origin at (100, 50) */
			glassine_hw_pen_down(k >= 0 && k < nsurf ? surfaces[k] : NULL, x, y, x + 100, y + 50, t);
		} else if (sscanf(s, "move %lf %lf %u %lf %lf %lf", &x, &y, &t, &p, &tx, &ty) == 6) {
			glassine_hw_axis(p, tx, ty);
			glassine_hw_pen_motion(x + 100, y + 50, t);
		} else if (sscanf(s, "up %u", &t) == 1) {
			glassine_hw_pen_up(t);
		} else if (sscanf(s, "per-mm %lf", &v) == 1) {
			glassine_hw_set_per_mm(v);
		}
		wl_display_flush_clients(display);
		printf("ok\n");
		fflush(stdout);
		s = e + 1;
	}
	n -= (size_t)(s - line);
	memmove(line, s, n);
	return 0;
}

int main(void)
{
	display = wl_display_create();
	if (!display) return 1;
	const char *sock = wl_display_add_socket_auto(display);
	if (!sock) return 1;
	static struct wl_listener created = { .notify = client_created };
	wl_display_add_client_created_listener(display, &created);
	wl_global_create(display, &wl_compositor_interface, 4, NULL, bind_compositor);
	static const struct glassine_hw_hooks hooks = { .identify = identify, .region_copy = region_copy,
							.region_contains = region_contains, .region_free = region_free,
							.region_box = region_box };
	const char *svc = getenv("GLASSINE_HANDWRITING_SERVICE");
	glassine_hw_init(display, &hooks, svc);
	wl_event_loop_add_fd(wl_display_get_event_loop(display), 0, WL_EVENT_READABLE, on_stdin, NULL);
	printf("socket %s\n", sock);
	fflush(stdout);
	wl_display_run(display);
	wl_display_destroy_clients(display);
	wl_display_destroy(display);
	return 0;
}
