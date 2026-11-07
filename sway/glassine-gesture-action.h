/* glassine-gesture-action.h — what a touch gesture does: the action ids of the
 * gestures file and the request each one sends. Pure logic, no wlroots, so the
 * host test (test/gesture-action-test.sh) builds it alone; cursor.c finds the
 * window and runs what comes back.
 *
 * The gestures file ($HOME/.config/glassine/gestures, written by Settings > Gestures):
 * lines "FINGERS WHAT ACTION", FINGERS 3 or 4 with WHAT up/down/left/right, or
 * "2 tap ACTION"; '#' starts a comment line; the last matching line wins; a
 * missing file or line: the default. Action ids:
 *   home              the shell's Home (glassine-shell-wl home)
 *   overview          the shell's overview (glassine-shell-wl toggle overview)
 *   quick, notices    the shell's popups
 *   screen-next, screen-prev   the next/previous screen (sway workspace)
 *   refresh           a full e-ink refresh
 *   fullscreen, unfullscreen   the focused window full screen or back, through
 *                     glassine-swayd's `full` (the bar stays above it; never
 *                     sway's own fullscreen)
 *   minimize, close, swap, move-next, move-prev, page-up, page-down
 *                     the window under the fingers
 *   nothing, right-click (2 tap only)
 * Old ids are read and mapped (glassine_ga_canon), never written: launcher ->
 * home, ws-next/ws-prev -> screen-next/screen-prev, maximize -> fullscreen. */
#ifndef GLASSINE_GESTURE_ACTION_H
#define GLASSINE_GESTURE_ACTION_H
#include <stddef.h>

/* glassine-swayd's mark on a window it holds full screen (its FULL_MARK) */
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
/* the sway command of an action that needs no window, or NULL */
const char *glassine_ga_global(const char *act);

enum glassine_ga_full {
	GLASSINE_GA_NOT_FULL_ACTION, /* not fullscreen/unfullscreen */
	GLASSINE_GA_NOTHING,         /* already as asked */
	GLASSINE_GA_SWAYD,           /* send buf to glassine-swayd */
	GLASSINE_GA_SWAY,            /* run buf as a sway command */
};
/* fullscreen/unfullscreen of window con_id: marked = it carries
 * GLASSINE_GA_FULL_MARK, sway_full = in sway's own fullscreen (an app asked
 * for it; unfullscreen ends that, fullscreen turns it into swayd's) */
enum glassine_ga_full glassine_ga_full(const char *act, unsigned long con_id, int marked, int sway_full,
	char *buf, size_t len);
/* $XDG_RUNTIME_DIR/glassine-swayd (/tmp without it, as glassine-swayd does) */
void glassine_ga_swayd_path(char *path, size_t len);
/* msg as one datagram to the socket at path, without blocking: 0, or -1 and errno */
int glassine_ga_send(const char *path, const char *msg);

#endif
