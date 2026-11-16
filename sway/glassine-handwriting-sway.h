/* glassine-handwriting-sway.h — sway's side of the pen interface (glassine-handwriting-sway.c) */
#ifndef GLASSINE_HANDWRITING_SWAY_H
#define GLASSINE_HANDWRITING_SWAY_H
#include <stdbool.h>
#include <stdint.h>

struct sway_cursor;
struct sway_tablet_tool;
struct wlr_surface;
struct wl_display;

void glassine_hw_sway_init(struct wl_display *display);
/* the tip down at (sx, sy) on surface (NULL: none), or up */
void glassine_hw_sway_tip(struct sway_cursor *cursor, struct sway_tablet_tool *tool, struct wlr_surface *surface,
			  double sx, double sy, uint32_t time, bool down);
/* after the cursor followed the tool: its pressure (NAN unchanged) and position */
void glassine_hw_sway_axis(struct sway_cursor *cursor, struct sway_tablet_tool *tool, double pressure, uint32_t time);
/* the tool left proximity */
void glassine_hw_sway_out(uint32_t time);

#endif
