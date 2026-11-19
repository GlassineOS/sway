/* gesture-edge-test.c — host test of the edge swipes in sway/glassine-gesture.c
 * (GlassineOS/shell spec 001 FR-060): a finger landing alone near an armed
 * edge of the output's box and moving straight inward by swipe_px; what the
 * app sees before (PASS) and after (swallowed), and what is no edge swipe.
 * Run by gesture-edge-test.sh. Exit 0: passed; 1: a check failed. */
#include "glassine-gesture.h"
#include <stdio.h>

static int fails, checks;
static struct glassine_gesture g;
static uint32_t t;

static void want(const char *what, enum glassine_g_result got, enum glassine_g_result w) {
	checks++;
	if (got != w) {
		fails++;
		printf("FAIL %s: got %d, want %d\n", what, got, w);
	}
}

static void wantu(const char *what, unsigned got, unsigned w) {
	checks++;
	if (got != w) {
		fails++;
		printf("FAIL %s: got %#x, want %#x\n", what, got, w);
	}
}

/* the output's box: not at the layout's origin (a second output, as sway lays it out) */
#define BX 100.0
#define BY 50.0
#define BW 1404.0
#define BH 1872.0
#define ALL (GLASSINE_G_E_TOP | GLASSINE_G_E_BOTTOM | GLASSINE_G_E_LEFT | GLASSINE_G_E_RIGHT)

static void reset(void) {
	glassine_g_init(&g, 3, 125);
	t = 1000;
}

static enum glassine_g_result down(int32_t id, double x, double y, unsigned armed) {
	glassine_g_clock(&g, t += 10);
	glassine_g_edges(&g, BX, BY, BW, BH, armed);
	return glassine_g_down(&g, id, x, y);
}

static enum glassine_g_result move(int32_t id, double x, double y) {
	glassine_g_clock(&g, t += 10);
	return glassine_g_motion(&g, id, x, y);
}

static enum glassine_g_result up(int32_t id) {
	glassine_g_clock(&g, t += 10);
	return glassine_g_up(&g, id);
}

/* lands at x, y, moves by dx, dy in 5 steps; the last motion's result */
static enum glassine_g_result swipe(const char *what, double x, double y, double dx, double dy, unsigned armed) {
	reset();
	want(what, down(1, x, y, armed), GLASSINE_G_PASS);
	enum glassine_g_result r = GLASSINE_G_PASS;
	for (int i = 1; i <= 5; i++) {
		r = move(1, x + dx * i / 5, y + dy * i / 5);
		if (r != GLASSINE_G_PASS) {
			break;
		}
	}
	return r;
}

int main(void) {
	/* which edges a landing is near (24 px, inclusive) */
	reset();
	wantu("near top", glassine_g_edge_near(&g, BX + 700, BY + 3, BX, BY, BW, BH), GLASSINE_G_E_TOP);
	wantu("near top at 24", glassine_g_edge_near(&g, BX + 700, BY + 24, BX, BY, BW, BH), GLASSINE_G_E_TOP);
	wantu("not near at 30", glassine_g_edge_near(&g, BX + 700, BY + 30, BX, BY, BW, BH), 0);
	wantu("near bottom", glassine_g_edge_near(&g, BX + 700, BY + BH - 10, BX, BY, BW, BH), GLASSINE_G_E_BOTTOM);
	wantu("near left", glassine_g_edge_near(&g, BX + 1, BY + 900, BX, BY, BW, BH), GLASSINE_G_E_LEFT);
	wantu("near right", glassine_g_edge_near(&g, BX + BW - 5, BY + 900, BX, BY, BW, BH), GLASSINE_G_E_RIGHT);
	wantu("corner", glassine_g_edge_near(&g, BX + 2, BY + 2, BX, BY, BW, BH), GLASSINE_G_E_TOP | GLASSINE_G_E_LEFT);
	wantu("outside the box", glassine_g_edge_near(&g, BX - 5, BY + 2, BX, BY, BW, BH), 0);
	wantu("middle", glassine_g_edge_near(&g, BX + 700, BY + 900, BX, BY, BW, BH), 0);
	/* a finger already down: no edge swipe can start */
	want("other finger", down(9, BX + 700, BY + 900, 0), GLASSINE_G_PASS);
	wantu("near top, a finger down", glassine_g_edge_near(&g, BX + 700, BY + 3, BX, BY, BW, BH), 0);

	/* the four edges, straight inward by 130 */
	want("top, down 130", swipe("top", BX + 700, BY + 5, 0, 130, ALL), GLASSINE_G_EDGE_TOP);
	checks++;
	if (g.ex != BX + 700 || g.ey != BY + 5) {
		fails++;
		printf("FAIL top: landing point %.0f,%.0f\n", g.ex, g.ey);
	}
	want("bottom, up 130", swipe("bottom", BX + 700, BY + BH - 5, 0, -130, ALL), GLASSINE_G_EDGE_BOTTOM);
	want("left, right 130", swipe("left", BX + 5, BY + 900, 130, 0, ALL), GLASSINE_G_EDGE_LEFT);
	want("right, left 130", swipe("right", BX + BW - 5, BY + 900, -130, 0, ALL), GLASSINE_G_EDGE_RIGHT);
	/* slightly slanted, inward still the main direction */
	want("top, slanted", swipe("top slanted", BX + 700, BY + 5, 60, 130, ALL), GLASSINE_G_EDGE_TOP);

	/* the rest of that sequence is swallowed; the next one passes again */
	swipe("after", BX + 700, BY + 5, 0, 130, GLASSINE_G_E_TOP);
	want("after: motion", move(1, BX + 700, BY + 400), GLASSINE_G_SWALLOW);
	want("after: a 2nd finger", down(2, BX + 300, BY + 900, 0), GLASSINE_G_SWALLOW);
	want("after: its up", up(2), GLASSINE_G_SWALLOW);
	want("after: up", up(1), GLASSINE_G_SWALLOW);
	want("after: new down", down(3, BX + 700, BY + 900, 0), GLASSINE_G_PASS);
	want("after: new motion", move(3, BX + 700, BY + 1100), GLASSINE_G_PASS);
	want("after: new up", up(3), GLASSINE_G_PASS);

	/* fires once: further travel does not fire again */
	swipe("once", BX + 700, BY + 5, 0, 130, GLASSINE_G_E_TOP);
	want("once: more travel", move(1, BX + 700, BY + 300), GLASSINE_G_SWALLOW);

	/* no edge swipe */
	want("30 px from the edge", swipe("30 px", BX + 700, BY + 30, 0, 130, ALL), GLASSINE_G_PASS);
	want("top not armed (nothing)", swipe("not armed", BX + 700, BY + 5, 0, 130, GLASSINE_G_E_BOTTOM),
		GLASSINE_G_PASS);
	want("no edge armed", swipe("unarmed", BX + 700, BY + 5, 0, 130, 0), GLASSINE_G_PASS);
	want("top, sideways", swipe("sideways", BX + 300, BY + 5, 130, 10, ALL), GLASSINE_G_PASS);
	/* after a sideways decision, more inward travel does not fire */
	want("sideways, then down", move(1, BX + 430, BY + 300), GLASSINE_G_PASS);
	want("top, outward", swipe("outward", BX + 700, BY + 20, 0, -130, GLASSINE_G_E_TOP), GLASSINE_G_PASS);
	want("top, too short", swipe("short", BX + 700, BY + 5, 0, 120, ALL), GLASSINE_G_PASS);
	want("top, short: its up", up(1), GLASSINE_G_PASS);
	/* corner: the direction picks the edge, if it is armed */
	want("corner, down", swipe("corner down", BX + 5, BY + 5, 0, 130, ALL), GLASSINE_G_EDGE_TOP);
	want("corner, right", swipe("corner right", BX + 5, BY + 5, 130, 0, ALL), GLASSINE_G_EDGE_LEFT);
	want("corner, right, left not armed", swipe("corner right", BX + 5, BY + 5, 130, 0, GLASSINE_G_E_TOP),
		GLASSINE_G_PASS);
	/* the arming is for one down only */
	reset();
	glassine_g_edges(&g, BX, BY, BW, BH, ALL);
	want("arm once: first down", down(1, BX + 700, BY + 900, ALL), GLASSINE_G_PASS);
	want("arm once: its up", up(1), GLASSINE_G_PASS);
	glassine_g_clock(&g, t += 10);
	want("arm once: unarmed down", glassine_g_down(&g, 2, BX + 700, BY + 5), GLASSINE_G_PASS);
	want("arm once: no edge", move(2, BX + 700, BY + 140), GLASSINE_G_PASS);

	/* a second finger before recognition: no edge swipe, the normal gestures go on */
	reset();
	want("2nd: down", down(1, BX + 700, BY + 5, ALL), GLASSINE_G_PASS);
	want("2nd: second finger", down(2, BX + 900, BY + 600, ALL), GLASSINE_G_PASS);
	want("2nd: motion", move(1, BX + 700, BY + 140), GLASSINE_G_PASS);
	want("2nd: third finger", down(3, BX + 500, BY + 600, ALL), GLASSINE_G_CANCEL);
	move(1, BX + 700, BY + 140 + 130);
	move(2, BX + 900, BY + 600 + 130);
	want("2nd: 3-finger swipe down", move(3, BX + 500, BY + 600 + 130), GLASSINE_G_SWIPE_DOWN);
	up(1);
	up(2);
	want("2nd: last up", up(3), GLASSINE_G_SWALLOW);
	want("2nd: next sequence", down(4, BX + 700, BY + 900, 0), GLASSINE_G_PASS);

	/* the candidate's up or a cancel ends it */
	reset();
	down(1, BX + 700, BY + 5, ALL);
	want("up: candidate's up", up(1), GLASSINE_G_PASS);
	want("up: other down", down(2, BX + 700, BY + 900, 0), GLASSINE_G_PASS);
	want("up: other moves", move(2, BX + 700, BY + 1100), GLASSINE_G_PASS);
	reset();
	down(1, BX + 700, BY + 5, ALL);
	glassine_g_clock(&g, t += 10);
	want("cancel: candidate cancelled", glassine_g_cancel(&g, 1), GLASSINE_G_PASS);
	want("cancel: down again", down(1, BX + 700, BY + 900, 0), GLASSINE_G_PASS);
	want("cancel: no edge", move(1, BX + 700, BY + 1100), GLASSINE_G_PASS);

	/* a two-finger tap that starts near an edge still works */
	reset();
	want("tap: 1st near top", down(1, BX + 700, BY + 5, ALL), GLASSINE_G_PASS);
	want("tap: 2nd", down(2, BX + 760, BY + 300, ALL), GLASSINE_G_PASS);
	want("tap: 1st up held", up(1), GLASSINE_G_SWALLOW);
	want("tap: 2nd up", up(2), GLASSINE_G_TAP2);

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
