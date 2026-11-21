/* output-density-test.c — sway/glassine-output-density.c: the output key and the file */
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
	printf(fails ? "output-density: FAILED\n" : "output-density: all cases passed\n");
	return fails != 0;
}
