/* glassine-gesture.c — see glassine-gesture.h */
#include "glassine-gesture.h"
#include <math.h>
#include <string.h>

void glassine_g_init(struct glassine_gesture *g, int fingers, double swipe_px)
{
    memset(g, 0, sizeof *g);
    g->fingers = fingers < 2 ? 2 : fingers > GLASSINE_G_MAXPT ? GLASSINE_G_MAXPT : fingers;
    g->swipe_px = swipe_px > 0 ? swipe_px : 125;
    g->tap_ms = 400;
    g->tap_join_ms = 250;
    g->tap_px = 15;
}

void glassine_g_tap_init(struct glassine_gesture *g, int tap_ms, int join_ms, double px)
{
    if (tap_ms) g->tap_ms = tap_ms < 0 ? 0 : (uint32_t)tap_ms;
    if (join_ms > 0) g->tap_join_ms = join_ms;
    if (px > 0) g->tap_px = px;
}

void glassine_g_clock(struct glassine_gesture *g, uint32_t ms)
{
    g->now = ms;
}

int glassine_g_flush(struct glassine_gesture *g, int32_t *ids, int max)
{
    int k = 0;
    for (int i = 0; i < g->nflush && k < max; i++) ids[k++] = g->flush[i];
    g->nflush = 0;
    return k;
}

static void tap_fail(struct glassine_gesture *g);
static int tap_late(const struct glassine_gesture *g);

int glassine_g_tick(struct glassine_gesture *g)
{
    if (!g->nheld || !tap_late(g)) return 0;
    tap_fail(g);
    return 1;
}

uint32_t glassine_g_hold_ms(const struct glassine_gesture *g)
{
    if (!g->nheld) return 0;
    uint32_t used = g->now - g->t0;
    return used >= g->tap_ms ? 1 : g->tap_ms - used + 1;
}

/* the tap will not come: the held-back up is due */
static void tap_fail(struct glassine_gesture *g)
{
    for (int i = 0; i < g->nheld && g->nflush < GLASSINE_G_MAXPT; i++) g->flush[g->nflush++] = g->held[i];
    g->nheld = 0;
    g->tap = 0;
}

static int tap_late(const struct glassine_gesture *g)
{
    return g->now - g->t0 > g->tap_ms;
}

static int find(const struct glassine_gesture *g, int32_t id)
{
    for (int i = 0; i < g->n; i++) if (g->pt[i].id == id) return i;
    return -1;
}

static void drop(struct glassine_gesture *g, int i)
{
    g->pt[i] = g->pt[--g->n];
    if (!g->n) g->active = g->fired = g->most = g->tap = 0;   /* the last finger is up: the gesture is over */
}

/* the centroid's travel since each finger's start (fingers that joined late
 * count from where they landed) */
static void travel(const struct glassine_gesture *g, double *dx, double *dy)
{
    *dx = *dy = 0;
    for (int i = 0; i < g->n; i++) { *dx += g->pt[i].x - g->pt[i].x0; *dy += g->pt[i].y - g->pt[i].y0; }
    if (g->n) { *dx /= g->n; *dy /= g->n; }
}

enum glassine_g_result glassine_g_down(struct glassine_gesture *g, int32_t id, double x, double y)
{
    int i = find(g, id);
    if (i < 0) {
        if (g->n == GLASSINE_G_MAXPT) return g->active ? GLASSINE_G_SWALLOW : GLASSINE_G_PASS;
        i = g->n++;
    }
    g->pt[i].id = id;
    g->pt[i].x = g->pt[i].x0 = x;
    g->pt[i].y = g->pt[i].y0 = y;
    if (g->n == 1 && !g->active && !g->nheld) {           /* a new sequence: may become a tap */
        g->tap = g->tap_ms ? 1 : 0;
        g->t0 = g->now;
    } else if (g->tap == 1 && g->n == 2 && g->now - g->t0 <= g->tap_join_ms) {
        g->tap = 2;
        g->tx = (g->pt[0].x + g->pt[1].x) / 2;
        g->ty = (g->pt[0].y + g->pt[1].y) / 2;
    } else if (g->tap) {
        tap_fail(g);                                    /* a 3rd finger, or the 2nd came late */
    }
    if (g->active) {
        if (!g->fired && g->n > g->most) {      /* one more finger before the swipe: count from here */
            g->most = g->n;
            for (int k = 0; k < g->n; k++) { g->pt[k].x0 = g->pt[k].x; g->pt[k].y0 = g->pt[k].y; }
        }
        return GLASSINE_G_SWALLOW;
    }
    if (g->n >= g->fingers) {
        g->most = g->n;
        g->active = 1;
        /* the gesture's travel counts from here */
        g->cx = g->cy = 0;
        for (int k = 0; k < g->n; k++) {
            g->pt[k].x0 = g->pt[k].x; g->pt[k].y0 = g->pt[k].y;
            g->cx += g->pt[k].x / g->n; g->cy += g->pt[k].y / g->n;
        }
        return GLASSINE_G_CANCEL;
    }
    return GLASSINE_G_PASS;
}

enum glassine_g_result glassine_g_motion(struct glassine_gesture *g, int32_t id, double x, double y)
{
    int i = find(g, id);
    if (i < 0) return g->active ? GLASSINE_G_SWALLOW : GLASSINE_G_PASS;
    g->pt[i].x = x;
    g->pt[i].y = y;
    if (g->tap && (fabs(x - g->pt[i].x0) > g->tap_px || fabs(y - g->pt[i].y0) > g->tap_px || tap_late(g)))
        tap_fail(g);
    if (!g->active) return GLASSINE_G_PASS;
    if (g->fired) return GLASSINE_G_SWALLOW;
    double dx, dy;
    travel(g, &dx, &dy);
    if (fabs(dx) >= g->swipe_px && fabs(dx) > fabs(dy)) {
        g->fired = 1;
        return dx < 0 ? GLASSINE_G_SWIPE_LEFT : GLASSINE_G_SWIPE_RIGHT;
    }
    if (fabs(dy) >= g->swipe_px && fabs(dy) > fabs(dx)) {
        g->fired = 1;
        return dy < 0 ? GLASSINE_G_SWIPE_UP : GLASSINE_G_SWIPE_DOWN;
    }
    return GLASSINE_G_SWALLOW;
}

enum glassine_g_result glassine_g_up(struct glassine_gesture *g, int32_t id)
{
    int active = g->active, i = find(g, id);
    if (i >= 0 && g->tap) {
        if (g->tap != 2 || tap_late(g)) {
            tap_fail(g);
        } else if (g->n == 2) {                     /* the first of the two: hold its up back */
            g->held[g->nheld++] = id;
            drop(g, i);
            return GLASSINE_G_SWALLOW;
        } else if (g->n == 1 && g->nheld == 1) {    /* the second: a tap */
            g->ntap = 0;
            g->tap_ids[g->ntap++] = g->held[0];
            g->tap_ids[g->ntap++] = id;
            g->nheld = 0;
            drop(g, i);
            return GLASSINE_G_TAP2;
        } else {
            tap_fail(g);
        }
    }
    if (i >= 0) drop(g, i);
    return active ? GLASSINE_G_SWALLOW : GLASSINE_G_PASS;
}

enum glassine_g_result glassine_g_cancel(struct glassine_gesture *g, int32_t id)
{
    if (g->tap) tap_fail(g);
    return glassine_g_up(g, id);
}

int glassine_g_points(const struct glassine_gesture *g, int32_t *ids, int max)
{
    int k = 0;
    for (int i = 0; i < g->n && k < max; i++) ids[k++] = g->pt[i].id;
    return k;
}
