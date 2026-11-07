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

const char *glassine_ga_global(const char *act) {
	static const char *const global[][2] = {
		{ "refresh", "exec /usr/bin/glassine-refresh" },
		{ "screen-next", "workspace next_on_output" },
		{ "screen-prev", "workspace prev_on_output" },
		{ "home", "exec /usr/bin/glassine-shell-wl home" },
		{ "overview", "exec /usr/bin/glassine-shell-wl toggle overview" },
		{ "quick", "exec /usr/bin/glassine-shell-wl toggle quick" },
		{ "notices", "exec /usr/bin/glassine-shell-wl toggle notices" },
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
		if (!marked) {
			return GLASSINE_GA_NOT_FULL_ACTION;
		}
		on = 0;
		act = "unfullscreen";
	}
	if (!on && strcmp(act, "unfullscreen")) {
		return GLASSINE_GA_NOT_FULL_ACTION;
	}
	/* swayd's `full W` toggles: sent only when it changes the window the way asked */
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
