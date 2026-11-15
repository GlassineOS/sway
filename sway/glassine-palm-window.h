#ifndef GLASSINE_PALM_WINDOW_H
#define GLASSINE_PALM_WINDOW_H
/*
 * The writing paper's decision window (no wlroots; the glue is in
 * input/cursor.c). A touch that lands outside the input method's surfaces
 * while the paper is up is held for a short window, with its motion queued:
 *  - the pen comes (proximity or contact) while it is held: a palm, dropped
 *    (glassine_pw_pen; the glue swallows the rest of it until it lifts);
 *  - it lifts while held: a tap, its down and motion go out before the up
 *    (glassine_pw_up);
 *  - the window ends while it is down: a finger, its down and motion go out
 *    now and the rest live (glassine_pw_take_due).
 * Times: "now" is the glue's monotonic clock in ms; an event's own time is
 * only stored, for the replay.
 */
#include <stdbool.h>
#include <stdint.h>

#define GLASSINE_PW_MAX 8     /* touches held at once (more pass through) */
#define GLASSINE_PW_EVMAX 32  /* events queued per touch (later motion replaces the last) */

enum glassine_pw_kind { GLASSINE_PW_DOWN, GLASSINE_PW_MOTION };

struct glassine_pw_ev {
	enum glassine_pw_kind kind;
	uint32_t time_msec;
	double x, y; /* the device's coordinates, as the event had them */
};

struct glassine_pw_touch {
	bool used;
	int32_t id;
	uint32_t deadline;
	int n;
	struct glassine_pw_ev ev[GLASSINE_PW_EVMAX];
};

struct glassine_palm_window {
	uint32_t ms;
	struct glassine_pw_touch t[GLASSINE_PW_MAX];
};

void glassine_pw_init(struct glassine_palm_window *w, uint32_t ms);
/* hold a new touch; false: no room (or already held), the caller passes it on */
bool glassine_pw_hold(struct glassine_palm_window *w, int32_t id, uint32_t now,
	uint32_t time_msec, double x, double y);
bool glassine_pw_held(const struct glassine_palm_window *w, int32_t id);
/* queue a motion of a held touch; false: not held */
bool glassine_pw_motion(struct glassine_palm_window *w, int32_t id,
	uint32_t time_msec, double x, double y);
/* a held touch lifted (a tap): its events to deliver before the up, in out
 * (GLASSINE_PW_EVMAX); -1: not held */
int glassine_pw_up(struct glassine_palm_window *w, int32_t id, struct glassine_pw_ev *out);
/* a held touch was cancelled: dropped; false: not held */
bool glassine_pw_cancel(struct glassine_palm_window *w, int32_t id);
/* the pen came: every held touch is a palm, dropped; their ids in ids */
int glassine_pw_pen(struct glassine_palm_window *w, int32_t *ids, int max);
/* one touch whose window has ended (a finger): its id and events to deliver
 * now, in out (GLASSINE_PW_EVMAX); -1: none due */
int glassine_pw_take_due(struct glassine_palm_window *w, uint32_t now, int32_t *id,
	struct glassine_pw_ev *out);
/* ms until the next window ends (at least 1); 0: nothing held */
uint32_t glassine_pw_next_ms(const struct glassine_palm_window *w, uint32_t now);

#endif
