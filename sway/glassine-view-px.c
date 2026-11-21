/* glassine-view-px.c — see glassine-view-px.h */
#include "glassine-view-px.h"
#include <math.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_scene.h>
#include "sway/output.h"
#include "sway/scene_descriptor.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"

int glassine_px_whole(double scale)
{
	int n = (int)floor(scale + 0.5);
	return n < 1 ? 1 : n;
}

double glassine_px_units(const struct sway_view *view)
{
	return view->px_n > 0 ? (double)view->px_scale / view->px_n : 1;
}

double glassine_px_surface_units(struct wlr_surface *surface)
{
	struct sway_view *view = surface ? view_from_wlr_surface(surface) : NULL;
	return view ? glassine_px_units(view) : 1;
}

double glassine_px_node_units(struct wlr_scene_node *node)
{
	int ux, uy;
	struct wlr_scene_tree *t = wlr_scene_node_pixel_tree(node, &ux, &uy);
	return t ? t->px_den / (120.0 * t->px_n) : 1;
}

struct scale_data {
	int n;
	double scale;
};

static void prefer_scale(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	struct scale_data *d = data;
	struct wlr_scene_surface *s = wlr_scene_surface_try_from_buffer(buffer);
	if (!s) return;
	wlr_surface_set_preferred_buffer_scale(s->surface,
		d->n > 0 ? d->n : wlr_fractional_scale_v1_whole_scale(s->surface, d->scale));
}

void glassine_px_popup_tree(const struct sway_view *view, struct wlr_scene_tree *tree)
{
	wlr_scene_tree_set_pixel_units(tree, view->px_n, view->px_scale);
}

bool glassine_px_update(struct sway_view *view, struct sway_output *output)
{
	if (!view->surface) return false;
	float scale = output && output->wlr_output ? output->wlr_output->scale : 1;
	if (!view->px_fractional && wlr_fractional_scale_v1_surface_has_object(view->surface)) view->px_fractional = true;
	int n = 0;
	if (output && view->type == SWAY_VIEW_XDG_SHELL && !view->px_fractional) {
		n = glassine_px_whole(scale);
		if (lroundf(scale * 120) == n * 120) n = 0;
	}
	bool same_scale = lroundf(scale * 120) == lroundf(view->px_scale * 120);
	bool changed = n != view->px_n || (n > 0 && !same_scale);
	if (!changed && same_scale) return false;
	view->px_n = n;
	view->px_scale = scale;
	if (changed) {
		view->px_reconfigure = true;
		wlr_scene_tree_set_pixel_units(view->content_tree, n, scale);
		if (view->saved_surface_tree) wlr_scene_tree_set_pixel_units(view->saved_surface_tree, n, scale);
	}
	struct scale_data d = { n, scale };
	wlr_scene_node_for_each_buffer(&view->content_tree->node, prefer_scale, &d);
	struct wlr_scene_node *node;
	wl_list_for_each(node, &root->layers.popup->children, link) {
		struct sway_popup_desc *desc = scene_descriptor_try_get(node, SWAY_SCENE_DESC_POPUP);
		if (!desc || desc->view != view || node->type != WLR_SCENE_NODE_TREE) continue;
		if (changed) wlr_scene_tree_set_pixel_units(wlr_scene_tree_from_node(node), n, scale);
		wlr_scene_node_for_each_buffer(node, prefer_scale, &d);
	}
	if (changed && view->container) view_center_and_clip_surface(view);
	return changed;
}

/* the layout x, y of the view's output */
static void output_origin(const struct sway_view *view, int *ox, int *oy)
{
	*ox = *oy = 0;
	struct sway_container *con = view->container;
	struct sway_workspace *ws = con ? con->pending.workspace : NULL;
	if (ws && ws->output) {
		*ox = ws->output->lx;
		*oy = ws->output->ly;
	}
}

/* as wlroots' scene scales a box's edge */
static int dev_length(int offset, int length, float scale)
{
	return (int)(round((offset + length) * scale) - round(offset * scale));
}

void glassine_px_box(const struct sway_view *view, double lx, double ly, double width, double height,
		     int *uw, int *uh, int *dw, int *dh)
{
	int ox, oy, n = view->px_n > 0 ? view->px_n : 1;
	output_origin(view, &ox, &oy);
	int w = dev_length((int)lx - ox, (int)width, view->px_scale);
	int h = dev_length((int)ly - oy, (int)height, view->px_scale);
	if (dw) *dw = w;
	if (dh) *dh = h;
	if (uw) *uw = w > 0 ? (w + n - 1) / n : 0;
	if (uh) *uh = h > 0 ? (h + n - 1) / n : 0;
}

static double layout_length(const struct sway_view *view, int offset, int units)
{
	int n = view->px_n;
	int best = (int)floor((double)units * n / view->px_scale);
	if (best < 1) best = 1;
	for (int d = 0; d <= 2; d++) {
		for (int s = -1; s <= 1; s += 2) {
			int l = best + s * d;
			if (l > 0 && (dev_length(offset, l, view->px_scale) + n - 1) / n == units) return l;
		}
	}
	return best;
}

void glassine_px_layout_size(const struct sway_view *view, double lx, double ly, int uw, int uh,
			     double *width, double *height)
{
	int ox, oy;
	output_origin(view, &ox, &oy);
	*width = layout_length(view, (int)lx - ox, uw);
	*height = layout_length(view, (int)ly - oy, uh);
}

bool glassine_px_needs_configure(const struct sway_view *view, double lx, double ly, double width,
				 double height)
{
	if (view->px_reconfigure) return true;
	if (view->px_n == 0) return false;
	int uw, uh;
	glassine_px_box(view, lx, ly, width, height, &uw, &uh, NULL, NULL);
	return uw != view->px_conf_width || uh != view->px_conf_height;
}
