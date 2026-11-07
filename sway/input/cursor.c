#include <assert.h>
#include <math.h>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <errno.h>
#include <time.h>
#include <strings.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/types/wlr_tablet_pad.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/region.h>
#include "config.h"
#include "log.h"
#include "util.h"
#include "sway/commands.h"
#include "sway/input/cursor.h"
#include "sway/input/keyboard.h"
#include "sway/input/tablet.h"
#include "sway/layers.h"
#include "sway/output.h"
#include "sway/scene_descriptor.h"
#include "sway/server.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "sway/desktop/transaction.h"
#include "glassine-gesture.h"
#include "../glassine-gesture-action.h"

/* Glassine : N-finger swipes on the touchscreen, in the compositor.
 * One finger or two go to the apps at once; the Nth finger turns the
 * sequence into a gesture: the apps' touches are cancelled, the rest of the
 * sequence is swallowed, and a swipe runs an action.
 * GLASSINE_SWIPE_FINGERS (3; 0: off), GLASSINE_SWIPE_PX (125, layout px)
 * The actions come from $HOME/.config/glassine/gestures, which
 * glassine-settings > Gestures writes and this reads at each swipe (no reload);
 * its format, the action ids, the defaults and the old ids still read:
 * glassine-gesture-action.h. FINGERS 3 or 4 (a 4th finger before the swipe
 * makes it a 4-finger one). Window actions act on the window under the
 * fingers (where the gesture began; none there: nothing):
 * fullscreen, unfullscreen through glassine-swayd's `full` (the bar stays
 * above the window; never sway's fullscreen)
 * minimize a fullscreen window (glassine-swayd's or sway's): back from
 * fullscreen; a maximized one:
 * back to its tile; else to sway's scratchpad (glassine-shell
 * lists those in its workspaces popup and brings them back)
 * close, swap (with the next tile),
 * move-next, move-prev (to the next/previous workspace, and along)
 * page-up, page-down (that key to it: wtype)
 * the others: nothing, refresh (a full e-ink refresh), screen-next,
 * screen-prev, home, overview, quick, notices (glassine-shell's requests).
 * Two-finger tap : a right click where the fingers were, for apps
 * whose menus only open on a mouse right click (sway has no long press =
 * right click as glassine-touchd on X had: a hold stays the apps'). The line
 * "2 tap ACTION" in the same file: right-click (default), nothing, or any
 * action above (window actions: the window under the fingers).
 * GLASSINE_TAP2_MS (400; 0: off), GLASSINE_TAP2_JOIN_MS (250), GLASSINE_TAP2_PX (15) */
static struct glassine_gesture gest;
static int glassine_g_state; /* 0 unset, 1 on, -1 off */
static bool glassine_g_internal; /* our own cancels: not fed back */

static uint32_t get_current_time_msec(void);
static struct wlr_touch *glassine_g_touch; /* the touchscreen (for the tap timer) */
static struct wl_event_source *glassine_g_timer;

static int glassine_g_env(const char *name, int dflt) {
	const char *v = getenv(name);
	return v && *v ? atoi(v) : dflt;
}

static bool glassine_g_on(void) {
	if (!glassine_g_state) {
		const char *px = getenv("GLASSINE_SWIPE_PX");
		int n = glassine_g_env("GLASSINE_SWIPE_FINGERS", 3), tap = glassine_g_env("GLASSINE_TAP2_MS", 400);
		glassine_g_init(&gest, n > 0 ? n : 3, px ? atof(px) : 125);
		if (n <= 0) {
			gest.fingers = GLASSINE_G_MAXPT + 1; /* no swipes: never that many fingers */
		}
		glassine_g_tap_init(&gest, tap > 0 ? tap : -1, glassine_g_env("GLASSINE_TAP2_JOIN_MS", 0), glassine_g_env("GLASSINE_TAP2_PX", 0));
		glassine_g_state = n > 0 || tap > 0 ? 1 : -1;
		sway_log(SWAY_INFO, "glassine: touch gestures: swipes %s, two-finger tap %s",
			n > 0 ? "on" : "off", tap > 0 ? "on" : "off");
	}
	return glassine_g_state > 0;
}

static const char *const glassine_g_dirs[4] = { "left", "right", "up", "down" };

/* the action for "FINGERS WHAT" (glassine-gesture-action.h) */
static void glassine_g_lookup(int fingers, const char *what, char *act, size_t len) {
	char path[512];
	glassine_ga_file(path, sizeof path);
	glassine_ga_lookup(path, fingers, what, act, len);
}

/* the action for this many fingers and direction (index into glassine_g_dirs) */
static void glassine_g_action(int fingers, int dir, char *act, size_t len) {
	glassine_g_lookup(fingers >= 4 ? 4 : 3, glassine_g_dirs[dir], act, len);
}

/* fullscreen, unfullscreen, and minimize of a window glassine-swayd holds full
 * screen: through glassine-swayd (glassine_ga_full). false: not handled here */
static bool glassine_g_full(struct sway_seat *seat, struct sway_container *con, const char *act, const char *what) {
	char buf[128], path[108];
	bool marked = container_has_mark(con, GLASSINE_GA_FULL_MARK);
	switch (glassine_ga_full(act, (unsigned long)con->node.id, marked,
			con->pending.fullscreen_mode != FULLSCREEN_NONE, buf, sizeof buf)) {
	case GLASSINE_GA_NOT_FULL_ACTION:
		return false;
	case GLASSINE_GA_SWAYD:
		if (!marked && container_is_floating(con)) {
			/* swayd's `full` looks only among the tiled windows and the ones it holds full
			 * itself (marked): for any other floating one it logs "no window" and does nothing */
			sway_log(SWAY_INFO, "glassine: %s (%s): a floating window can't be made full screen", what, act);
			return true;
		}
		glassine_ga_swayd_path(path, sizeof path);
		if (glassine_ga_send(path, buf) < 0) {
			sway_log_errno(SWAY_ERROR, "glassine: %s (%s): glassine-swayd %s", what, act, path);
		} else {
			sway_log(SWAY_DEBUG, "glassine: %s (%s): glassine-swayd %s", what, act, buf);
		}
		return true;
	case GLASSINE_GA_SWAY:;
		sway_log(SWAY_DEBUG, "glassine: %s (%s): %s", what, act, buf);
		struct sway_binding binding = { .type = BINDING_KEYSYM, .command = buf };
		seat_execute_command(seat, &binding);
		transaction_commit_dirty();
		return true;
	default:
		sway_log(SWAY_DEBUG, "glassine: %s (%s): already so", what, act);
		return true;
	}
}

/* run ACT; window actions on the window at x, y. what: for the log */
static void glassine_g_run(struct sway_seat *seat, const char *act, double x, double y, const char *what) {
	char buf[512], nbuf[128];
	const char *cmd = glassine_ga_global(act);
	if (!cmd && strcmp(act, "nothing")) {
		struct wlr_surface *surface = NULL;
		double sx, sy;
		struct sway_node *node = node_at_coords(seat, x, y, &surface, &sx, &sy);
		struct sway_container *con = node && node->type == N_CONTAINER ? node->sway_container : NULL;
		if (!con || !con->view) {
			sway_log(SWAY_DEBUG, "glassine: %s (%s) at %.0f,%.0f: no window there", what, act, x, y);
			return;
		}
		if (glassine_g_full(seat, con, act, what)) {
			return;
		}
		bool full = con->pending.fullscreen_mode != FULLSCREEN_NONE;
		bool max = container_has_mark(con, "glassine-max");
		const char *c = NULL;
		if (!strcmp(act, "minimize")) {
			/* glassine-max: the old maximize (a floating window filling the workspace) */
			c = full ? "fullscreen disable" : max ? "floating disable, unmark glassine-max" : "move scratchpad";
		} else if (!strcmp(act, "close")) {
			c = "kill";
		} else if (!strcmp(act, "move-next") || !strcmp(act, "move-prev")) {
			/* past the last workspace a new one, no wrap-around
			 * (the last one's window then went to the first); move-prev on the first:
			 * nothing. A window alone on the last one stays (a new one = the same) */
			bool next = !strcmp(act, "move-next");
			struct sway_workspace *ws = con->pending.workspace;
			struct sway_output *op = ws ? ws->output : NULL;
			int i = op ? list_find(op->workspaces, ws) : -1;
			if (i < 0) {
				c = NULL;
			} else if (next ? i < op->workspaces->length - 1 : i > 0) {
				c = next ? "move container to workspace next_on_output; workspace next_on_output"
					: "move container to workspace prev_on_output; workspace prev_on_output";
			} else if (next && workspace_num_tiling_views(ws) + (size_t)ws->floating->length > 1) {
				int maxn = 0;
				for (int o = 0; o < root->outputs->length; o++) {
					struct sway_output *x = root->outputs->items[o];
					for (int w = 0; w < x->workspaces->length; w++) {
						struct sway_workspace *y = x->workspaces->items[w];
						int n = atoi(y->name);
						maxn = n > maxn ? n : maxn;
					}
				}
				snprintf(nbuf, sizeof nbuf, "move container to workspace number %d; workspace number %d",
					maxn + 1, maxn + 1);
				c = nbuf;
			}
		} else if (!strcmp(act, "swap") && !container_is_floating(con)) {
			list_t *sib = container_get_siblings(con);
			if (sib && sib->length > 1) {
				struct sway_container *o = sib->items[(container_sibling_index(con) + 1) % sib->length];
				snprintf(buf, sizeof buf, "[con_id=%zu] swap container with con_id %zu",
					con->node.id, o->node.id);
				cmd = buf;
			}
		} else if (!strcmp(act, "page-up") || !strcmp(act, "page-down")) {
			/* a Page Up/Down key for the window under the
			 * fingers (focused first; wtype types it through a virtual keyboard) */
			c = !strcmp(act, "page-up") ? "focus; exec wtype -k Prior" : "focus; exec wtype -k Next";
		}
		if (c) {
			snprintf(buf, sizeof buf, "[con_id=%zu] %s", con->node.id, c);
			cmd = buf;
		}
	}
	if (!cmd) {
		sway_log(SWAY_DEBUG, "glassine: %s: %s, nothing to do", what, act);
		return;
	}
	sway_log(SWAY_DEBUG, "glassine: %s (%s): %s", what, act, cmd);
	struct sway_binding binding = { .type = BINDING_KEYSYM, .command = (char *)cmd };
	seat_execute_command(seat, &binding);
	transaction_commit_dirty();
}

static void glassine_g_command(struct sway_seat *seat, enum glassine_g_result r) {
	int dir = r - GLASSINE_G_SWIPE_LEFT, fingers = gest.most;
	char act[64], what[64];
	glassine_g_action(fingers, dir, act, sizeof act);
	snprintf(what, sizeof what, "%d-finger swipe %s", fingers, glassine_g_dirs[dir]);
	glassine_g_run(seat, act, gest.cx, gest.cy, what);
}

static void handle_touch_cancel(struct wl_listener *listener, void *data);
static void handle_touch_up(struct wl_listener *listener, void *data);
static void handle_touch_frame(struct wl_listener *listener, void *data);

/* an up the gesture held back, to the apps now */
static void glassine_g_up_to_apps(struct sway_cursor *cursor, struct wlr_touch *touch,
		uint32_t time_msec, int32_t id) {
	struct wlr_touch_up_event ev = { .touch = touch, .time_msec = time_msec, .touch_id = id };
	glassine_g_internal = true;
	handle_touch_up(&cursor->touch_up, &ev);
	glassine_g_internal = false;
}

/* the ups held back for a tap that did not come: to the apps, before the
 * event that told */
static void glassine_g_flush_ups(struct sway_cursor *cursor, struct wlr_touch *touch, uint32_t time_msec) {
	int32_t ids[GLASSINE_G_MAXPT];
	int n = glassine_g_flush(&gest, ids, GLASSINE_G_MAXPT);
	for (int i = 0; i < n; i++) {
		glassine_g_up_to_apps(cursor, touch, time_msec, ids[i]);
	}
	if (n) {
		sway_log(SWAY_DEBUG, "glassine: no two-finger tap, %d held-back up(s) delivered", n);
	}
}

static int glassine_g_timer_fired(void *data) {
	struct sway_cursor *cursor = data;
	glassine_g_clock(&gest, get_current_time_msec());
	if (glassine_g_touch && glassine_g_tick(&gest)) {
		glassine_g_flush_ups(cursor, glassine_g_touch, get_current_time_msec());
		handle_touch_frame(&cursor->touch_frame, NULL);
	}
	return 0;
}

/* after each touch event: a held-back up times out on its own */
static void glassine_g_timer_update(struct sway_cursor *cursor) {
	uint32_t ms = glassine_g_hold_ms(&gest);
	if (!glassine_g_timer && ms) {
		glassine_g_timer = wl_event_loop_add_timer(server.wl_event_loop, glassine_g_timer_fired, cursor);
	}
	if (glassine_g_timer) {
		wl_event_source_timer_update(glassine_g_timer, ms);
	}
}

/* the apps' sequence ends (the gesture took it); an app that only gets a
 * pointer from touch (sway simulates one) gets its left button released */
static void glassine_g_cancel_ids(struct sway_cursor *cursor, struct wlr_touch *touch,
		uint32_t time_msec, const int32_t *ids, int n) {
	if (cursor->simulating_pointer_from_touch) {
		if (!cursor->pointer_touch_up) {
			cursor->pointer_touch_up = true;
			dispatch_cursor_button(cursor, &touch->base, time_msec, BTN_LEFT,
				WL_POINTER_BUTTON_STATE_RELEASED);
		}
		return;
	}
	glassine_g_internal = true;
	for (int i = 0; i < n; i++) {
		struct wlr_touch_cancel_event ev = {
			.touch = touch, .time_msec = time_msec, .touch_id = ids[i],
		};
		handle_touch_cancel(&cursor->touch_cancel, &ev);
	}
	glassine_g_internal = false;
}

/* a two-finger tap: by default a right click at the tap */
static void glassine_g_tap(struct sway_cursor *cursor, struct wlr_touch *touch, uint32_t time_msec) {
	char act[64];
	glassine_g_lookup(2, "tap", act, sizeof act);
	if (!strcmp(act, "nothing")) {
		for (int i = 0; i < gest.ntap; i++) {
			glassine_g_up_to_apps(cursor, touch, time_msec, gest.tap_ids[i]);
		}
		sway_log(SWAY_DEBUG, "glassine: two-finger tap at %.0f,%.0f: nothing", gest.tx, gest.ty);
		return;
	}
	glassine_g_cancel_ids(cursor, touch, time_msec, gest.tap_ids, gest.ntap);
	if (strcmp(act, "right-click")) {
		glassine_g_run(cursor->seat, act, gest.tx, gest.ty, "two-finger tap");
		return;
	}
	sway_log(SWAY_DEBUG, "glassine: two-finger tap at %.0f,%.0f: right click", gest.tx, gest.ty);
	double dx = gest.tx - cursor->cursor->x, dy = gest.ty - cursor->cursor->y;
	pointer_motion(cursor, time_msec, &touch->base, dx, dy, dx, dy);
	dispatch_cursor_button(cursor, &touch->base, time_msec, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
	dispatch_cursor_button(cursor, &touch->base, time_msec, BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
	wlr_seat_pointer_notify_frame(cursor->seat->wlr_seat);
}

/* the gesture took over: cancel what the apps hold (all but the new finger) */
static void glassine_g_cancel_apps(struct sway_cursor *cursor, struct wlr_touch *touch,
		uint32_t time_msec, int32_t new_id) {
	int32_t ids[GLASSINE_G_MAXPT];
	int n = glassine_g_points(&gest, ids, GLASSINE_G_MAXPT), k = 0;
	for (int i = 0; i < n; i++) {
		if (ids[i] != new_id) {
			ids[k++] = ids[i];
		}
	}
	glassine_g_cancel_ids(cursor, touch, time_msec, ids, k);
	sway_log(SWAY_DEBUG, "glassine: %d-finger gesture, %d app touches cancelled", n, n - 1);
}

static uint32_t get_current_time_msec(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/*
 * Glassine palm rejection: no touch while the pen is in range (about 1 cm above
 * the glass) and for grace_ms after it leaves. libinput only ignores a box
 * beside the tip while the pen reports tilt, worked out in the pen's raw mm,
 * which on the Glassine misses the hand (the touchscreen has no resolution, its
 * axes are turned against the pen's, and the box assumes the raw top is the
 * user's top). Touches already down when the pen arrives are cancelled;
 * a touch that began while blocked stays ignored until it lifts, also after
 * the pen has left. /etc/glassine/pen-palm (read at the first use): "off", or
 * "grace_ms N" (default 250). A pen that stops reporting for 3 s without a
 * proximity out no longer blocks (a lost event must not leave touch dead).
 */
#define PENBLOCK_MAX 16
static int penblock_state; /* 0 unread, 1 on, -1 off */
static uint32_t penblock_grace = 250;
static int penblock_tools; /* tools in proximity */
static uint32_t penblock_last, penblock_until; /* last tool event; blocked until */
static int32_t penblock_down[PENBLOCK_MAX]; /* touches the session got */
static int32_t penblock_held[PENBLOCK_MAX]; /* touches we swallow until up */
static int penblock_ndown, penblock_nheld;
static struct wlr_touch *penblock_touch;

static bool penblock_on(void) {
	if (!penblock_state) {
		penblock_state = 1;
		FILE *f = fopen("/etc/glassine/pen-palm", "r");
		char line[64];
		unsigned v;
		while (f && fgets(line, sizeof line, f)) {
			if (!strncmp(line, "off", 3)) {
				penblock_state = -1;
			} else if (sscanf(line, "grace_ms %u", &v) == 1) {
				penblock_grace = v;
			}
		}
		if (f) {
			fclose(f);
		}
		sway_log(SWAY_INFO, "glassine: pen palm rejection %s, grace %u ms",
			penblock_state > 0 ? "on" : "off", penblock_grace);
	}
	return penblock_state > 0;
}

static bool penblock_has(int32_t *ids, int n, int32_t id) {
	for (int i = 0; i < n; i++) {
		if (ids[i] == id) {
			return true;
		}
	}
	return false;
}

static void penblock_add(int32_t *ids, int *n, int32_t id) {
	if (!penblock_has(ids, *n, id) && *n < PENBLOCK_MAX) {
		ids[(*n)++] = id;
	}
}

static void penblock_del(int32_t *ids, int *n, int32_t id) {
	for (int i = 0; i < *n; i++) {
		if (ids[i] == id) {
			ids[i] = ids[--(*n)];
			return;
		}
	}
}

static bool penblock_blocked(void) {
	uint32_t now = get_current_time_msec();
	if (penblock_tools > 0 && now - penblock_last > 3000) {
		sway_log(SWAY_DEBUG, "glassine: pen silent 3 s in proximity, touch unblocked");
		penblock_tools = 0;
		penblock_until = now;
	}
	return penblock_tools > 0 || (int32_t)(penblock_until - now) > 0;
}

/* a touch event for id: swallow it? (a new touch while blocked is held) */
static bool penblock_swallow(int32_t id, bool down, bool up) {
	if (!penblock_on()) {
		return false;
	}
	if (penblock_has(penblock_held, penblock_nheld, id)) {
		if (up) {
			penblock_del(penblock_held, &penblock_nheld, id);
		}
		return true;
	}
	if (down && penblock_blocked()) {
		penblock_add(penblock_held, &penblock_nheld, id);
		sway_log(SWAY_DEBUG, "glassine: touch %d ignored, pen in range", id);
		return true;
	}
	if (down) {
		penblock_add(penblock_down, &penblock_ndown, id);
	} else if (up) {
		penblock_del(penblock_down, &penblock_ndown, id);
	}
	return false;
}

static void penblock_tool_event(void) {
	penblock_last = get_current_time_msec();
}

/* the pen came into range: cancel the session's touches, hold them to the lift */
static void penblock_pen_in(struct sway_cursor *cursor, uint32_t time_msec) {
	if (!penblock_on()) {
		return;
	}
	penblock_tools++;
	penblock_tool_event();
	int32_t ids[PENBLOCK_MAX];
	int n = penblock_ndown;
	memcpy(ids, penblock_down, sizeof ids);
	for (int i = 0; i < n && penblock_touch; i++) {
		struct wlr_touch_cancel_event ev = {
			.touch = penblock_touch, .time_msec = time_msec, .touch_id = ids[i],
		};
		handle_touch_cancel(&cursor->touch_cancel, &ev);
		penblock_add(penblock_held, &penblock_nheld, ids[i]);
	}
	if (n) {
		sway_log(SWAY_DEBUG, "glassine: pen in range, %d touches cancelled", n);
	}
}

static void penblock_pen_out(void) {
	if (!penblock_on()) {
		return;
	}
	if (penblock_tools > 0) {
		penblock_tools--;
	}
	penblock_until = get_current_time_msec() + penblock_grace;
}

/**
 * Returns the node at the cursor's position. If there is a surface at that
 * location, it is stored in **surface (it may not be a view).
 */
struct sway_node *node_at_coords(
		struct sway_seat *seat, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *scene_node = NULL;

	struct wlr_scene_node *node;
	wl_list_for_each_reverse(node, &root->layer_tree->children, link) {
		struct wlr_scene_tree *layer = wlr_scene_tree_from_node(node);

		bool non_interactive = scene_descriptor_try_get(&layer->node,
			SWAY_SCENE_DESC_NON_INTERACTIVE);
		if (non_interactive) {
			continue;
		}

		scene_node = wlr_scene_node_at(&layer->node, lx, ly, sx, sy);
		if (scene_node) {
			break;
		}
	}

	if (scene_node) {
		// determine what wlr_surface we clicked on
		if (scene_node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *scene_buffer =
				wlr_scene_buffer_from_node(scene_node);
			struct wlr_scene_surface *scene_surface =
				wlr_scene_surface_try_from_buffer(scene_buffer);

			if (scene_surface) {
				*surface = scene_surface->surface;
			}
		}

		// determine what container we clicked on
		struct wlr_scene_node *current = scene_node;
		while (true) {
			struct sway_container *con = scene_descriptor_try_get(current,
				SWAY_SCENE_DESC_CONTAINER);

			if (!con) {
				struct sway_view *view = scene_descriptor_try_get(current,
					SWAY_SCENE_DESC_VIEW);
				if (view) {
					con = view->container;
				}
			}

			if (!con) {
				struct sway_popup_desc *popup =
					scene_descriptor_try_get(current, SWAY_SCENE_DESC_POPUP);
				if (popup && popup->view) {
					con = popup->view->container;
				}
			}

			if (con && (!con->view || con->view->surface)) {
				return &con->node;
			}

			if (scene_descriptor_try_get(current, SWAY_SCENE_DESC_LAYER_SHELL)) {
				// We don't want to feed through the current workspace on
				// layer shells
				return NULL;
			}

#if WLR_HAS_XWAYLAND
			if (scene_descriptor_try_get(current, SWAY_SCENE_DESC_XWAYLAND_UNMANAGED)) {
				return NULL;
			}
#endif

			if (!current->parent) {
				break;
			}

			current = &current->parent->node;
		}
	}

	// if we aren't on a container, determine what workspace we are on
	struct wlr_output *wlr_output = wlr_output_layout_output_at(
			root->output_layout, lx, ly);
	if (wlr_output == NULL) {
		return NULL;
	}

	struct sway_output *output = wlr_output->data;
	if (!output || !output->enabled) {
		// output is being destroyed or is being enabled
		return NULL;
	}

	struct sway_workspace *ws = output_get_active_workspace(output);
	if (!ws) {
		return NULL;
	}

	return &ws->node;
}

void cursor_rebase(struct sway_cursor *cursor) {
	uint32_t time_msec = get_current_time_msec();
	seatop_rebase(cursor->seat, time_msec);
}

void cursor_rebase_all(void) {
	if (!root->outputs->length) {
		return;
	}

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		cursor_rebase(seat->cursor);
	}
}

void cursor_update_image(struct sway_cursor *cursor,
		struct sway_node *node) {
	if (node && node->type == N_CONTAINER) {
		// Try a node's resize edge
		enum wlr_edges edge = find_resize_edge(node->sway_container, NULL, cursor);
		if (edge == WLR_EDGE_NONE) {
			cursor_set_image(cursor, "default", NULL);
		} else if (container_is_floating(node->sway_container)) {
			cursor_set_image(cursor, wlr_xcursor_get_resize_name(edge), NULL);
		} else {
			if (edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
				cursor_set_image(cursor, "col-resize", NULL);
			} else {
				cursor_set_image(cursor, "row-resize", NULL);
			}
		}
	} else {
		cursor_set_image(cursor, "default", NULL);
	}
}

static void cursor_hide(struct sway_cursor *cursor) {
	wlr_cursor_unset_image(cursor->cursor);
	cursor->hidden = true;
	wlr_seat_pointer_notify_clear_focus(cursor->seat->wlr_seat);
}

static int hide_notify(void *data) {
	struct sway_cursor *cursor = data;
	cursor_hide(cursor);
	return 1;
}

/*
 * Glassine : the pointer shows on the e-ink panel (glassine-epdd-desktop paints the
 * backend's cursor), so it must not follow the pen or a finger: tablet tool
 * and pad activity never unhide it (as touch upstream) and hide a visible one
 * at once (only the image: the pointer focus is the pen's emulated pointer,
 * left to the hide timer, which clears it as before). The timer is chosen by
 * what was used last: after touch or the pen GLASSINE_HIDE_SHORT_MS (the old seat
 * hide_cursor 500: no hover left where the pen was), after the
 * mouse the seat's hide_cursor. Keyboard and switch activity reach this only
 * from seat_apply_config (every device, at start and reload): they neither
 * unhide nor pick the timer (a power key is a keyboard: the pointer would
 * show for the seat's timeout at every sway start).
 */
#define GLASSINE_HIDE_SHORT_MS 500

int cursor_get_timeout(struct sway_cursor *cursor) {
	if (cursor->pressed_button_count > 0) {
		// Do not hide cursor unless all buttons are released
		return 0;
	}
	if (cursor->glassine_short_hide) {
		return GLASSINE_HIDE_SHORT_MS;
	}

	struct seat_config *sc = seat_get_config(cursor->seat);
	if (!sc) {
		sc = seat_get_config_by_name("*");
	}
	int timeout = sc ? sc->hide_cursor_timeout : 0;
	if (timeout < 0) {
		timeout = 0;
	}
	return timeout;
}

void cursor_notify_key_press(struct sway_cursor *cursor) {
	if (cursor->hidden) {
		return;
	}

	if (cursor->hide_when_typing == HIDE_WHEN_TYPING_DEFAULT) {
		// No cached value, need to lookup in the seat_config
		const struct seat_config *seat_config = seat_get_config(cursor->seat);
		if (!seat_config) {
			seat_config = seat_get_config_by_name("*");
			if (!seat_config) {
				return;
			}
		}
		cursor->hide_when_typing = seat_config->hide_cursor_when_typing;
		// The default is currently disabled
		if (cursor->hide_when_typing == HIDE_WHEN_TYPING_DEFAULT) {
			cursor->hide_when_typing = HIDE_WHEN_TYPING_DISABLE;
		}
	}

	if (cursor->hide_when_typing == HIDE_WHEN_TYPING_ENABLE) {
		cursor_hide(cursor);
	}
}

static enum sway_input_idle_source idle_source_from_device(
		struct wlr_input_device *device) {
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		return IDLE_SOURCE_KEYBOARD;
	case WLR_INPUT_DEVICE_POINTER:
		return IDLE_SOURCE_POINTER;
	case WLR_INPUT_DEVICE_TOUCH:
		return IDLE_SOURCE_TOUCH;
	case WLR_INPUT_DEVICE_TABLET:
		return IDLE_SOURCE_TABLET_TOOL;
	case WLR_INPUT_DEVICE_TABLET_PAD:
		return IDLE_SOURCE_TABLET_PAD;
	case WLR_INPUT_DEVICE_SWITCH:
		return IDLE_SOURCE_SWITCH;
	}

	abort();
}

void cursor_handle_activity_from_idle_source(struct sway_cursor *cursor,
		enum sway_input_idle_source idle_source) {
	bool pen = idle_source == IDLE_SOURCE_TABLET_TOOL ||
		idle_source == IDLE_SOURCE_TABLET_PAD;
	if (pen || idle_source == IDLE_SOURCE_TOUCH) {
		cursor->glassine_short_hide = true;
	} else if (idle_source == IDLE_SOURCE_POINTER) {
		cursor->glassine_short_hide = false;
	}
	wl_event_source_timer_update(
			cursor->hide_source, cursor_get_timeout(cursor));

	seat_idle_notify_activity(cursor->seat, idle_source);
	if (idle_source == IDLE_SOURCE_POINTER) {
		cursor_unhide(cursor);
	} else if (pen && !cursor->hidden) {
		wlr_cursor_unset_image(cursor->cursor);
		cursor->hidden = true;
	}
}

void cursor_handle_activity_from_device(struct sway_cursor *cursor,
		struct wlr_input_device *device) {
	enum sway_input_idle_source idle_source = idle_source_from_device(device);
	cursor_handle_activity_from_idle_source(cursor, idle_source);
}

void cursor_unhide(struct sway_cursor *cursor) {
	if (!cursor->hidden) {
		return;
	}

	cursor->hidden = false;
	if (cursor->image_surface) {
		cursor_set_image_surface(cursor,
				cursor->image_surface,
				cursor->hotspot_x,
				cursor->hotspot_y,
				cursor->image_client);
	} else {
		const char *image = cursor->image;
		cursor->image = NULL;
		cursor_set_image(cursor, image, cursor->image_client);
	}
	cursor_rebase(cursor);
	wl_event_source_timer_update(cursor->hide_source, cursor_get_timeout(cursor));
}

void pointer_motion(struct sway_cursor *cursor, uint32_t time_msec,
		struct wlr_input_device *device, double dx, double dy,
		double dx_unaccel, double dy_unaccel) {
	wlr_relative_pointer_manager_v1_send_relative_motion(
		server.relative_pointer_manager,
		cursor->seat->wlr_seat, (uint64_t)time_msec * 1000,
		dx, dy, dx_unaccel, dy_unaccel);

	// Only apply pointer constraints to real pointer input.
	if (cursor->active_constraint && device->type == WLR_INPUT_DEVICE_POINTER) {
		struct wlr_surface *surface = NULL;
		double sx, sy;
		node_at_coords(cursor->seat,
			cursor->cursor->x, cursor->cursor->y, &surface, &sx, &sy);

		if (cursor->active_constraint->surface != surface) {
			return;
		}

		double sx_confined, sy_confined;
		if (!wlr_region_confine(&cursor->confine, sx, sy, sx + dx, sy + dy,
				&sx_confined, &sy_confined)) {
			return;
		}

		dx = sx_confined - sx;
		dy = sy_confined - sy;
	}

	wlr_cursor_move(cursor->cursor, device, dx, dy);

	seatop_pointer_motion(cursor->seat, time_msec);
}

static void handle_pointer_motion_relative(
		struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(listener, cursor, motion);
	struct wlr_pointer_motion_event *e = data;
	cursor_handle_activity_from_device(cursor, &e->pointer->base);

	pointer_motion(cursor, e->time_msec, &e->pointer->base, e->delta_x,
		e->delta_y, e->unaccel_dx, e->unaccel_dy);
}

static void handle_pointer_motion_absolute(
		struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor =
		wl_container_of(listener, cursor, motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->pointer->base);

	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(cursor->cursor, &event->pointer->base,
			event->x, event->y, &lx, &ly);

	double dx = lx - cursor->cursor->x;
	double dy = ly - cursor->cursor->y;

	pointer_motion(cursor, event->time_msec, &event->pointer->base, dx, dy,
		dx, dy);
}

void dispatch_cursor_button(struct sway_cursor *cursor,
		struct wlr_input_device *device, uint32_t time_msec, uint32_t button,
		enum wl_pointer_button_state state) {
	if (time_msec == 0) {
		time_msec = get_current_time_msec();
	}

	seatop_button(cursor->seat, time_msec, device, button, state);
}

static void handle_pointer_button(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(listener, cursor, button);
	struct wlr_pointer_button_event *event = data;

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		cursor->pressed_button_count++;
	} else {
		if (cursor->pressed_button_count > 0) {
			cursor->pressed_button_count--;
		} else {
			sway_log(SWAY_ERROR, "Pressed button count was wrong");
		}
	}

	cursor_handle_activity_from_device(cursor, &event->pointer->base);
	dispatch_cursor_button(cursor, &event->pointer->base,
			event->time_msec, event->button, event->state);
}

void dispatch_cursor_axis(struct sway_cursor *cursor,
		struct wlr_pointer_axis_event *event) {
	seatop_pointer_axis(cursor->seat, event);
}

static void handle_pointer_axis(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(listener, cursor, axis);
	struct wlr_pointer_axis_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->pointer->base);
	dispatch_cursor_axis(cursor, event);
}

static void handle_pointer_frame(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(listener, cursor, frame);
	wlr_seat_pointer_notify_frame(cursor->seat->wlr_seat);
}

static void handle_touch_down(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(listener, cursor, touch_down);
	struct wlr_touch_down_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->touch->base);
	penblock_touch = event->touch;
	if (penblock_swallow(event->touch_id, true, false)) {
		return;
	}
	cursor_hide(cursor);

	struct sway_seat *seat = cursor->seat;

	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(cursor->cursor, &event->touch->base,
			event->x, event->y, &lx, &ly);

	if (glassine_g_on()) {
		glassine_g_touch = event->touch;
		glassine_g_clock(&gest, event->time_msec);
		enum glassine_g_result r = glassine_g_down(&gest, event->touch_id, lx, ly);
		glassine_g_flush_ups(cursor, event->touch, event->time_msec);
		glassine_g_timer_update(cursor);
		if (r == GLASSINE_G_CANCEL) {
			glassine_g_cancel_apps(cursor, event->touch, event->time_msec, event->touch_id);
		}
		if (r != GLASSINE_G_PASS) {
			return;
		}
	}

	seat->touch_id = event->touch_id;
	seat->touch_x = lx;
	seat->touch_y = ly;

	seatop_touch_down(seat, event, lx, ly);
}

static void handle_touch_up(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(listener, cursor, touch_up);
	struct wlr_touch_up_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->touch->base);
	if (penblock_swallow(event->touch_id, false, true)) {
		return;
	}

	struct sway_seat *seat = cursor->seat;

	if (!glassine_g_internal && glassine_g_on()) {
		glassine_g_clock(&gest, event->time_msec);
		enum glassine_g_result r = glassine_g_up(&gest, event->touch_id);
		glassine_g_flush_ups(cursor, event->touch, event->time_msec);
		glassine_g_timer_update(cursor);
		if (r == GLASSINE_G_TAP2) {
			glassine_g_tap(cursor, event->touch, event->time_msec);
		}
		if (r != GLASSINE_G_PASS) {
			return;
		}
	}

	if (cursor->simulating_pointer_from_touch) {
		if (cursor->pointer_touch_id == cursor->seat->touch_id) {
			cursor->pointer_touch_up = true;
			dispatch_cursor_button(cursor, &event->touch->base,
				event->time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
		}
	} else {
		seatop_touch_up(seat, event);
	}
}

static void handle_touch_cancel(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(listener, cursor, touch_cancel);
	struct wlr_touch_cancel_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->touch->base);
	if (penblock_swallow(event->touch_id, false, true)) {
		return;
	}

	struct sway_seat *seat = cursor->seat;

	if (!glassine_g_internal && glassine_g_on()) {
		glassine_g_clock(&gest, event->time_msec);
		enum glassine_g_result r = glassine_g_cancel(&gest, event->touch_id);
		glassine_g_flush_ups(cursor, event->touch, event->time_msec);
		glassine_g_timer_update(cursor);
		if (r != GLASSINE_G_PASS) {
			return;
		}
	}

	if (cursor->simulating_pointer_from_touch) {
		if (cursor->pointer_touch_id == cursor->seat->touch_id) {
			cursor->pointer_touch_up = true;
			dispatch_cursor_button(cursor, &event->touch->base,
				event->time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
		}
	} else {
		seatop_touch_cancel(seat, event);
	}
}

static void handle_touch_motion(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor =
		wl_container_of(listener, cursor, touch_motion);
	struct wlr_touch_motion_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->touch->base);
	if (penblock_swallow(event->touch_id, false, false)) {
		return;
	}

	struct sway_seat *seat = cursor->seat;

	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(cursor->cursor, &event->touch->base,
			event->x, event->y, &lx, &ly);

	if (glassine_g_on()) {
		glassine_g_clock(&gest, event->time_msec);
		enum glassine_g_result r = glassine_g_motion(&gest, event->touch_id, lx, ly);
		glassine_g_flush_ups(cursor, event->touch, event->time_msec);
		glassine_g_timer_update(cursor);
		if (r >= GLASSINE_G_SWIPE_LEFT && r <= GLASSINE_G_SWIPE_DOWN) {
			glassine_g_command(seat, r);
		}
		if (r != GLASSINE_G_PASS) {
			return;
		}
	}

	if (seat->touch_id == event->touch_id) {
		seat->touch_x = lx;
		seat->touch_y = ly;

		drag_icons_update_position(seat);
	}

	if (cursor->simulating_pointer_from_touch) {
		if (seat->touch_id == cursor->pointer_touch_id) {
			double dx, dy;
			dx = lx - cursor->cursor->x;
			dy = ly - cursor->cursor->y;
			pointer_motion(cursor, event->time_msec, &event->touch->base,
				dx, dy, dx, dy);
		}
	} else {
		seatop_touch_motion(seat, event, lx, ly);
	}
}

static void handle_touch_frame(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor =
		wl_container_of(listener, cursor, touch_frame);

	struct wlr_seat *wlr_seat = cursor->seat->wlr_seat;

	if (cursor->simulating_pointer_from_touch) {
		wlr_seat_pointer_notify_frame(wlr_seat);

		if (cursor->pointer_touch_up) {
			cursor->pointer_touch_up = false;
			cursor->simulating_pointer_from_touch = false;
		}
	} else {
		wlr_seat_touch_notify_frame(wlr_seat);
	}
}

static double apply_mapping_from_coord(double low, double high, double value) {
	if (isnan(value)) {
		return value;
	}

	return (value - low) / (high - low);
}

static void apply_mapping_from_region(struct wlr_input_device *device,
		struct input_config_mapped_from_region *region, double *x, double *y) {
	double x1 = region->x1, x2 = region->x2;
	double y1 = region->y1, y2 = region->y2;

	if (region->mm && device->type == WLR_INPUT_DEVICE_TABLET) {
		struct wlr_tablet *tablet = wlr_tablet_from_input_device(device);
		if (tablet->width_mm == 0 || tablet->height_mm == 0) {
			return;
		}
		x1 /= tablet->width_mm;
		x2 /= tablet->width_mm;
		y1 /= tablet->height_mm;
		y2 /= tablet->height_mm;
	}

	*x = apply_mapping_from_coord(x1, x2, *x);
	*y = apply_mapping_from_coord(y1, y2, *y);
}

static void handle_tablet_tool_position(struct sway_cursor *cursor,
		struct sway_tablet_tool *tool,
		bool change_x, bool change_y,
		double x, double y, double dx, double dy,
		int32_t time_msec) {

	if (!change_x && !change_y) {
		return;
	}

	struct sway_tablet *tablet = tool->tablet;
	struct sway_input_device *input_device = tablet->seat_device->input_device;
	struct input_config *ic = input_device_get_config(input_device);
	if (ic != NULL && ic->mapped_from_region != NULL) {
		apply_mapping_from_region(input_device->wlr_device,
			ic->mapped_from_region, &x, &y);
	}

	switch (tool->mode) {
	case SWAY_TABLET_TOOL_MODE_ABSOLUTE:
		wlr_cursor_warp_absolute(cursor->cursor, input_device->wlr_device,
			change_x ? x : NAN, change_y ? y : NAN);
		break;
	case SWAY_TABLET_TOOL_MODE_RELATIVE:
		wlr_cursor_move(cursor->cursor, input_device->wlr_device, dx, dy);
		break;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct sway_seat *seat = cursor->seat;
	node_at_coords(seat, cursor->cursor->x, cursor->cursor->y, &surface, &sx, &sy);

	// The logic for whether we should send a tablet event or an emulated pointer
	// event is tricky. It comes down to:
	// * If we began a drag on a non-tablet surface (simulating_pointer_from_tool_tip),
	//   then we should continue sending emulated pointer events regardless of
	//   whether the surface currently under us accepts tablet or not.
	// * Otherwise, if we are over a surface that accepts tablet, then we should
	//   send tablet events.
	// * If we began a drag over a tablet surface, we should continue sending
	//   tablet events until the drag is released, even if we are now over a
	//   non-tablet surface.
	if (!cursor->simulating_pointer_from_tool_tip &&
			((surface && wlr_surface_accepts_tablet_v2(tablet->tablet_v2, surface)) ||
				wlr_tablet_tool_v2_has_implicit_grab(tool->tablet_v2_tool))) {
		seatop_tablet_tool_motion(seat, tool, time_msec);
	} else {
		wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->tablet_v2_tool);
		pointer_motion(cursor, time_msec, input_device->wlr_device, dx, dy, dx, dy);
	}
}

static void handle_tool_axis(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(listener, cursor, tool_axis);
	struct wlr_tablet_tool_axis_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->tablet->base);
	penblock_tool_event();

	struct sway_tablet_tool *sway_tool = event->tool->data;
	if (!sway_tool) {
		sway_log(SWAY_DEBUG, "tool axis before proximity");
		return;
	}

	handle_tablet_tool_position(cursor, sway_tool,
		event->updated_axes & WLR_TABLET_TOOL_AXIS_X,
		event->updated_axes & WLR_TABLET_TOOL_AXIS_Y,
		event->x, event->y, event->dx, event->dy, event->time_msec);

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE) {
		wlr_tablet_v2_tablet_tool_notify_pressure(
			sway_tool->tablet_v2_tool, event->pressure);
	}

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE) {
		wlr_tablet_v2_tablet_tool_notify_distance(
			sway_tool->tablet_v2_tool, event->distance);
	}

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_X) {
		sway_tool->tilt_x = event->tilt_x;
	}

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_TILT_Y) {
		sway_tool->tilt_y = event->tilt_y;
	}

	if (event->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y)) {
		wlr_tablet_v2_tablet_tool_notify_tilt(
			sway_tool->tablet_v2_tool,
			sway_tool->tilt_x, sway_tool->tilt_y);
	}

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION) {
		wlr_tablet_v2_tablet_tool_notify_rotation(
			sway_tool->tablet_v2_tool, event->rotation);
	}

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER) {
		wlr_tablet_v2_tablet_tool_notify_slider(
			sway_tool->tablet_v2_tool, event->slider);
	}

	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL) {
		wlr_tablet_v2_tablet_tool_notify_wheel(
			sway_tool->tablet_v2_tool, event->wheel_delta, 0);
	}
}

static void handle_tool_tip(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(listener, cursor, tool_tip);
	struct wlr_tablet_tool_tip_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->tablet->base);
	penblock_tool_event();

	struct sway_tablet_tool *sway_tool = event->tool->data;
	struct wlr_tablet_v2_tablet *tablet_v2 = sway_tool->tablet->tablet_v2;
	struct sway_seat *seat = cursor->seat;


	double sx, sy;
	struct wlr_surface *surface = NULL;
	node_at_coords(seat, cursor->cursor->x, cursor->cursor->y,
		&surface, &sx, &sy);

	if (cursor->simulating_pointer_from_tool_tip &&
			event->state == WLR_TABLET_TOOL_TIP_UP) {
		cursor->simulating_pointer_from_tool_tip = false;
		dispatch_cursor_button(cursor, &event->tablet->base, event->time_msec,
			BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
		wlr_seat_pointer_notify_frame(cursor->seat->wlr_seat);
	} else if (!surface || !wlr_surface_accepts_tablet_v2(tablet_v2, surface)) {
		// If we started holding the tool tip down on a surface that accepts
		// tablet v2, we should notify that surface if it gets released over a
		// surface that doesn't support v2.
		if (event->state == WLR_TABLET_TOOL_TIP_UP) {
			seatop_tablet_tool_tip(seat, sway_tool, event->time_msec,
				WLR_TABLET_TOOL_TIP_UP);
		} else {
			cursor->simulating_pointer_from_tool_tip = true;
			dispatch_cursor_button(cursor, &event->tablet->base,
				event->time_msec, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
			wlr_seat_pointer_notify_frame(cursor->seat->wlr_seat);
		}
	} else {
		seatop_tablet_tool_tip(seat, sway_tool, event->time_msec, event->state);
	}
}

static struct sway_tablet *get_tablet_for_device(struct sway_cursor *cursor,
		struct wlr_input_device *device) {
	struct sway_tablet *tablet;
	wl_list_for_each(tablet, &cursor->tablets, link) {
		if (tablet->seat_device->input_device->wlr_device == device) {
			return tablet;
		}
	}
	return NULL;
}

static void handle_tool_proximity(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor =
		wl_container_of(listener, cursor, tool_proximity);
	struct wlr_tablet_tool_proximity_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->tablet->base);

	struct wlr_tablet_tool *tool = event->tool;
	if (!tool->data) {
		struct sway_tablet *tablet = get_tablet_for_device(cursor,
			&event->tablet->base);
		if (!tablet) {
			sway_log(SWAY_ERROR, "no tablet for tablet tool");
			return;
		}
		sway_tablet_tool_configure(tablet, tool);
	}

	struct sway_tablet_tool *sway_tool = tool->data;
	if (!sway_tool) {
		sway_log(SWAY_ERROR, "tablet tool not initialized");
		return;
	}

	if (event->state == WLR_TABLET_TOOL_PROXIMITY_OUT) {
		penblock_pen_out();
		wlr_tablet_v2_tablet_tool_notify_proximity_out(sway_tool->tablet_v2_tool);
		return;
	}
	penblock_pen_in(cursor, event->time_msec);

	handle_tablet_tool_position(cursor, sway_tool, true, true, event->x, event->y,
		0, 0, event->time_msec);
}

static void handle_tool_button(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(listener, cursor, tool_button);
	struct wlr_tablet_tool_button_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->tablet->base);

	struct sway_tablet_tool *sway_tool = event->tool->data;
	if (!sway_tool) {
		sway_log(SWAY_DEBUG, "tool button before proximity");
		return;
	}
	struct wlr_tablet_v2_tablet *tablet_v2 = sway_tool->tablet->tablet_v2;

	double sx, sy;
	struct wlr_surface *surface = NULL;

	node_at_coords(cursor->seat, cursor->cursor->x, cursor->cursor->y,
		&surface, &sx, &sy);

	// TODO: floating resize should support graphics tablet events
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(cursor->seat->wlr_seat);
	uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
	bool mod_pressed = modifiers & config->floating_mod;

	bool surface_supports_tablet_events =
		surface && wlr_surface_accepts_tablet_v2(tablet_v2, surface);

	// Simulate pointer when:
	// 1. The modifier key is pressed, OR
	// 2. The surface under the cursor does not support tablet events.
	bool should_simulate_pointer = mod_pressed || !surface_supports_tablet_events;

	// Similar to tool tip, we need to selectively simulate mouse events, but we
	// want to make sure that it is always consistent. Because all tool buttons
	// currently map to BTN_RIGHT, we need to keep count of how many tool
	// buttons are currently pressed down so we can send consistent events.
	//
	// The logic follows:
	// - If we are already simulating the pointer, we should continue to do so
	//   until at least no tool button is held down.
	// - If we should simulate the pointer and no tool button is currently held
	//   down, begin simulating the pointer.
	// - If neither of the above are true, send the tablet events.
	if ((cursor->tool_buttons > 0 && cursor->simulating_pointer_from_tool_button)
		|| (cursor->tool_buttons == 0 && should_simulate_pointer)) {
		cursor->simulating_pointer_from_tool_button = true;

		// TODO: the user may want to configure which tool buttons are mapped to
		// which simulated pointer buttons
		switch (event->state) {
		case WLR_BUTTON_PRESSED:
			if (cursor->tool_buttons == 0) {
				dispatch_cursor_button(cursor, &event->tablet->base,
					event->time_msec, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
			}
			break;
		case WLR_BUTTON_RELEASED:
			if (cursor->tool_buttons <= 1) {
				dispatch_cursor_button(cursor, &event->tablet->base,
					event->time_msec, BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
			}
			break;
		}
		wlr_seat_pointer_notify_frame(cursor->seat->wlr_seat);
	} else {
		cursor->simulating_pointer_from_tool_button = false;

		wlr_tablet_v2_tablet_tool_notify_button(sway_tool->tablet_v2_tool,
			event->button, (enum zwp_tablet_pad_v2_button_state)event->state);
	}

	// Update tool button count.
	switch (event->state) {
	case WLR_BUTTON_PRESSED:
		cursor->tool_buttons++;
		break;
	case WLR_BUTTON_RELEASED:
		if (cursor->tool_buttons == 0) {
			sway_log(SWAY_ERROR, "inconsistent tablet tool button events");
		} else {
			cursor->tool_buttons--;
		}
		break;
	}
}

static void check_constraint_region(struct sway_cursor *cursor) {
	struct wlr_pointer_constraint_v1 *constraint = cursor->active_constraint;
	pixman_region32_t *region = &constraint->region;
	struct sway_view *view = view_from_wlr_surface(constraint->surface);
	if (cursor->active_confine_requires_warp && view) {
		cursor->active_confine_requires_warp = false;

		struct sway_container *con = view->container;

		double sx = cursor->cursor->x - con->pending.content_x + view->geometry.x;
		double sy = cursor->cursor->y - con->pending.content_y + view->geometry.y;

		if (!pixman_region32_contains_point(region,
				floor(sx), floor(sy), NULL)) {
			int nboxes;
			pixman_box32_t *boxes = pixman_region32_rectangles(region, &nboxes);
			if (nboxes > 0) {
				double sx = (boxes[0].x1 + boxes[0].x2) / 2.;
				double sy = (boxes[0].y1 + boxes[0].y2) / 2.;

				wlr_cursor_warp_closest(cursor->cursor, NULL,
					sx + con->pending.content_x - view->geometry.x,
					sy + con->pending.content_y - view->geometry.y);

				cursor_rebase(cursor);
			}
		}
	}

	// A locked pointer will result in an empty region, thus disallowing all movement
	if (constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
		pixman_region32_copy(&cursor->confine, region);
	} else {
		pixman_region32_clear(&cursor->confine);
	}
}

static void handle_constraint_commit(struct wl_listener *listener,
		void *data) {
	struct sway_cursor *cursor =
		wl_container_of(listener, cursor, constraint_commit);
	struct wlr_pointer_constraint_v1 *constraint = cursor->active_constraint;
	assert(constraint->surface == data);

	check_constraint_region(cursor);
}

static void handle_pointer_constraint_set_region(struct wl_listener *listener,
		void *data) {
	struct sway_pointer_constraint *sway_constraint =
		wl_container_of(listener, sway_constraint, set_region);
	struct sway_cursor *cursor = sway_constraint->cursor;

	cursor->active_confine_requires_warp = true;
}

static void handle_request_pointer_set_cursor(struct wl_listener *listener,
		void *data) {
	struct sway_cursor *cursor =
		wl_container_of(listener, cursor, request_set_cursor);
	if (!seatop_allows_set_cursor(cursor->seat)) {
		return;
	}
	struct wlr_seat_pointer_request_set_cursor_event *event = data;

	struct wl_client *focused_client = NULL;
	struct wlr_surface *focused_surface =
		cursor->seat->wlr_seat->pointer_state.focused_surface;
	if (focused_surface != NULL) {
		focused_client = wl_resource_get_client(focused_surface->resource);
	}

	// TODO: check cursor mode
	if (focused_client == NULL ||
			event->seat_client->client != focused_client) {
		sway_log(SWAY_DEBUG, "denying request to set cursor from unfocused client");
		return;
	}

	cursor_set_image_surface(cursor, event->surface, event->hotspot_x,
			event->hotspot_y, focused_client);
}

static void handle_pointer_hold_begin(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(
			listener, cursor, hold_begin);
	struct wlr_pointer_hold_begin_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->pointer->base);
	seatop_hold_begin(cursor->seat, event);
}

static void handle_pointer_hold_end(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(
			listener, cursor, hold_end);
	struct wlr_pointer_hold_end_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->pointer->base);
	seatop_hold_end(cursor->seat, event);
}

static void handle_pointer_pinch_begin(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(
			listener, cursor, pinch_begin);
	struct wlr_pointer_pinch_begin_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->pointer->base);
	seatop_pinch_begin(cursor->seat, event);
}

static void handle_pointer_pinch_update(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(
			listener, cursor, pinch_update);
	struct wlr_pointer_pinch_update_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->pointer->base);
	seatop_pinch_update(cursor->seat, event);
}

static void handle_pointer_pinch_end(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(
			listener, cursor, pinch_end);
	struct wlr_pointer_pinch_end_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->pointer->base);
	seatop_pinch_end(cursor->seat, event);
}

static void handle_pointer_swipe_begin(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(
			listener, cursor, swipe_begin);
	struct wlr_pointer_swipe_begin_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->pointer->base);
	seatop_swipe_begin(cursor->seat, event);
}

static void handle_pointer_swipe_update(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(
			listener, cursor, swipe_update);
	struct wlr_pointer_swipe_update_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->pointer->base);
	seatop_swipe_update(cursor->seat, event);
}

static void handle_pointer_swipe_end(struct wl_listener *listener, void *data) {
	struct sway_cursor *cursor = wl_container_of(
			listener, cursor, swipe_end);
	struct wlr_pointer_swipe_end_event *event = data;
	cursor_handle_activity_from_device(cursor, &event->pointer->base);
	seatop_swipe_end(cursor->seat, event);
}

static void handle_image_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct sway_cursor *cursor =
		wl_container_of(listener, cursor, image_surface_destroy);
	cursor_set_image(cursor, NULL, cursor->image_client);
	cursor_rebase(cursor);
}

static void set_image_surface(struct sway_cursor *cursor,
		struct wlr_surface *surface) {
	wl_list_remove(&cursor->image_surface_destroy.link);
	cursor->image_surface = surface;
	if (surface) {
		wl_signal_add(&surface->events.destroy, &cursor->image_surface_destroy);
	} else {
		wl_list_init(&cursor->image_surface_destroy.link);
	}
}

void cursor_set_image(struct sway_cursor *cursor, const char *image,
		struct wl_client *client) {
	if (!(cursor->seat->wlr_seat->capabilities & WL_SEAT_CAPABILITY_POINTER)) {
		return;
	}

	const char *current_image = cursor->image;
	set_image_surface(cursor, NULL);
	cursor->image = image;
	cursor->hotspot_x = cursor->hotspot_y = 0;
	cursor->image_client = client;

	if (cursor->hidden) {
		return;
	}

	if (!image) {
		wlr_cursor_unset_image(cursor->cursor);
	} else if (!current_image || strcmp(current_image, image) != 0) {
		wlr_cursor_set_xcursor(cursor->cursor, cursor->xcursor_manager, image);
	}
}

void cursor_set_image_surface(struct sway_cursor *cursor,
		struct wlr_surface *surface, int32_t hotspot_x, int32_t hotspot_y,
		struct wl_client *client) {
	if (!(cursor->seat->wlr_seat->capabilities & WL_SEAT_CAPABILITY_POINTER)) {
		return;
	}

	set_image_surface(cursor, surface);
	cursor->image = NULL;
	cursor->hotspot_x = hotspot_x;
	cursor->hotspot_y = hotspot_y;
	cursor->image_client = client;

	if (cursor->hidden) {
		return;
	}

	wlr_cursor_set_surface(cursor->cursor, surface, hotspot_x, hotspot_y);
}

void sway_cursor_destroy(struct sway_cursor *cursor) {
	if (!cursor) {
		return;
	}

	wl_event_source_remove(cursor->hide_source);

	wl_list_remove(&cursor->image_surface_destroy.link);
	wl_list_remove(&cursor->hold_begin.link);
	wl_list_remove(&cursor->hold_end.link);
	wl_list_remove(&cursor->pinch_begin.link);
	wl_list_remove(&cursor->pinch_update.link);
	wl_list_remove(&cursor->pinch_end.link);
	wl_list_remove(&cursor->swipe_begin.link);
	wl_list_remove(&cursor->swipe_update.link);
	wl_list_remove(&cursor->swipe_end.link);
	wl_list_remove(&cursor->motion.link);
	wl_list_remove(&cursor->motion_absolute.link);
	wl_list_remove(&cursor->button.link);
	wl_list_remove(&cursor->axis.link);
	wl_list_remove(&cursor->frame.link);
	wl_list_remove(&cursor->touch_down.link);
	wl_list_remove(&cursor->touch_up.link);
	wl_list_remove(&cursor->touch_cancel.link);
	wl_list_remove(&cursor->touch_motion.link);
	wl_list_remove(&cursor->touch_frame.link);
	wl_list_remove(&cursor->tool_axis.link);
	wl_list_remove(&cursor->tool_tip.link);
	wl_list_remove(&cursor->tool_button.link);
	wl_list_remove(&cursor->request_set_cursor.link);

	wlr_xcursor_manager_destroy(cursor->xcursor_manager);
	wlr_cursor_destroy(cursor->cursor);
	free(cursor);
}

struct sway_cursor *sway_cursor_create(struct sway_seat *seat) {
	struct sway_cursor *cursor = calloc(1, sizeof(struct sway_cursor));
	if (!sway_assert(cursor, "could not allocate sway cursor")) {
		return NULL;
	}

	struct wlr_cursor *wlr_cursor = wlr_cursor_create();
	if (!sway_assert(wlr_cursor, "could not allocate wlr cursor")) {
		free(cursor);
		return NULL;
	}

	cursor->previous.x = wlr_cursor->x;
	cursor->previous.y = wlr_cursor->y;

	cursor->seat = seat;
	wlr_cursor_attach_output_layout(wlr_cursor, root->output_layout);

	cursor->hide_source = wl_event_loop_add_timer(server.wl_event_loop,
			hide_notify, cursor);
	cursor->glassine_short_hide = true;	// Glassine: no mouse used yet

	wl_list_init(&cursor->image_surface_destroy.link);
	cursor->image_surface_destroy.notify = handle_image_surface_destroy;

	wl_signal_add(&wlr_cursor->events.hold_begin, &cursor->hold_begin);
	cursor->hold_begin.notify = handle_pointer_hold_begin;
	wl_signal_add(&wlr_cursor->events.hold_end, &cursor->hold_end);
	cursor->hold_end.notify = handle_pointer_hold_end;

	wl_signal_add(&wlr_cursor->events.pinch_begin, &cursor->pinch_begin);
	cursor->pinch_begin.notify = handle_pointer_pinch_begin;
	wl_signal_add(&wlr_cursor->events.pinch_update, &cursor->pinch_update);
	cursor->pinch_update.notify = handle_pointer_pinch_update;
	wl_signal_add(&wlr_cursor->events.pinch_end, &cursor->pinch_end);
	cursor->pinch_end.notify = handle_pointer_pinch_end;

	wl_signal_add(&wlr_cursor->events.swipe_begin, &cursor->swipe_begin);
	cursor->swipe_begin.notify = handle_pointer_swipe_begin;
	wl_signal_add(&wlr_cursor->events.swipe_update, &cursor->swipe_update);
	cursor->swipe_update.notify = handle_pointer_swipe_update;
	wl_signal_add(&wlr_cursor->events.swipe_end, &cursor->swipe_end);
	cursor->swipe_end.notify = handle_pointer_swipe_end;

	// input events
	wl_signal_add(&wlr_cursor->events.motion, &cursor->motion);
	cursor->motion.notify = handle_pointer_motion_relative;

	wl_signal_add(&wlr_cursor->events.motion_absolute,
		&cursor->motion_absolute);
	cursor->motion_absolute.notify = handle_pointer_motion_absolute;

	wl_signal_add(&wlr_cursor->events.button, &cursor->button);
	cursor->button.notify = handle_pointer_button;

	wl_signal_add(&wlr_cursor->events.axis, &cursor->axis);
	cursor->axis.notify = handle_pointer_axis;

	wl_signal_add(&wlr_cursor->events.frame, &cursor->frame);
	cursor->frame.notify = handle_pointer_frame;

	wl_signal_add(&wlr_cursor->events.touch_down, &cursor->touch_down);
	cursor->touch_down.notify = handle_touch_down;

	wl_signal_add(&wlr_cursor->events.touch_up, &cursor->touch_up);
	cursor->touch_up.notify = handle_touch_up;

	wl_signal_add(&wlr_cursor->events.touch_cancel, &cursor->touch_cancel);
	cursor->touch_cancel.notify = handle_touch_cancel;

	wl_signal_add(&wlr_cursor->events.touch_motion,
		&cursor->touch_motion);
	cursor->touch_motion.notify = handle_touch_motion;

	wl_signal_add(&wlr_cursor->events.touch_frame, &cursor->touch_frame);
	cursor->touch_frame.notify = handle_touch_frame;

	wl_signal_add(&wlr_cursor->events.tablet_tool_axis,
		&cursor->tool_axis);
	cursor->tool_axis.notify = handle_tool_axis;

	wl_signal_add(&wlr_cursor->events.tablet_tool_tip, &cursor->tool_tip);
	cursor->tool_tip.notify = handle_tool_tip;

	wl_signal_add(&wlr_cursor->events.tablet_tool_proximity, &cursor->tool_proximity);
	cursor->tool_proximity.notify = handle_tool_proximity;

	wl_signal_add(&wlr_cursor->events.tablet_tool_button, &cursor->tool_button);
	cursor->tool_button.notify = handle_tool_button;

	wl_signal_add(&seat->wlr_seat->events.request_set_cursor,
			&cursor->request_set_cursor);
	cursor->request_set_cursor.notify = handle_request_pointer_set_cursor;

	wl_list_init(&cursor->constraint_commit.link);
	wl_list_init(&cursor->tablets);
	wl_list_init(&cursor->tablet_pads);

	cursor->cursor = wlr_cursor;

	return cursor;
}

/**
 * Warps the cursor to the middle of the container argument.
 * Does nothing if the cursor is already inside the container and `force` is
 * false. If container is NULL, returns without doing anything.
 */
void cursor_warp_to_container(struct sway_cursor *cursor,
		struct sway_container *container, bool force) {
	if (!container) {
		return;
	}

	struct wlr_box box;
	container_get_box(container, &box);
	if (!force && wlr_box_contains_point(&box, cursor->cursor->x,
			cursor->cursor->y)) {
		return;
	}

	double x = container->pending.x + container->pending.width / 2.0;
	double y = container->pending.y + container->pending.height / 2.0;

	wlr_cursor_warp(cursor->cursor, NULL, x, y);
	cursor_unhide(cursor);
}

/**
 * Warps the cursor to the middle of the workspace argument.
 * If workspace is NULL, returns without doing anything.
 */
void cursor_warp_to_workspace(struct sway_cursor *cursor,
		struct sway_workspace *workspace) {
	if (!workspace) {
		return;
	}

	double x = workspace->x + workspace->width / 2.0;
	double y = workspace->y + workspace->height / 2.0;

	wlr_cursor_warp(cursor->cursor, NULL, x, y);
	cursor_unhide(cursor);
}

uint32_t get_mouse_bindsym(const char *name, char **error) {
	if (strncasecmp(name, "button", strlen("button")) == 0) {
		// Map to x11 mouse buttons
		int number = name[strlen("button")] - '0';
		if (number < 1 || number > 9 || strlen(name) > strlen("button0")) {
			*error = strdup("Only buttons 1-9 are supported. For other mouse "
					"buttons, use the name of the event code.");
			return 0;
		}
		static const uint32_t buttons[] = {BTN_LEFT, BTN_MIDDLE, BTN_RIGHT,
			SWAY_SCROLL_UP, SWAY_SCROLL_DOWN, SWAY_SCROLL_LEFT,
			SWAY_SCROLL_RIGHT, BTN_SIDE, BTN_EXTRA};
		return buttons[number - 1];
	} else if (strncmp(name, "BTN_", strlen("BTN_")) == 0) {
		// Get event code from name
		int code = libevdev_event_code_from_name(EV_KEY, name);
		if (code == -1) {
			*error = format_str("Unknown event %s", name);
			return 0;
		}
		return code;
	}
	return 0;
}

uint32_t get_mouse_bindcode(const char *name, char **error) {
	// Validate event code
	errno = 0;
	char *endptr;
	int code = strtol(name, &endptr, 10);
	if (endptr == name && code <= 0) {
		*error = strdup("Button event code must be a positive integer.");
		return 0;
	} else if (errno == ERANGE) {
		*error = strdup("Button event code out of range.");
		return 0;
	}
	const char *event = libevdev_event_code_get_name(EV_KEY, code);
	if (!event || strncmp(event, "BTN_", strlen("BTN_")) != 0) {
		*error = format_str("Event code %d (%s) is not a button",
			code, event ? event : "(null)");
		return 0;
	}
	return code;
}

uint32_t get_mouse_button(const char *name, char **error) {
	uint32_t button = get_mouse_bindsym(name, error);
	if (!button && !*error) {
		button = get_mouse_bindcode(name, error);
	}
	return button;
}

const char *get_mouse_button_name(uint32_t button) {
	const char *name = libevdev_event_code_get_name(EV_KEY, button);
	if (!name) {
		if (button == SWAY_SCROLL_UP) {
			name = "SWAY_SCROLL_UP";
		} else if (button == SWAY_SCROLL_DOWN) {
			name = "SWAY_SCROLL_DOWN";
		} else if (button == SWAY_SCROLL_LEFT) {
			name = "SWAY_SCROLL_LEFT";
		} else if (button == SWAY_SCROLL_RIGHT) {
			name = "SWAY_SCROLL_RIGHT";
		}
	}
	return name;
}

static void warp_to_constraint_cursor_hint(struct sway_cursor *cursor) {
	struct wlr_pointer_constraint_v1 *constraint = cursor->active_constraint;

	if (constraint->current.cursor_hint.enabled) {
		double sx = constraint->current.cursor_hint.x;
		double sy = constraint->current.cursor_hint.y;

		struct sway_view *view = view_from_wlr_surface(constraint->surface);
		if (!view) {
			return;
		}

		struct sway_container *con = view->container;

		double lx = sx + con->pending.content_x - view->geometry.x;
		double ly = sy + con->pending.content_y - view->geometry.y;

		wlr_cursor_warp(cursor->cursor, NULL, lx, ly);

		// Warp the pointer as well, so that on the next pointer rebase we don't
		// send an unexpected synthetic motion event to clients.
		wlr_seat_pointer_warp(constraint->seat, sx, sy);
	}
}

void handle_constraint_destroy(struct wl_listener *listener, void *data) {
	struct sway_pointer_constraint *sway_constraint =
		wl_container_of(listener, sway_constraint, destroy);
	struct wlr_pointer_constraint_v1 *constraint = data;
	struct sway_cursor *cursor = sway_constraint->cursor;

	wl_list_remove(&sway_constraint->set_region.link);
	wl_list_remove(&sway_constraint->destroy.link);

	if (cursor->active_constraint == constraint) {
		warp_to_constraint_cursor_hint(cursor);

		if (cursor->constraint_commit.link.next != NULL) {
			wl_list_remove(&cursor->constraint_commit.link);
		}
		wl_list_init(&cursor->constraint_commit.link);
		cursor->active_constraint = NULL;
	}

	free(sway_constraint);
}

void handle_pointer_constraint(struct wl_listener *listener, void *data) {
	struct wlr_pointer_constraint_v1 *constraint = data;
	struct sway_seat *seat = constraint->seat->data;

	struct sway_pointer_constraint *sway_constraint =
		calloc(1, sizeof(struct sway_pointer_constraint));
	sway_constraint->cursor = seat->cursor;
	sway_constraint->constraint = constraint;

	sway_constraint->set_region.notify = handle_pointer_constraint_set_region;
	wl_signal_add(&constraint->events.set_region, &sway_constraint->set_region);

	sway_constraint->destroy.notify = handle_constraint_destroy;
	wl_signal_add(&constraint->events.destroy, &sway_constraint->destroy);

	struct wlr_surface *surface = seat->wlr_seat->keyboard_state.focused_surface;
	if (surface && surface == constraint->surface) {
		sway_cursor_constrain(seat->cursor, constraint);
	}
}

void sway_cursor_constrain(struct sway_cursor *cursor,
		struct wlr_pointer_constraint_v1 *constraint) {
	struct seat_config *config = seat_get_config(cursor->seat);
	if (!config) {
		config = seat_get_config_by_name("*");
	}

	if (!config || config->allow_constrain == CONSTRAIN_DISABLE) {
		return;
	}

	if (cursor->active_constraint == constraint) {
		return;
	}

	wl_list_remove(&cursor->constraint_commit.link);
	if (cursor->active_constraint) {
		if (constraint == NULL) {
			warp_to_constraint_cursor_hint(cursor);
		}
		wlr_pointer_constraint_v1_send_deactivated(
			cursor->active_constraint);
	}

	cursor->active_constraint = constraint;

	if (constraint == NULL) {
		wl_list_init(&cursor->constraint_commit.link);
		return;
	}

	cursor->active_confine_requires_warp = true;

	// FIXME: Big hack, stolen from wlr_pointer_constraints_v1.c:121.
	// This is necessary because the focus may be set before the surface
	// has finished committing, which means that warping won't work properly,
	// since this code will be run *after* the focus has been set.
	// That is why we duplicate the code here.
	if (pixman_region32_not_empty(&constraint->current.region)) {
		pixman_region32_intersect(&constraint->region,
			&constraint->surface->input_region, &constraint->current.region);
	} else {
		pixman_region32_copy(&constraint->region,
			&constraint->surface->input_region);
	}

	check_constraint_region(cursor);

	wlr_pointer_constraint_v1_send_activated(constraint);

	cursor->constraint_commit.notify = handle_constraint_commit;
	wl_signal_add(&constraint->surface->events.commit,
		&cursor->constraint_commit);
}

void handle_request_set_cursor_shape(struct wl_listener *listener, void *data) {
	const struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	struct sway_seat *seat = event->seat_client->seat->data;

	if (!seatop_allows_set_cursor(seat)) {
		return;
	}

	struct wl_client *focused_client = NULL;
	struct wlr_surface *focused_surface = seat->wlr_seat->pointer_state.focused_surface;
	if (focused_surface != NULL) {
		focused_client = wl_resource_get_client(focused_surface->resource);
	}

	// TODO: check cursor mode
	if (focused_client == NULL || event->seat_client->client != focused_client) {
		sway_log(SWAY_DEBUG, "denying request to set cursor from unfocused client");
		return;
	}

	cursor_set_image(seat->cursor, wlr_cursor_shape_v1_name(event->shape), focused_client);
}
