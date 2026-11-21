/* glassine-view-px.h — windows of whole-scale-only clients, pixel for pixel.
 *
 * On an output of fractional scale F, a client that takes only whole buffer scales
 * (it made no wp_fractional_scale_v1 object for its toplevel's surface) gets the
 * nearest whole scale N (at least 1) and is never resampled: its view lives in its
 * own coordinates, "units", one unit = N output pixels = N/F layout px. Its
 * configure sizes, geometry, size hints, popups and input positions are in units;
 * the scene shows its surfaces through a pixel tree (wlroots/0010). view->px_n is
 * N for such a view and 0 for every other one, which then takes sway's own path:
 * a client with a fractional scale object, any view on an output whose scale is
 * whole, Xwayland. */
#ifndef GLASSINE_VIEW_PX_H
#define GLASSINE_VIEW_PX_H
#include <stdbool.h>

struct sway_view;
struct sway_output;
struct wlr_surface;
struct wlr_scene_tree;
struct wlr_scene_node;

/* the nearest whole scale to an output's scale, at least 1 */
int glassine_px_whole(double scale);

/* Decide the view's path for the output it is on (NULL: none yet) and set its
 * trees and its surfaces' preferred buffer scale. True: the path changed, the
 * view needs a configure. */
bool glassine_px_update(struct sway_view *view, struct sway_output *output);

/* units per layout px: F/N, 1 for a view on sway's own path */
double glassine_px_units(const struct sway_view *view);

/* For a pixel view whose content box is width x height at layout lx, ly on its
 * output: the box's size in output pixels (dw, dh) and the size in units to
 * configure (uw, uh): the units that cover it, the last one cut when the box is
 * no multiple of N. Any pointer may be NULL. */
void glassine_px_box(const struct sway_view *view, double lx, double ly, double width, double height,
		     int *uw, int *uh, int *dw, int *dh);

/* the layout size whose box at lx, ly is configured as uw x uh units (the nearest one) */
void glassine_px_layout_size(const struct sway_view *view, double lx, double ly, int uw, int uh,
			     double *width, double *height);

/* units per layout px of the view a surface belongs to (its toplevel, a
 * subsurface or a popup of it); 1 for any other surface */
double glassine_px_surface_units(struct wlr_surface *surface);

/* units per layout px where a scene node is: below a pixel tree its units, else 1 */
double glassine_px_node_units(struct wlr_scene_node *node);

/* a popup's own tree (a child of the popup layer) follows its view's path */
void glassine_px_popup_tree(const struct sway_view *view, struct wlr_scene_tree *tree);

/* true when a configure is due though the layout size did not change: the path
 * changed, or the box moved so that its output pixels differ */
bool glassine_px_needs_configure(const struct sway_view *view, double lx, double ly, double width,
				 double height);
#endif
