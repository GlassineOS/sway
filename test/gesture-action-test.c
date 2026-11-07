/* gesture-action-test.c — host test of sway/glassine-gesture-action.c: the
 * gestures file's ids (old and new), the defaults, and what each action sends,
 * glassine-swayd's socket faked by a datagram socket in a temporary
 * XDG_RUNTIME_DIR. Run by gesture-action-test.sh; argv[1]: an empty directory. */
#include "glassine-gesture-action.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int fails, checks;

static void eq(const char *what, const char *got, const char *want) {
	checks++;
	if (!got != !want || (got && strcmp(got, want))) {
		fails++;
		printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want ? want : "(null)");
	}
}

static const char *lookup(const char *path, int fingers, const char *what) {
	static char act[64];
	glassine_ga_lookup(path, fingers, what, act, sizeof act);
	return act;
}

static void write_file(const char *path, const char *text) {
	FILE *f = fopen(path, "w");
	if (!f || fputs(text, f) < 0 || fclose(f)) {
		perror(path);
		exit(2);
	}
}

/* fullscreen/unfullscreen: the kind and the text it gives */
static void full(const char *act, unsigned long id, int marked, int sway_full,
		enum glassine_ga_full want, const char *want_buf) {
	char buf[128] = "", what[96];
	enum glassine_ga_full r = glassine_ga_full(act, id, marked, sway_full, buf, sizeof buf);
	snprintf(what, sizeof what, "%s id %lx marked %d sway_full %d", act, id, marked, sway_full);
	checks++;
	if (r != want) {
		fails++;
		printf("FAIL %s: kind %d, want %d\n", what, r, want);
	}
	if (want_buf) {
		eq(what, buf, want_buf);
	}
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s EMPTY_DIR\n", argv[0]);
		return 2;
	}
	const char *dir = argv[1];
	char path[512], rt[512];

	/* old ids are mapped, new and unknown ones kept */
	eq("canon launcher", glassine_ga_canon("launcher"), "home");
	eq("canon ws-next", glassine_ga_canon("ws-next"), "screen-next");
	eq("canon ws-prev", glassine_ga_canon("ws-prev"), "screen-prev");
	eq("canon maximize", glassine_ga_canon("maximize"), "fullscreen");
	static const char *const same[] = { "home", "overview", "screen-next", "screen-prev", "fullscreen",
		"unfullscreen", "minimize", "close", "swap", "move-next", "move-prev", "refresh", "quick",
		"notices", "page-up", "page-down", "nothing", "right-click", "something-new" };
	for (size_t i = 0; i < sizeof same / sizeof *same; i++) {
		eq("canon keeps", glassine_ga_canon(same[i]), same[i]);
	}

	/* the defaults: no file */
	snprintf(path, sizeof path, "%s/no-such-file", dir);
	static const char *const def[][3] = {
		{ "3", "left", "screen-next" }, { "3", "right", "screen-prev" },
		{ "3", "up", "fullscreen" }, { "3", "down", "minimize" },
		{ "4", "left", "move-next" }, { "4", "right", "move-prev" },
		{ "4", "up", "home" }, { "4", "down", "refresh" },
		{ "2", "tap", "right-click" },
	};
	for (size_t i = 0; i < sizeof def / sizeof *def; i++) {
		char what[64];
		snprintf(what, sizeof what, "default %s %s", def[i][0], def[i][1]);
		eq(what, lookup(path, atoi(def[i][0]), def[i][1]), def[i][2]);
		eq(what, lookup(NULL, atoi(def[i][0]), def[i][1]), def[i][2]);
	}
	eq("default 5 up", lookup(path, 5, "up"), "nothing");
	eq("default 3 tap", lookup(path, 3, "tap"), "nothing");

	/* a file with old ids, new ids, a comment and a later line that wins */
	snprintf(path, sizeof path, "%s/gestures", dir);
	write_file(path,
		"# 4 up overview\n"
		"4 up launcher\n"
		"3 left ws-prev\n"
		"3 right ws-next\n"
		"3 up maximize\n"
		"3 down unfullscreen\n"
		"4 left overview\n"
		"4 right home\n"
		"4 right screen-next\n"
		"2 tap launcher\n"
		"garbage\n");
	eq("file 4 up launcher", lookup(path, 4, "up"), "home");
	eq("file 3 left ws-prev", lookup(path, 3, "left"), "screen-prev");
	eq("file 3 right ws-next", lookup(path, 3, "right"), "screen-next");
	eq("file 3 up maximize", lookup(path, 3, "up"), "fullscreen");
	eq("file 3 down unfullscreen", lookup(path, 3, "down"), "unfullscreen");
	eq("file 4 left overview", lookup(path, 4, "left"), "overview");
	eq("file 4 right last line", lookup(path, 4, "right"), "screen-next");
	eq("file 4 down default", lookup(path, 4, "down"), "refresh");
	eq("file 2 tap launcher", lookup(path, 2, "tap"), "home");

	/* $HOME's file */
	setenv("HOME", "/h", 1);
	glassine_ga_file(path, sizeof path);
	eq("gestures file", path, "/h/.config/glassine/gestures");

	/* the shell's requests and the other global actions */
	eq("home", glassine_ga_global("home"), "exec /usr/bin/glassine-shell-wl home");
	eq("overview", glassine_ga_global("overview"), "exec /usr/bin/glassine-shell-wl toggle overview");
	eq("quick", glassine_ga_global("quick"), "exec /usr/bin/glassine-shell-wl toggle quick");
	eq("notices", glassine_ga_global("notices"), "exec /usr/bin/glassine-shell-wl toggle notices");
	eq("screen-next", glassine_ga_global("screen-next"), "workspace next_on_output");
	eq("screen-prev", glassine_ga_global("screen-prev"), "workspace prev_on_output");
	eq("refresh", glassine_ga_global("refresh"), "exec /usr/bin/glassine-refresh");
	static const char *const local[] = { "launcher", "ws-next", "ws-prev", "maximize", "fullscreen",
		"unfullscreen", "minimize", "close", "swap", "move-next", "move-prev", "page-up", "page-down",
		"nothing", "right-click" };
	for (size_t i = 0; i < sizeof local / sizeof *local; i++) {
		eq("not global", glassine_ga_global(local[i]), NULL);
	}

	/* fullscreen/unfullscreen: swayd's `full` (a toggle) only when it changes the window */
	full("fullscreen", 0x2a, 0, 0, GLASSINE_GA_SWAYD, "full 2a");
	full("fullscreen", 0x2a, 0, 1, GLASSINE_GA_SWAYD, "full 2a");
	full("fullscreen", 0x2a, 1, 0, GLASSINE_GA_NOTHING, NULL);
	full("unfullscreen", 0xabc, 1, 0, GLASSINE_GA_SWAYD, "full abc");
	full("unfullscreen", 0xabc, 0, 0, GLASSINE_GA_NOTHING, NULL);
	full("unfullscreen", 0xabc, 0, 1, GLASSINE_GA_SWAY, "[con_id=2748] fullscreen disable");
	full("maximize", 1, 0, 0, GLASSINE_GA_NOT_FULL_ACTION, NULL);
	full("minimize", 1, 0, 0, GLASSINE_GA_NOT_FULL_ACTION, NULL);
	full("home", 1, 0, 0, GLASSINE_GA_NOT_FULL_ACTION, NULL);

	/* the socket path */
	unsetenv("XDG_RUNTIME_DIR");
	glassine_ga_swayd_path(path, sizeof path);
	eq("swayd path, no XDG_RUNTIME_DIR", path, "/tmp/glassine-swayd");
	setenv("XDG_RUNTIME_DIR", "", 1);
	glassine_ga_swayd_path(path, sizeof path);
	eq("swayd path, empty XDG_RUNTIME_DIR", path, "/tmp/glassine-swayd");
	snprintf(rt, sizeof rt, "%s/run", dir);
	if (mkdir(rt, 0700) && errno != EEXIST) {
		perror(rt);
		return 2;
	}
	setenv("XDG_RUNTIME_DIR", rt, 1);
	glassine_ga_swayd_path(path, sizeof path);
	char want[600];
	snprintf(want, sizeof want, "%s/glassine-swayd", rt);
	eq("swayd path", path, want);

	/* no glassine-swayd: an error, no hang */
	checks++;
	if (glassine_ga_send(path, "full 2a") == 0) {
		fails++;
		printf("FAIL send without swayd succeeded\n");
	}

	/* a fake glassine-swayd gets exactly the datagram */
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof a.sun_path) {
		printf("skipped: socket path too long (%s)\n", path);
		return 2;
	}
	strcpy(a.sun_path, path);
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a)) {
		perror("fake glassine-swayd");
		return 2;
	}
	char buf[128];
	int id = 0x1f3;
	enum glassine_ga_full r = glassine_ga_full("fullscreen", id, 0, 0, buf, sizeof buf);
	checks++;
	if (r != GLASSINE_GA_SWAYD || glassine_ga_send(path, buf)) {
		fails++;
		printf("FAIL send to fake swayd: %s\n", strerror(errno));
	}
	char got[256];
	ssize_t n = recv(fd, got, sizeof got - 1, MSG_DONTWAIT);
	got[n > 0 ? n : 0] = 0;
	eq("swayd datagram", got, "full 1f3");
	/* swayd parses it with sscanf("full %lx"): the same id back */
	unsigned long back = 0;
	checks++;
	if (sscanf(got, "full %lx", &back) != 1 || back != (unsigned long)id) {
		fails++;
		printf("FAIL swayd parse: %lx\n", back);
	}
	close(fd);
	unlink(path);

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
