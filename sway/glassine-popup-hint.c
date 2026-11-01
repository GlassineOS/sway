/* glassine-popup-hint.c — app menus (xdg_popup) as popups for glassine-epdd-desktop. Built into
 * the patched sway (0003): on X, glassine-epdd-desktop sees a menu window map and unmap and
 * paints it as a popup (DU at once, GC16 when it closes); sway IPC doesn't
 * report xdg_popups, so sway itself writes `popup X Y W H` / `popup gone X Y W H`
 * (portrait panel px = layout px x the output scale, turned back by glassine-rotd's
 * /run/glassine-rotation) to glassine-epdd-desktop's FIFO.
 * Nothing happens when glassine-epdd-desktop isn't there (O_NONBLOCK open fails). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "sway/server.h"
#include "sway/tree/root.h"
#include "glassine-popup-hint.h"

#define EPDD_CTL "/run/glassine-epdd-desktop.ctl"

struct glassine_popup {
	struct wlr_xdg_popup *popup;
	struct wlr_scene_tree *tree;    /* the popup's xdg surface tree: at its geometry origin */
	struct wl_listener map, unmap, destroy;
	struct wl_event_source *idle;
	int box[4];                     /* as sent: x y w h, panel px */
	bool shown;
};

static void hint(const char *what, const int b[4]) {
	char c[96];
	int n = snprintf(c, sizeof c, "%s %d %d %d %d\n", what, b[0], b[1], b[2], b[3]);
	int fd = open(EPDD_CTL, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		return;
	}
	if (write(fd, c, n) != n) {
		/* a full FIFO: glassine-epdd-desktop is busy, the menu is painted as damage */
	}
	close(fd);
}

/* after the map's commit is done: the scene positions are up to date then */
static void show(void *data) {
	struct glassine_popup *p = data;
	p->idle = NULL;
	struct wlr_surface *s = p->popup->base->surface;
	if (!s->mapped) {
		return;
	}
	int lx, ly;
	if (!wlr_scene_node_coords(&p->tree->node, &lx, &ly)) {
		return;                 /* not shown (a hidden workspace) */
	}
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(p->popup->base, &geo);
	/* the whole surface: shadows around the menu change too */
	double x = lx - geo.x, y = ly - geo.y, w = s->current.width, h = s->current.height;
	struct wlr_output *o = wlr_output_layout_output_at(root->output_layout, x + w / 2, y + h / 2);
	if (!o) {
		return;
	}
	struct wlr_box ob;
	wlr_output_layout_get_box(root->output_layout, o, &ob);
	float sc = o->scale;
	int b[4] = { (int)((x - ob.x) * sc), (int)((y - ob.y) * sc), (int)(w * sc + 0.999), (int)(h * sc + 0.999) };
	/* turned by glassine-rotd (/run/glassine-rotation, clockwise as `transform`): back to
	 * portrait px, as glassine-swayd does; 270 = the device turned clockwise */
	int ow = (int)(ob.width * sc + 0.5), oh = (int)(ob.height * sc + 0.5), rot = 0;
	FILE *rf = fopen("/run/glassine-rotation", "r");
	if (rf) {
		if (fscanf(rf, "%d", &rot) != 1) {
			rot = 0;
		}
		fclose(rf);
	}
	switch (rot) {
	case 270: p->box[0] = b[1]; p->box[1] = ow - b[0] - b[2]; p->box[2] = b[3]; p->box[3] = b[2]; break;
	case 90: p->box[0] = oh - b[1] - b[3]; p->box[1] = b[0]; p->box[2] = b[3]; p->box[3] = b[2]; break;
	case 180: p->box[0] = ow - b[0] - b[2]; p->box[1] = oh - b[1] - b[3]; p->box[2] = b[2]; p->box[3] = b[3]; break;
	default: memcpy(p->box, b, sizeof b);
	}
	p->shown = true;
	hint("popup", p->box);
}

static void gone(struct glassine_popup *p) {
	if (p->idle) {
		wl_event_source_remove(p->idle);
		p->idle = NULL;
	}
	if (p->shown) {
		hint("popup gone", p->box);
		p->shown = false;
	}
}

static void on_map(struct wl_listener *l, void *data) {
	struct glassine_popup *p = wl_container_of(l, p, map);
	if (!p->idle) {
		p->idle = wl_event_loop_add_idle(server.wl_event_loop, show, p);
	}
}

static void on_unmap(struct wl_listener *l, void *data) {
	struct glassine_popup *p = wl_container_of(l, p, unmap);
	gone(p);
}

static void on_destroy(struct wl_listener *l, void *data) {
	struct glassine_popup *p = wl_container_of(l, p, destroy);
	gone(p);
	wl_list_remove(&p->map.link);
	wl_list_remove(&p->unmap.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

void glassine_popup_track(struct wlr_xdg_popup *popup, struct wlr_scene_tree *tree) {
	struct glassine_popup *p = calloc(1, sizeof *p);
	if (!p) {
		return;
	}
	p->popup = popup;
	p->tree = tree;
	p->map.notify = on_map;
	wl_signal_add(&popup->base->surface->events.map, &p->map);
	p->unmap.notify = on_unmap;
	wl_signal_add(&popup->base->surface->events.unmap, &p->unmap);
	p->destroy.notify = on_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);
}
