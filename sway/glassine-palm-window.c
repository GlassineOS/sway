#include <string.h>
#include "glassine-palm-window.h"

void glassine_pw_init(struct glassine_palm_window *w, uint32_t ms) {
	memset(w, 0, sizeof *w);
	w->ms = ms;
}

static struct glassine_pw_touch *find(const struct glassine_palm_window *w, int32_t id) {
	for (int i = 0; i < GLASSINE_PW_MAX; i++) {
		if (w->t[i].used && w->t[i].id == id) {
			return (struct glassine_pw_touch *)&w->t[i];
		}
	}
	return NULL;
}

bool glassine_pw_hold(struct glassine_palm_window *w, int32_t id, uint32_t now,
		uint32_t time_msec, double x, double y) {
	if (find(w, id)) {
		return false;
	}
	for (int i = 0; i < GLASSINE_PW_MAX; i++) {
		struct glassine_pw_touch *t = &w->t[i];
		if (!t->used) {
			t->used = true;
			t->id = id;
			t->deadline = now + w->ms;
			t->n = 1;
			t->ev[0] = (struct glassine_pw_ev){ GLASSINE_PW_DOWN, time_msec, x, y };
			return true;
		}
	}
	return false;
}

bool glassine_pw_held(const struct glassine_palm_window *w, int32_t id) {
	return find(w, id) != NULL;
}

bool glassine_pw_motion(struct glassine_palm_window *w, int32_t id,
		uint32_t time_msec, double x, double y) {
	struct glassine_pw_touch *t = find(w, id);
	if (!t) {
		return false;
	}
	if (t->n == GLASSINE_PW_EVMAX) {
		t->n--; /* full: the latest position wins (the down stays) */
	}
	t->ev[t->n++] = (struct glassine_pw_ev){ GLASSINE_PW_MOTION, time_msec, x, y };
	return true;
}

static int take(struct glassine_pw_touch *t, struct glassine_pw_ev *out) {
	int n = t->n;
	memcpy(out, t->ev, n * sizeof *out);
	t->used = false;
	return n;
}

int glassine_pw_up(struct glassine_palm_window *w, int32_t id, struct glassine_pw_ev *out) {
	struct glassine_pw_touch *t = find(w, id);
	return t ? take(t, out) : -1;
}

bool glassine_pw_cancel(struct glassine_palm_window *w, int32_t id) {
	struct glassine_pw_touch *t = find(w, id);
	if (t) {
		t->used = false;
	}
	return t != NULL;
}

int glassine_pw_pen(struct glassine_palm_window *w, int32_t *ids, int max) {
	int n = 0;
	for (int i = 0; i < GLASSINE_PW_MAX; i++) {
		if (w->t[i].used) {
			if (n < max) {
				ids[n++] = w->t[i].id;
			}
			w->t[i].used = false;
		}
	}
	return n;
}

int glassine_pw_take_due(struct glassine_palm_window *w, uint32_t now, int32_t *id,
		struct glassine_pw_ev *out) {
	for (int i = 0; i < GLASSINE_PW_MAX; i++) {
		struct glassine_pw_touch *t = &w->t[i];
		if (t->used && (int32_t)(t->deadline - now) <= 0) {
			*id = t->id;
			return take(t, out);
		}
	}
	return -1;
}

uint32_t glassine_pw_next_ms(const struct glassine_palm_window *w, uint32_t now) {
	uint32_t best = 0;
	for (int i = 0; i < GLASSINE_PW_MAX; i++) {
		const struct glassine_pw_touch *t = &w->t[i];
		if (t->used) {
			int32_t left = (int32_t)(t->deadline - now);
			uint32_t ms = left < 1 ? 1 : (uint32_t)left;
			if (!best || ms < best) {
				best = ms;
			}
		}
	}
	return best;
}
