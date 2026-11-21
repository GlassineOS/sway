/* glassine-ink-area.h — the pen's ink areas for glassine-epdd-desktop (see glassine-ink-area.c) */
#ifndef GLASSINE_INK_AREA_H
#define GLASSINE_INK_AREA_H
#include "sway/output.h"

/* the glassine_ink_v1 global (glassine-ink-v1.xml), at server start */
void glassine_ink_init(struct wl_display *display);

/* after an output commit: send the windows' visible regions when they changed */
void glassine_ink_update(struct sway_output *output);

/* the surface's client registered ink areas on it, or writing regions in ink mode (a
 * writing surface) */
bool glassine_ink_has_areas(struct wlr_surface *s);

/* what the apps said their commits since this output's last frame are (the ink protocol's
 * set_content): 0 = nothing, else the kind (glassine_ink_surface_v1.content) and in *box
 * where, in the coordinates of the output's frame buffer. Taking it forgets it */
struct wlr_box;
uint32_t glassine_ink_content_take(struct sway_output *output, struct wlr_box *box);

/* the writing regions (glassine-handwriting.c) moved or changed: send the areas again */
void glassine_ink_changed(void);

#endif
