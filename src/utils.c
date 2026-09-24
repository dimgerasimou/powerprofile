/* See LICENSE file for copyright and license details. */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "utils.h"

int verbose = 0;

static const char *program_name = "powerprofile";

void
set_name(const char *name)
{
	program_name = name;
}

const char *
get_name(void)
{
	return program_name;
}

void
die(const char *fmt, ...)
{
	va_list ap;
	int saved_errno = errno;

	fprintf(stderr, "%s: ", program_name);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] && fmt[strlen(fmt) - 1] == ':')
		fprintf(stderr, " %s", strerror(saved_errno));
	fputc('\n', stderr);

	exit(1);
}

void
warn(const char *fmt, ...)
{
	va_list ap;
	int saved_errno = errno;

	fprintf(stderr, "%s: ", program_name);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] && fmt[strlen(fmt) - 1] == ':')
		fprintf(stderr, " %s", strerror(saved_errno));
	fputc('\n', stderr);
}

void
vinfo(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void
argerr(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", program_name);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fprintf(stderr, "\nTry '%s --help' for more information.\n", program_name);
	exit(1);
}

char *
trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;

	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';

	return s;
}

int
readfile(const char *path, char *buf, size_t bufsz)
{
	FILE *f;

	buf[0] = '\0';

	if (!(f = fopen(path, "r")))
		return -1;

	if (!fgets(buf, (int)bufsz, f))
		buf[0] = '\0';
	fclose(f);

	buf[strcspn(buf, "\n")] = '\0';

	return buf[0] ? 0 : -1;
}
