/* output-density-test.c — sway/glassine-output-density.c: the output key, the file, and
 * which reported sizes count */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "glassine-output-density.h"

static int fails;

static void check(const char *what, int ok)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) fails++;
}

static void put(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	if (!f) { perror(path); exit(1); }
	fputs(text, f);
	fclose(f);
}

int main(int argc, char **argv)
{
	char k[256], p[4096];
	if (argc < 2) return 2;
	glassine_od_key("Acme", "Panel 13", "X1", "DP-1", k, sizeof k);
	check("key: make, model and serial", !strcmp(k, "Acme Panel 13 X1"));
	glassine_od_key("Acme", NULL, "", "DP-1", k, sizeof k);
	check("key: a missing part is Unknown", !strcmp(k, "Acme Unknown Unknown"));
	glassine_od_key(NULL, "Unknown", "", "HDMI-A-1", k, sizeof k);
	check("key: all three missing, the connector name", !strcmp(k, "HDMI-A-1"));

	snprintf(p, sizeof p, "%s/output-density", argv[1]);
	check("no file: none", glassine_od_lookup(p, "HDMI-A-1") == 0);
	put(p, "# measured\n\n8.5 HDMI-A-1\n3.78 Acme Panel 13 X1\n200 DP-2\nabc DP-3\n4,5 DP-4\n-3 DP-5\n"
	       "9.25 HDMI-A-1\n1.5.2 DP-6\n0.5 DP-7\n7 DP-8\n");
	check("the later of two lines", glassine_od_lookup(p, "HDMI-A-1") == 9.25);
	check("a key with spaces", glassine_od_lookup(p, "Acme Panel 13 X1") == 3.78);
	check("out of range: ignored", glassine_od_lookup(p, "DP-2") == 0 && glassine_od_lookup(p, "DP-7") == 0);
	check("no number: ignored", glassine_od_lookup(p, "DP-3") == 0 && glassine_od_lookup(p, "DP-4") == 0 &&
	      glassine_od_lookup(p, "DP-5") == 0 && glassine_od_lookup(p, "DP-6") == 0);
	check("a whole number", glassine_od_lookup(p, "DP-8") == 7);
	check("a key not in it", glassine_od_lookup(p, "Acme Panel 13") == 0);

	setenv("XDG_CONFIG_HOME", "/x/cfg", 1);
	glassine_od_file(p, sizeof p);
	check("the file under XDG_CONFIG_HOME", !strcmp(p, "/x/cfg/glassine/output-density"));
	unsetenv("XDG_CONFIG_HOME");
	setenv("HOME", "/x/home", 1);
	glassine_od_file(p, sizeof p);
	check("else under HOME/.config", !strcmp(p, "/x/home/.config/glassine/output-density"));

	/* a reported size counts only when plausible (GlassineOS/shell spec 012 FR-006) */
	static const struct { int wpx, hpx, wmm, hmm, ok; const char *what; } v[] = {
		{ 1404, 1872, 157, 209, 1, "a tablet's panel, 8.9 px per mm" },
		{ 1872, 1404, 209, 157, 1, "the same, landscape" },
		{ 1920, 1080, 527, 296, 1, "a 24 inch monitor" },
		{ 3840, 2160, 597, 336, 1, "a 27 inch 4K monitor" },
		{ 1920, 1080, 0, 0, 0, "no size" },
		{ 1920, 1080, 527, 0, 0, "no height" },
		{ 1920, 1080, 0, 296, 0, "no width" },
		{ 1920, 1080, -1, 296, 0, "a negative width" },
		{ 1920, 1080, 160, 90, 0, "16:9 in centimetres as mm x 10" },
		{ 1920, 1200, 160, 100, 0, "16:10 in centimetres as mm x 10" },
		{ 1920, 1080, 16, 9, 0, "16:9 as the size" },
		{ 1920, 1200, 16, 10, 0, "16:10 as the size" },
		{ 1080, 1920, 90, 160, 0, "16:9, turned" },
		{ 1200, 1920, 100, 160, 0, "16:10, turned" },
		{ 1080, 1920, 9, 16, 0, "9:16 as the size" },
		{ 1200, 1920, 10, 16, 0, "10:16 as the size" },
		{ 1920, 1080, 1600, 900, 1, "a 1.6 m wide wall display, 1.2 px per mm" },
		{ 1920, 1080, 2000, 1125, 0, "under 1 px per mm" },
		{ 1920, 1080, 19, 11, 0, "over 100 px per mm" },
		{ 1920, 1080, 1920, 1080, 1, "1 px per mm, the lower bound" },
		{ 1900, 1000, 19, 10, 1, "100 px per mm, the upper bound" },
		{ 1920, 1080, 527, 5, 0, "a plausible width, a height over 100 px per mm" },
		{ 1920, 1080, 10, 296, 0, "a plausible height, a width over 100 px per mm" },
		{ 1280, 720, 160, 90, 0, "16:9 in centimetres, a density that would be plausible" },
		{ 0, 1080, 527, 296, 0, "no mode" },
	};
	for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
		char what[128];
		snprintf(what, sizeof what, "size %s: %s", v[i].ok ? "counts" : "refused", v[i].what);
		check(what, glassine_od_size_plausible(v[i].wpx, v[i].hpx, v[i].wmm, v[i].hmm) == v[i].ok);
	}
	printf(fails ? "output-density: FAILED\n" : "output-density: all cases passed\n");
	return fails != 0;
}
