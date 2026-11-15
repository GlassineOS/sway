/* Host test of the writing paper's decision window (sway/glassine-palm-window.c) */
#include <stdio.h>
#include <stdlib.h>
#include "glassine-palm-window.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
	struct glassine_palm_window w;
	struct glassine_pw_ev ev[GLASSINE_PW_EVMAX];
	int32_t ids[GLASSINE_PW_MAX], id;

	/* a tap: lifts inside the window, down + motion go out before the up */
	glassine_pw_init(&w, 300);
	CHECK(glassine_pw_hold(&w, 1, 1000, 50, 0.1, 0.2));
	CHECK(glassine_pw_held(&w, 1));
	CHECK(glassine_pw_next_ms(&w, 1000) == 300);
	CHECK(glassine_pw_motion(&w, 1, 60, 0.11, 0.21));
	CHECK(glassine_pw_take_due(&w, 1200, &id, ev) == -1);
	int n = glassine_pw_up(&w, 1, ev);
	CHECK(n == 2);
	CHECK(ev[0].kind == GLASSINE_PW_DOWN && ev[0].time_msec == 50 && ev[0].x == 0.1 && ev[0].y == 0.2);
	CHECK(ev[1].kind == GLASSINE_PW_MOTION && ev[1].time_msec == 60 && ev[1].x == 0.11);
	CHECK(!glassine_pw_held(&w, 1));
	CHECK(glassine_pw_up(&w, 1, ev) == -1);
	CHECK(glassine_pw_next_ms(&w, 1200) == 0);

	/* a palm: the pen comes while it is held; nothing to deliver, ever */
	CHECK(glassine_pw_hold(&w, 2, 2000, 0, 0.5, 0.5));
	CHECK(glassine_pw_hold(&w, 3, 2100, 0, 0.6, 0.5));
	n = glassine_pw_pen(&w, ids, GLASSINE_PW_MAX);
	CHECK(n == 2 && ((ids[0] == 2 && ids[1] == 3) || (ids[0] == 3 && ids[1] == 2)));
	CHECK(!glassine_pw_held(&w, 2) && !glassine_pw_held(&w, 3));
	CHECK(!glassine_pw_motion(&w, 2, 0, 0, 0));
	CHECK(glassine_pw_up(&w, 2, ev) == -1);
	CHECK(glassine_pw_take_due(&w, 9000, &id, ev) == -1);
	CHECK(glassine_pw_pen(&w, ids, GLASSINE_PW_MAX) == 0);

	/* a finger: still down when the window ends; due exactly at the deadline */
	CHECK(glassine_pw_hold(&w, 4, 3000, 7, 0.3, 0.3));
	CHECK(glassine_pw_take_due(&w, 3299, &id, ev) == -1);
	CHECK(glassine_pw_next_ms(&w, 3299) == 1);
	CHECK(glassine_pw_next_ms(&w, 3400) == 1); /* overdue: still a timer */
	n = glassine_pw_take_due(&w, 3300, &id, ev);
	CHECK(n == 1 && id == 4 && ev[0].kind == GLASSINE_PW_DOWN);
	CHECK(!glassine_pw_held(&w, 4));
	CHECK(!glassine_pw_motion(&w, 4, 0, 0, 0)); /* live now: not queued */

	/* two touches, the earlier window ends first; the clock wraps */
	glassine_pw_init(&w, 300);
	CHECK(glassine_pw_hold(&w, 5, 0xFFFFFF00u, 0, 0, 0));
	CHECK(glassine_pw_hold(&w, 6, 0xFFFFFF80u, 0, 0, 0));
	CHECK(glassine_pw_next_ms(&w, 0xFFFFFF00u) == 300);
	CHECK(glassine_pw_take_due(&w, 0x2Cu, &id, ev) == 1 && id == 5); /* 0x100 + 0x2C = 300 */
	CHECK(glassine_pw_take_due(&w, 0x2Cu, &id, ev) == -1);
	CHECK(glassine_pw_next_ms(&w, 0x2Cu) == 128);
	CHECK(glassine_pw_cancel(&w, 6) && !glassine_pw_held(&w, 6));
	CHECK(!glassine_pw_cancel(&w, 6));

	/* motion overflow keeps the down and the latest position */
	CHECK(glassine_pw_hold(&w, 7, 0, 0, 0, 0));
	for (int i = 1; i <= 100; i++) {
		CHECK(glassine_pw_motion(&w, 7, i, i, i));
	}
	n = glassine_pw_up(&w, 7, ev);
	CHECK(n == GLASSINE_PW_EVMAX && ev[0].kind == GLASSINE_PW_DOWN && ev[n - 1].x == 100);

	/* the same id twice, and no room: the caller passes it on */
	CHECK(glassine_pw_hold(&w, 8, 0, 0, 0, 0));
	CHECK(!glassine_pw_hold(&w, 8, 0, 0, 0, 0));
	for (int i = 1; i < GLASSINE_PW_MAX; i++) {
		CHECK(glassine_pw_hold(&w, 100 + i, 0, 0, 0, 0));
	}
	CHECK(!glassine_pw_hold(&w, 200, 0, 0, 0, 0));
	CHECK(glassine_pw_pen(&w, ids, GLASSINE_PW_MAX) == GLASSINE_PW_MAX);

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("palm window: all checks passed\n");
	return 0;
}
