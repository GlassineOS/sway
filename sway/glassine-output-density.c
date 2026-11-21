/* glassine-output-density.c — see glassine-output-density.h */
#include "glassine-output-density.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void glassine_od_file(char *buf, size_t len)
{
	const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	if (x && *x == '/') snprintf(buf, len, "%s/glassine/output-density", x);
	else snprintf(buf, len, "%s/.config/glassine/output-density", h ? h : "");
}

static const char *part(const char *s)
{
	return s && *s ? s : "Unknown";
}

static int known(const char *s)
{
	return s && *s && strcmp(s, "Unknown");
}

void glassine_od_key(const char *make, const char *model, const char *serial, const char *name, char *buf, size_t len)
{
	if (!known(make) && !known(model) && !known(serial)) snprintf(buf, len, "%s", name ? name : "");
	else snprintf(buf, len, "%s %s %s", part(make), part(model), part(serial));
}

bool glassine_od_size_plausible(int width_px, int height_px, int width_mm, int height_mm)
{
	static const int ratio[][2] = { { 16, 9 }, { 16, 10 }, { 160, 90 }, { 160, 100 } };
	if (width_px <= 0 || height_px <= 0 || width_mm <= 0 || height_mm <= 0) return false;
	double dw = (double)width_px / width_mm, dh = (double)height_px / height_mm;
	if (dw < 1 || dw > 100 || dh < 1 || dh > 100) return false;
	for (size_t i = 0; i < sizeof ratio / sizeof ratio[0]; i++) {
		if ((width_mm == ratio[i][0] && height_mm == ratio[i][1]) ||
		    (width_mm == ratio[i][1] && height_mm == ratio[i][0])) return false;
	}
	return true;
}

/* "DENSITY" with a dot, digits only: its value, else -1 */
static double number(const char *s, size_t n)
{
	int dots = 0, digits = 0;
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '.') dots++;
		else if (s[i] >= '0' && s[i] <= '9') digits++;
		else return -1;
	}
	if (dots > 1 || !digits) return -1;
	char b[64];
	if (n >= sizeof b) return -1;
	memcpy(b, s, n);
	b[n] = 0;
	return strtod(b, NULL);
}

double glassine_od_lookup(const char *path, const char *key)
{
	FILE *f = path && key && *key ? fopen(path, "r") : NULL;
	if (!f) return 0;
	double found = 0;
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		size_t n = strcspn(line, "\r\n");
		line[n] = 0;
		if (line[0] == '#' || !line[0]) continue;
		char *sp = strchr(line, ' ');
		if (!sp || strcmp(sp + 1, key)) continue;
		double d = number(line, (size_t)(sp - line));
		if (d >= 1 && d <= 100) found = d;
	}
	fclose(f);
	return found;
}
