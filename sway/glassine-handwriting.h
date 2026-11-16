/* glassine-handwriting.h — the pen interface of handwriting (see glassine-handwriting.c) */
#ifndef GLASSINE_HANDWRITING_H
#define GLASSINE_HANDWRITING_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct glassine_hw_hooks {
	/* the sandbox's application id of a client into buf ("" when it isn't sandboxed) */
	void (*identify)(struct wl_client *client, char *buf, size_t size);
	/* a wl_region's contents, copied; whether a point (surface coordinates) is inside; free */
	void *(*region_copy)(struct wl_resource *region);
	bool (*region_contains)(void *region, double x, double y);
	void (*region_free)(void *region);
	/* the region's bounding box: x0, y0, x1, y1 (optional) */
	void (*region_box)(void *region, int box[4]);
};

/* the glassine_handwriting_manager_v1 global; service: the program started for the
 * service's side (NULL or "": raw strokes only) */
void glassine_hw_init(struct wl_display *display, const struct glassine_hw_hooks *hooks, const char *service);

/* the output's logical px per mm (the ink's units for the service) */
void glassine_hw_set_per_mm(double per_mm);

/* the tool's pressure (0..1) and tilt (degrees) as they change; NAN: unchanged */
void glassine_hw_axis(double pressure, double tilt_x, double tilt_y);

/* the tip touches surface at (sx, sy) in its coordinates, (lx, ly) in the caller's: true
 * when a handwriting object takes the stroke. Motion and up in the caller's coordinates. */
bool glassine_hw_pen_down(struct wl_resource *surface, double sx, double sy, double lx, double ly, uint32_t time);
void glassine_hw_pen_motion(double lx, double ly, uint32_t time);
void glassine_hw_pen_up(uint32_t time);
bool glassine_hw_pen_active(void);

#endif
