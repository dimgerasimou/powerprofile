/* See LICENSE file for copyright and license details. */

#ifndef POWERPROFILE_INI_H
#define POWERPROFILE_INI_H

#include <stdio.h>

/* What an IniHandler returns for a key. */
enum {
	INI_OK      = 0,
	INI_UNKNOWN = 1,  /* key not recognised: warn and carry on */
	INI_ERROR   = -1, /* the handler already reported it: stop */
};

/*
 * Called for every section header (key == NULL) and every key = value line.
 * A non-zero return for a section header stops the parse; the handler is
 * expected to have said why.
 */
typedef int (*IniHandler)(void *user, const char *section, const char *key,
                          const char *value);

/*
 * Reads INI text from `f`: [section] headers and key = value lines, with
 * full-line comments starting with '#' or ';'. Keys before the first section
 * get an empty section name. `name` only appears in messages; `quiet`
 * suppresses them.
 *
 * Returns 0, or -1 on a malformed section header or when the handler
 * stopped the parse.
 */
int ini_parse(FILE *f, const char *name, IniHandler cb, void *user, int quiet);

#endif /* POWERPROFILE_INI_H */
