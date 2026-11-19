/* glassine-gesture-action.c — see glassine-gesture-action.h */
#include "glassine-gesture-action.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

const char *glassine_ga_canon(const char *id) {
	static const char *const old[][2] = {
		{ "launcher", "home" },
		{ "ws-next", "screen-next" },
		{ "ws-prev", "screen-prev" },
		{ "maximize", "fullscreen" },
	};
	for (size_t i = 0; i < sizeof old / sizeof *old; i++) {
		if (!strcmp(id, old[i][0])) {
			return old[i][1];
		}
	}
	return id;
}

const char *glassine_ga_default(int fingers, const char *what) {
	static const char *const dirs[4] = { "left", "right", "up", "down" };
	static const char *const dflt[2][4] = {
		{ "screen-next", "screen-prev", "fullscreen", "minimize" },
		{ "move-next", "move-prev", "home", "refresh" },
	};
	if (fingers == 2) {
		return strcmp(what, "tap") ? NULL : "right-click";
	}
	if (fingers != 3 && fingers != 4) {
		return NULL;
	}
	for (int i = 0; i < 4; i++) {
		if (!strcmp(what, dirs[i])) {
			return dflt[fingers == 4][i];
		}
	}
	return NULL;
}

void glassine_ga_file(char *path, size_t len) {
	const char *home = getenv("HOME");
	snprintf(path, len, "%s/.config/glassine/gestures", home ? home : "");
}

void glassine_ga_lookup(const char *path, int fingers, const char *what, char *act, size_t len) {
	const char *d = glassine_ga_default(fingers, what);
	char a[64] = "", line[256], w[16], v[64];
	int n;
	FILE *f = path ? fopen(path, "r") : NULL;
	if (f) {
		while (fgets(line, sizeof line, f)) {
			if (line[0] != '#' && sscanf(line, "%d %15s %63s", &n, w, v) == 3
					&& n == fingers && !strcmp(w, what)) {
				snprintf(a, sizeof a, "%s", v);
			}
		}
		fclose(f);
	}
	snprintf(act, len, "%s", glassine_ga_canon(*a ? a : d ? d : "nothing"));
}

const char *glassine_ga_edge_default(const char *edge) {
	if (!strcmp(edge, "top")) {
		return "show-bar";
	}
	if (!strcmp(edge, "bottom") || !strcmp(edge, "left") || !strcmp(edge, "right")) {
		return "nothing";
	}
	return NULL;
}

/* the edge's line as it is (no exit guard) */
static void edge_lookup(const char *path, const char *edge, char *act, size_t len) {
	const char *d = glassine_ga_edge_default(edge);
	char a[64] = "", line[256], k[16], w[16], v[64];
	FILE *f = path ? fopen(path, "r") : NULL;
	if (f) {
		while (fgets(line, sizeof line, f)) {
			if (line[0] != '#' && sscanf(line, "%15s %15s %63s", k, w, v) == 3
					&& !strcmp(k, "edge") && !strcmp(w, edge)) {
				snprintf(a, sizeof a, "%s", v);
			}
		}
		fclose(f);
	}
	snprintf(act, len, "%s", glassine_ga_canon(*a ? a : d ? d : "nothing"));
}

int glassine_ga_edge_armed(const char *act, int immersive) {
	if (!strcmp(act, "nothing")) {
		return 0;
	}
	return strcmp(act, "show-bar") || immersive;
}

int glassine_ga_is_exit(const char *act) {
	static const char *const exits[] = { "unfullscreen", "minimize", "home", "overview", "show-bar" };
	act = glassine_ga_canon(act);
	for (size_t i = 0; i < sizeof exits / sizeof *exits; i++) {
		if (!strcmp(act, exits[i])) {
			return 1;
		}
	}
	return 0;
}

int glassine_ga_has_exit(const char *path) {
	static const char *const dirs[4] = { "left", "right", "up", "down" };
	static const char *const edges[4] = { "top", "bottom", "left", "right" };
	char act[64];
	for (int fingers = 3; fingers <= 4; fingers++) {
		for (int i = 0; i < 4; i++) {
			glassine_ga_lookup(path, fingers, dirs[i], act, sizeof act);
			if (glassine_ga_is_exit(act)) {
				return 1;
			}
		}
	}
	glassine_ga_lookup(path, 2, "tap", act, sizeof act);
	if (glassine_ga_is_exit(act)) {
		return 1;
	}
	for (int i = 0; i < 4; i++) {
		edge_lookup(path, edges[i], act, sizeof act);
		if (glassine_ga_is_exit(act)) {
			return 1;
		}
	}
	return 0;
}

void glassine_ga_edge_lookup(const char *path, const char *edge, char *act, size_t len) {
	edge_lookup(path, edge, act, len);
	if (!strcmp(edge, "top") && !glassine_ga_is_exit(act) && !glassine_ga_has_exit(path)) {
		snprintf(act, len, "%s", "show-bar");
	}
}

const char *glassine_ga_global(const char *act) {
	static const char *const global[][2] = {
		{ "refresh", "exec /usr/bin/glassine-refresh" },
		{ "screen-next", "workspace next_on_output" },
		{ "screen-prev", "workspace prev_on_output" },
		{ "home", "exec /usr/bin/glassine-shell home" },
		{ "overview", "exec /usr/bin/glassine-shell toggle overview" },
		{ "quick", "exec /usr/bin/glassine-shell toggle quick" },
		{ "notices", "exec /usr/bin/glassine-shell toggle notices" },
		{ "show-bar", "exec /usr/bin/glassine-shell bar show" },
	};
	for (size_t i = 0; i < sizeof global / sizeof *global; i++) {
		if (!strcmp(act, global[i][0])) {
			return global[i][1];
		}
	}
	return NULL;
}

enum glassine_ga_full glassine_ga_full(const char *act, unsigned long con_id, int marked, int sway_full,
		char *buf, size_t len) {
	int on = !strcmp(act, "fullscreen");
	if (!strcmp(act, "minimize")) {
		if (!marked && !sway_full) {
			return GLASSINE_GA_NOT_FULL_ACTION;
		}
		on = 0;
		act = "unfullscreen";
	}
	if (!on && strcmp(act, "unfullscreen")) {
		return GLASSINE_GA_NOT_FULL_ACTION;
	}
	/* swayd's `full W` toggles sway's own fullscreen of W (an older swayd: its
	 * mark, GLASSINE_GA_FULL_MARK). So it is sent only when that is what was asked:
	 * `fullscreen` on a window in sway's fullscreen does nothing (it is full screen
	 * already), and unfullscreen/minimize of one is sway's `fullscreen disable`;
	 * a window still carrying the old mark goes back through swayd */
	if (on && sway_full && !marked) {
		return GLASSINE_GA_NOTHING;
	}
	if (on ? !marked : marked) {
		snprintf(buf, len, "full %lx", con_id);
		return GLASSINE_GA_SWAYD;
	}
	if (!on && sway_full) {
		snprintf(buf, len, "[con_id=%lu] fullscreen disable", con_id);
		return GLASSINE_GA_SWAY;
	}
	return GLASSINE_GA_NOTHING;
}

void glassine_ga_swayd_path(char *path, size_t len) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	snprintf(path, len, "%s/glassine-swayd", rt && *rt ? rt : "/tmp");
}

int glassine_ga_send(const char *path, const char *msg) {
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof a.sun_path) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(a.sun_path, path);
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	ssize_t r = sendto(fd, msg, strlen(msg), MSG_DONTWAIT, (struct sockaddr *)&a, sizeof a);
	int e = errno;
	close(fd);
	errno = e;
	return r < 0 ? -1 : 0;
}
