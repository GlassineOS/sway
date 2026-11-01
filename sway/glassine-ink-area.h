/* glassine-ink-area.h — the pen's ink areas for glassine-epdd-desktop (see glassine-ink-area.c) */
#ifndef GLASSINE_INK_AREA_H
#define GLASSINE_INK_AREA_H
#include "sway/output.h"

/* the glassine_ink_v1 global (glassine-ink-v1.xml), at server start */
void glassine_ink_init(struct wl_display *display);

/* after an output commit: send the windows' visible regions when they changed */
void glassine_ink_update(struct sway_output *output);

#endif
