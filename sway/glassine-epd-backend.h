/* glassine-epd-backend.h — the Glassine e-ink panel as a wlroots output (see
 * glassine-epd-backend.c, glassine-epd-output-proto.h). Built into the patched sway. */
#ifndef GLASSINE_EPD_BACKEND_H
#define GLASSINE_EPD_BACKEND_H
#include <stdbool.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/backend.h>

/* one output, Note Air 1-EPD-1: the portrait panel (GLASSINE_EPD_SIZE=WxH overrides
 * 1404x1872, for host tests); frames go to glassine-epdd-desktop over GLASSINE_EPD_SOCK */
struct wlr_backend *glassine_epd_backend_create(struct wl_event_loop *loop);
bool glassine_epd_backend_is(struct wlr_backend *backend);
/* for wlr_multi_for_each_backend(.., glassine_epd_backend_renderer, renderer),
 * after the renderer exists and before the backend starts: decides the grey
 * output (GLASSINE_EPD_GREY=1, a GLES renderer with R8 LINEAR render targets) */
void glassine_epd_backend_renderer(struct wlr_backend *backend, void *renderer);
/* the output is grey: sway renders R8 with *transform */
struct wlr_output;
bool glassine_epd_output_grey(struct wlr_output *output, enum wl_output_transform *transform);

#endif
