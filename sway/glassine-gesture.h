/* glassine-gesture.h — the compositor-side touch gestures of the Glassine sway session
 * (. Pure logic, no wlroots: sway's
 * touch handlers (0002-glassine-touch-gestures.patch) feed every touch event in and
 * do what comes back.
 *
 * Touches go to the apps at once (no holding back as glassine-touchd does on X;
 * apps handle tap, drag and long press themselves). When the Nth finger lands
 * (N = fingers, 3), the sequence becomes a gesture: the apps' touches are
 * cancelled (wl_touch.cancel), and every touch event until the last finger is
 * up is swallowed. A move of the fingers' centroid by swipe_px (layout
 * coordinates) fires the swipe once, by its main direction: left or right
 * (screens), up or down (fullscreen, minimize). cx, cy: where
 * the gesture started (the centroid when the Nth finger landed): the window
 * it acts on. most: the fingers down at most before the swipe fired (3 or 4:
 * which set of actions; a finger that lands before the swipe restarts the
 * travel, so a 4-finger swipe counts from its 4th finger).
 *
 * Two-finger tap (user: the X session's long press = right click
 * has no equal under sway; a hold stays the apps'): exactly two fingers, the
 * 2nd down within tap_join_ms of the 1st, neither moving tap_px, both up
 * within tap_ms of the 1st down. The apps get both touches at once as usual;
 * only the up of the first finger to lift is held back while the tap is still
 * possible (else the app would see a finished one-finger tap). The last up
 * then returns GLASSINE_G_TAP2: the glue cancels the apps' sequence (tap_ids) and
 * runs the tap's action at (tx, ty), the fingers' centroid when the 2nd
 * landed. Once the tap is impossible (a move, a 3rd finger, too slow), the
 * held-back up is due: the glue delivers what glassine_g_flush returns before it
 * acts on the event. "Too slow" is noticed at the next touch event, or by the
 * glue's timer (glassine_g_hold_ms, glassine_g_tick). glassine_g_clock sets the time of the
 * events.
 *
 * Edge swipes (GlassineOS/shell spec 001 FR-060): before a down the glue
 * gives the output's box under the landing point (layout coordinates, the
 * output as it is turned) and the edges that have an action (glassine_g_edges;
 * only for that down). A finger that lands alone (no other touch down) within
 * edge_px (24) of an armed edge is a candidate: its touch still goes to the
 * app at once (PASS). When its own travel first reaches swipe_px, the main
 * direction decides: straight inward from an armed edge it was near returns
 * GLASSINE_G_EDGE_TOP..RIGHT once (the glue cancels the app's touch of that
 * finger and runs the edge's action at ex, ey, where it landed), and the rest
 * of the sequence is swallowed, as after a swipe; any other direction: no
 * edge swipe. A second finger, its up or a cancel ends the candidate. An edge
 * that is not armed is not tracked at all. */
#ifndef GLASSINE_GESTURE_H
#define GLASSINE_GESTURE_H
#include <stdint.h>

#define GLASSINE_G_MAXPT 10

enum glassine_g_result {
    GLASSINE_G_PASS,          /* hand the event to the apps as usual */
    GLASSINE_G_SWALLOW,       /* drop it */
    GLASSINE_G_CANCEL,        /* drop it, and cancel the apps' touch sequence now */
    GLASSINE_G_SWIPE_LEFT,    /* drop it; the fingers moved left: run the "left" action */
    GLASSINE_G_SWIPE_RIGHT,
    GLASSINE_G_SWIPE_UP,
    GLASSINE_G_SWIPE_DOWN,
    GLASSINE_G_TAP2,          /* drop it; a two-finger tap: cancel tap_ids, run the tap at tx, ty */
    GLASSINE_G_EDGE_TOP,      /* drop it; an edge swipe: cancel the finger's app touch, run the edge's action at ex, ey */
    GLASSINE_G_EDGE_BOTTOM,
    GLASSINE_G_EDGE_LEFT,
    GLASSINE_G_EDGE_RIGHT,
};

/* the edges, as bits for glassine_g_edges (in the order of the results) */
#define GLASSINE_G_E_TOP    (1u << 0)
#define GLASSINE_G_E_BOTTOM (1u << 1)
#define GLASSINE_G_E_LEFT   (1u << 2)
#define GLASSINE_G_E_RIGHT  (1u << 3)

struct glassine_gesture {
    int fingers;        /* touches that make a gesture (3) */
    double swipe_px;    /* centroid travel for a swipe */
    /* state */
    int n;
    struct { int32_t id; double x, y, x0, y0; } pt[GLASSINE_G_MAXPT];
    int active, fired;  /* in a gesture / its swipe fired */
    double cx, cy;      /* the centroid when the gesture began */
    int most;           /* the most fingers down in this gesture before its swipe */
    /* two-finger tap */
    uint32_t tap_ms, tap_join_ms;   /* first down to last up (400) / to the 2nd down (250) */
    double tap_px;      /* the most a tapping finger may move (15) */
    uint32_t now, t0;   /* the event's time (glassine_g_clock) / the sequence's first down */
    int tap;            /* 1: one finger, may become a tap; 2: two fingers; 0: not a tap */
    double tx, ty;
    int nheld, nflush, ntap;
    int32_t held[GLASSINE_G_MAXPT], flush[GLASSINE_G_MAXPT], tap_ids[GLASSINE_G_MAXPT];
    /* edge swipes */
    double edge_px;     /* how near an edge a finger lands (24) */
    double bx, by, bw, bh;  /* the output's box for the next down (glassine_g_edges) */
    unsigned armed;     /* the edges with an action, for the next down only */
    unsigned edge;      /* the candidate's edges (those it landed near), 0: none */
    int32_t edge_id;    /* the candidate's touch */
    double ex, ey;      /* where it landed */
};

void glassine_g_init(struct glassine_gesture *g, int fingers, double swipe_px);
/* the two-finger tap's limits (0: keep the default; it is on after glassine_g_init);
 * tap_ms < 0: off */
void glassine_g_tap_init(struct glassine_gesture *g, int tap_ms, int join_ms, double px);
/* the time (ms) of the event about to be fed in */
void glassine_g_clock(struct glassine_gesture *g, uint32_t ms);
/* no event since: is the held-back up due now (at the time of glassine_g_clock)?
 * 1: the tap timed out, deliver glassine_g_flush's ups. hold_ms: how long the tap
 * can still come (for a timer; 0: nothing held) */
int glassine_g_tick(struct glassine_gesture *g);
uint32_t glassine_g_hold_ms(const struct glassine_gesture *g);
/* ups held back for a tap that will not come: deliver these first (returns how many) */
int glassine_g_flush(struct glassine_gesture *g, int32_t *ids, int max);
/* the box of the output under the next down and the edges (GLASSINE_G_E_*)
 * that have an action; used by that down only */
void glassine_g_edges(struct glassine_gesture *g, double x, double y, double w, double h, unsigned armed);
/* the edges of box x, y, w, h that a down at px, py would be near, if it can
 * start an edge swipe (no touch down): 0 otherwise. For the glue, to look up
 * only those edges' actions */
unsigned glassine_g_edge_near(const struct glassine_gesture *g, double px, double py,
    double x, double y, double w, double h);
enum glassine_g_result glassine_g_down(struct glassine_gesture *g, int32_t id, double x, double y);
enum glassine_g_result glassine_g_motion(struct glassine_gesture *g, int32_t id, double x, double y);
enum glassine_g_result glassine_g_up(struct glassine_gesture *g, int32_t id);
/* the device cancelled its touches: forget them all (PASS unless in a gesture) */
enum glassine_g_result glassine_g_cancel(struct glassine_gesture *g, int32_t id);
/* the ids the apps still hold (to cancel them): returns how many */
int glassine_g_points(const struct glassine_gesture *g, int32_t *ids, int max);

#endif
