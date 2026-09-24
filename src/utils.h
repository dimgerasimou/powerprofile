/* See LICENSE file for copyright and license details. */

#ifndef POWERPROFILE_UTILS_H
#define POWERPROFILE_UTILS_H

#include <stddef.h>
#include <string.h>

/* Locations shared by powerprofile and powerprofile-x. Overridable so the
 * test build can run against a fake tree.
 */
#ifndef RUNDIR
#define RUNDIR "/run"
#endif

#ifndef CONF_PATH
#define CONF_PATH "/etc/powerprofile.conf"
#endif

/* The profile powerprofile applied last; powerprofile-x watches it. */
#define LAST_PATH RUNDIR "/powerprofile-last"

extern int verbose;

void set_name(const char *name);
const char *get_name(void);

/*
 * Prints formated message to stderr and exits.
 * If last char is ':', prints strerror with set errno.
 */
_Noreturn void die(const char *fmt, ...);

/*
 * Prints formated message to stderr and returns.
 * If last char is ':', prints strerror with set errno.
 */
void warn(const char *fmt, ...);

/* Prints verbose info. */
void vinfo(const char *fmt, ...);

/* Prints formated message with prompt for '--help' */
_Noreturn void argerr(const char *fmt, ...);

/* Strips leading and trailing whitespace in place. */
char *trim(char *s);

/*
 * Reads the first line of `path` into `buf`, newline stripped.
 *
 * Returns 0 on success, -1 if the file is unreadable or empty.
 */
int readfile(const char *path, char *buf, size_t bufsz);

#endif /* POWERPROFILE_UTILS_H */
