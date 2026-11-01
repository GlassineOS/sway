/* glassine-popup-hint.h — xdg_popups as glassine-epdd-desktop popups (see glassine-popup-hint.c) */
#ifndef GLASSINE_POPUP_HINT_H
#define GLASSINE_POPUP_HINT_H
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

/* follow a new popup; tree = its xdg surface scene tree */
void glassine_popup_track(struct wlr_xdg_popup *popup, struct wlr_scene_tree *tree);

#endif
