/* See LICENSE file for copyright and license details. */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "ini.h"
#include "utils.h"

int
ini_parse(FILE *f, const char *name, IniHandler cb, void *user, int quiet)
{
	char line[512], sec[64] = "";
	int ln = 0;

	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		char *e, *eq;
		int rc;

		ln++;

		if (!*s || *s == '#' || *s == ';')
			continue;

		if (*s == '[') {
			if (!(e = strchr(s, ']'))) {
				if (!quiet)
					warn("%s:%d: unterminated section header", name, ln);
				return -1;
			}
			*e = '\0';
			snprintf(sec, sizeof(sec), "%s", trim(s + 1));

			if (cb(user, sec, NULL, NULL) != INI_OK)
				return -1;
			continue;
		}

		if (!(eq = strchr(s, '='))) {
			if (!quiet)
				warn("%s:%d: expected key = value", name, ln);
			continue;
		}

		*eq = '\0';
		s = trim(s);

		rc = cb(user, sec, s, trim(eq + 1));
		if (rc == INI_ERROR)
			return -1;
		if (rc == INI_UNKNOWN && !quiet)
			warn("%s:%d: unknown key \"%s\"", name, ln, s);
	}

	return 0;
}
