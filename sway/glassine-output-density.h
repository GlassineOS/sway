/* glassine-output-density.h — the person's density of an output of unknown size
 * (GlassineOS/handwriting spec 004 FR-017; written by Settings, GlassineOS/shell).
 *
 * The file is $XDG_CONFIG_HOME/glassine/output-density (default ~/.config/glassine/):
 * one line per output, "DENSITY OUTPUT-KEY": the output's pixels per millimetre, its
 * current mode's pixels as the mode is (not turned, not scaled) over its width in mm, a
 * decimal number with a dot from 1 to 100; one space; the output key to the end of the
 * line. "#" lines and blank lines are comments; a line whose density is out of range or
 * no number is ignored; of two lines for one key the later counts. The output key is
 * the output's make, model and serial joined by single spaces ("Unknown" for one that
 * is missing, as sway names them), or its connector name when all three are missing.
 * libc only. */
#ifndef GLASSINE_OUTPUT_DENSITY_H
#define GLASSINE_OUTPUT_DENSITY_H
#include <stddef.h>

/* the file's path into buf (XDG_CONFIG_HOME, else HOME/.config) */
void glassine_od_file(char *buf, size_t len);

/* the output key into buf; make, model, serial may be NULL or "" */
void glassine_od_key(const char *make, const char *model, const char *serial, const char *name, char *buf,
		     size_t len);

/* the density the file at path keeps for key; 0: none */
double glassine_od_lookup(const char *path, const char *key);

#endif
