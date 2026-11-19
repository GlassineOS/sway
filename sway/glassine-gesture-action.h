/* glassine-gesture-action.h — what a touch gesture does: the action ids of the
 * gestures file and the request each one sends. Pure logic, no wlroots, so the
 * host test (test/gesture-action-test.sh) builds it alone; cursor.c finds the
 * window and runs what comes back.
 *
 * The gestures file ($HOME/.config/glassine/gestures, written by Settings > Gestures):
 * lines "FINGERS WHAT ACTION", FINGERS 3 or 4 with WHAT up/down/left/right,
 * "2 tap ACTION", or "edge EDGE ACTION" with EDGE top/bottom/left/right (the
 * edge swipes, the screen's edges as it is turned; defaults: top show-bar, the
 * others nothing); '#' starts a comment line; the last matching line wins; a
 * missing file or line: the default. Exit guard (GlassineOS/shell spec 001
 * FR-062): when no trigger (3/4-finger swipes, 2 tap, edges) maps to one of
 * unfullscreen, minimize, home, overview, show-bar, the top edge is show-bar
 * whatever its line says (glassine_ga_edge_lookup). An edge whose action is
 * show-bar is only tracked over an immersive window (glassine_ga_edge_armed).
 * Action ids:
 *   home              the shell's Home (glassine-shell home)
 *   overview          the shell's overview (glassine-shell toggle overview)
 *   quick, notices    the shell's popups
 *   show-bar          the shell's bar above an immersive window (glassine-shell bar show)
 *   screen-next, screen-prev   the next/previous screen (sway workspace)
 *   refresh           a full e-ink refresh
 *   fullscreen, unfullscreen   the window under the fingers immersive (sway's
 *                     own fullscreen, set through glassine-swayd's `full`) or
 *                     back
 *   minimize, close, swap, move-next, move-prev, page-up, page-down
 *                     the window under the fingers
 *   nothing, right-click (2 tap only)
 * Old ids are read and mapped (glassine_ga_canon), never written: launcher ->
 * home, ws-next/ws-prev -> screen-next/screen-prev, maximize -> fullscreen. */
#ifndef GLASSINE_GESTURE_ACTION_H
#define GLASSINE_GESTURE_ACTION_H
#include <stddef.h>

/* an older glassine-swayd's mark on a window it held full screen (its FULL_MARK);
 * a window may still carry it */
#define GLASSINE_GA_FULL_MARK "_glassine_full"

/* the current id for an id (an old one mapped; any other as it is) */
const char *glassine_ga_canon(const char *id);
/* the default action for FINGERS (2: WHAT "tap"; 3, 4: a direction), or NULL */
const char *glassine_ga_default(int fingers, const char *what);
/* $HOME/.config/glassine/gestures */
void glassine_ga_file(char *path, size_t len);
/* the action for "FINGERS WHAT" from the gestures file at path (NULL: the
 * default's), mapped by glassine_ga_canon; the default when there is no line */
void glassine_ga_lookup(const char *path, int fingers, const char *what, char *act, size_t len);
/* the default action of edge EDGE (top, bottom, left, right), or NULL */
const char *glassine_ga_edge_default(const char *edge);
/* the action of edge EDGE from the gestures file at path (NULL: the
 * defaults'), mapped by glassine_ga_canon, with the exit guard applied to top */
void glassine_ga_edge_lookup(const char *path, const char *edge, char *act, size_t len);
/* 1: an edge with action act (glassine_ga_edge_lookup's) is tracked, given
 * whether the output under the landing point shows an immersive window (sway's
 * fullscreen on its workspace, or a global one): never for nothing, show-bar
 * only over an immersive window (it does nothing otherwise, so it must not take
 * the apps' or the bar's touches), any other action always */
int glassine_ga_edge_armed(const char *act, int immersive);
/* 1: act is one that leaves an immersive window (the exit guard's list) */
int glassine_ga_is_exit(const char *act);
/* 1: some trigger in the file at path (or its default) runs an exit action */
int glassine_ga_has_exit(const char *path);
/* the sway command of an action that needs no window, or NULL */
const char *glassine_ga_global(const char *act);

enum glassine_ga_full {
	GLASSINE_GA_NOT_FULL_ACTION, /* not handled here: the caller's own action */
	GLASSINE_GA_NOTHING,         /* already as asked */
	GLASSINE_GA_SWAYD,           /* send buf to glassine-swayd */
	GLASSINE_GA_SWAY,            /* run buf as a sway command */
};
/* fullscreen/unfullscreen of window con_id, and minimize of a full-screen one
 * (back from full): sway_full = in sway's own fullscreen (swayd's `full` or the
 * app's own request): fullscreen leaves it as it is (swayd's `full` toggles),
 * unfullscreen and minimize are sway's `fullscreen disable`; marked = it
 * carries an older swayd's GLASSINE_GA_FULL_MARK: back through swayd's `full`.
 * Any other minimize: NOT_FULL_ACTION */
enum glassine_ga_full glassine_ga_full(const char *act, unsigned long con_id, int marked, int sway_full,
	char *buf, size_t len);
/* $XDG_RUNTIME_DIR/glassine-swayd (/tmp without it, as glassine-swayd does) */
void glassine_ga_swayd_path(char *path, size_t len);
/* msg as one datagram to the socket at path, without blocking: 0, or -1 and errno */
int glassine_ga_send(const char *path, const char *msg);

#endif
