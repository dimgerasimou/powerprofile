/* powerprofile-x
 * Copyright (c) 2026 Dimitris Gerasimou
 * Licensed under the GNU General Public License v3.
 *
 * User half of powerprofile. Runs inside your X session, follows the
 * profile powerprofile applied last, and sets the refresh rate of the
 * internal panel from the profile's `refresh =` setting.
 *
 * It needs no privileges, no XAUTHORITY and no output name: it uses your
 * own X connection and finds the internal panel by itself. The rate is only
 * touched when the profile changes, so a manual xrandr change stays.
 *
 * To understand everything, start reading main().
 */

#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <getopt.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <unistd.h>

#include "ini.h"
#include "utils.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

/* Output names, by prefix, treated as the internal panel. */
static const char *const internal_prefixes[] = { "eDP", "LVDS", "DSI" };

enum {
	OPT_ONCE = 1000,
	OPT_CONFIG,
	OPT_STATE,
	OPT_OUTPUT,
	OPT_VERBOSE,
	OPT_VERSION,
	OPT_HELP,
};

typedef struct {
	int once;
	const char *config; /* --config */
	const char *state;  /* --state */
	const char *output; /* --output */
} Options;

/* What conf_cb() looks for and where it leaves it: the refresh of the
 * profile, and the one of [default] to fall back on.
 */
typedef struct {
	const char *profile;
	char *out;
	size_t outsz;
	char def[16];
	int found, def_found;
} Query;

static const char *forced; /* --output */

static int                conf_cb(void *user, const char *sec, const char *key, const char *val);
static int                conf_refresh(const char *conf, const char *profile, char *out, size_t outsz);
static int                xerr(Display *dpy, XErrorEvent *ev);
static double             rate_of(const XRRModeInfo *m);
static double             dist(double a, double b);
static const XRRModeInfo *mode_by_id(const XRRScreenResources *res, RRMode id);
static int                is_internal(const char *name);
static void               apply_refresh(Display *dpy, const char *want);
static void               sync_profile(Display *dpy, const Options *o, char *applied, size_t appliedsz);
static void               usage(void);
static void               options_parse(Options *o, int argc, char *argv[]);

static int
conf_cb(void *user, const char *sec, const char *key, const char *val)
{
	Query *q = user;

	if (!key || strcmp(key, "refresh") != 0)
		return INI_OK;

	if (!q->found && strcmp(sec, q->profile) == 0) {
		snprintf(q->out, q->outsz, "%s", val);
		q->found = 1;
	} else if (!q->def_found && strcmp(sec, "default") == 0) {
		snprintf(q->def, sizeof(q->def), "%s", val);
		q->def_found = 1;
	}

	return INI_OK;
}

/* Puts the `refresh =` of [profile] in out, or the one of [default] if the
 * profile has none. Returns 0, or -1 if neither does.
 */
static int
conf_refresh(const char *conf, const char *profile, char *out, size_t outsz)
{
	Query q = { profile, out, outsz, "", 0, 0 };
	FILE *f;

	if (!(f = fopen(conf, "r")))
		return -1;

	ini_parse(f, conf, conf_cb, &q, 1);
	fclose(f);

	if (!q.found && q.def_found) {
		snprintf(out, outsz, "%s", q.def);
		q.found = 1;
	}

	return q.found ? 0 : -1;
}

/* A failed request must not kill the helper. */
static int
xerr(Display *dpy, XErrorEvent *ev)
{
	(void)dpy;
	(void)ev;

	return 0;
}

static double
rate_of(const XRRModeInfo *m)
{
	double vtotal = m->vTotal;

	if (m->modeFlags & RR_DoubleScan)
		vtotal *= 2;
	if (m->modeFlags & RR_Interlace)
		vtotal /= 2;

	return (m->hTotal && vtotal) ? (double)m->dotClock / ((double)m->hTotal * vtotal) : 0;
}

static double
dist(double a, double b)
{
	return a > b ? a - b : b - a;
}

static const XRRModeInfo *
mode_by_id(const XRRScreenResources *res, RRMode id)
{
	for (int i = 0; i < res->nmode; i++)
		if (res->modes[i].id == id)
			return &res->modes[i];

	return NULL;
}

static int
is_internal(const char *name)
{
	for (size_t i = 0; i < sizeof(internal_prefixes) / sizeof(*internal_prefixes); i++)
		if (strncmp(name, internal_prefixes[i], strlen(internal_prefixes[i])) == 0)
			return 1;

	return 0;
}

/* Switches the internal panel to `want`, "max" or a rate in Hz, keeping its
 * current resolution: "max" picks the highest rate, a number the closest.
 */
static void
apply_refresh(Display *dpy, const char *want)
{
	XRRScreenResources *res;
	double target = strcmp(want, "max") == 0 ? 0 : atof(want);
	int done = 0;

	if (!(res = XRRGetScreenResourcesCurrent(dpy, DefaultRootWindow(dpy))))
		return;

	for (int i = 0; i < res->noutput && !done; i++) {
		XRROutputInfo *o = XRRGetOutputInfo(dpy, res, res->outputs[i]);
		const XRRModeInfo *cur, *best = NULL;
		XRRCrtcInfo *c;
		double best_rate = 0;

		if (!o)
			continue;

		if (o->connection != RR_Connected || !o->crtc ||
		    !(forced ? strcmp(o->name, forced) == 0 : is_internal(o->name))) {
			XRRFreeOutputInfo(o);
			continue;
		}

		done = 1;
		c = XRRGetCrtcInfo(dpy, res, o->crtc);
		cur = c ? mode_by_id(res, c->mode) : NULL;

		for (int j = 0; cur && j < o->nmode; j++) {
			const XRRModeInfo *m = mode_by_id(res, o->modes[j]);
			double r;

			if (!m || m->width != cur->width || m->height != cur->height)
				continue;

			r = rate_of(m);
			if (!best || (target > 0 ? dist(r, target) < dist(best_rate, target)
			                         : r > best_rate)) {
				best = m;
				best_rate = r;
			}
		}

		if (best && best->id != c->mode) {
			vinfo("%s: %.2f Hz -> %.2f Hz", o->name, rate_of(cur), best_rate);
			XRRSetCrtcConfig(dpy, res, o->crtc, CurrentTime, c->x, c->y,
			                 best->id, c->rotation, c->outputs, c->noutput);
		} else {
			vinfo("%s: already at %.2f Hz", o->name, cur ? rate_of(cur) : 0);
		}

		if (c)
			XRRFreeCrtcInfo(c);
		XRRFreeOutputInfo(o);
	}

	if (!done)
		vinfo("no internal panel found (eDP, LVDS or DSI); try --output");

	XRRFreeScreenResources(res);
	XFlush(dpy);
}

/* Applies the refresh of the current profile if its name changed since the
 * last time; `applied` remembers it.
 */
static void
sync_profile(Display *dpy, const Options *o, char *applied, size_t appliedsz)
{
	char profile[32], want[16];

	if (readfile(o->state, profile, sizeof(profile)) < 0 || strcmp(profile, applied) == 0)
		return;

	snprintf(applied, appliedsz, "%s", profile);

	if (conf_refresh(o->config, profile, want, sizeof(want)) == 0 && want[0]) {
		vinfo("profile %s: refresh = %s", profile, want);
		apply_refresh(dpy, want);
	} else {
		vinfo("profile %s: no refresh setting, leaving it alone", profile);
	}
}

static void
usage(void)
{
	printf("usage: %s [options]\n%s", get_name(),
	      "\tOptions:\n"
	      "\t\t[--help][--version]\n"
	      "\t\t[--once][--config file][--state file][--output name][--verbose]\n"
	      "\n"
	      "--help              Print this message and exit\n"
	      "--version           Print version and exit\n"
	      "--once              Apply the current profile once and exit\n"
	      "--config FILE       Read FILE instead of " CONF_PATH "\n"
	      "--state FILE        Follow FILE instead of " LAST_PATH "\n"
	      "--output NAME       Use this output instead of the internal panel\n"
	      "--verbose           Log what is being done to stderr\n"
	      "\n"
	      "Start it from your X session; it exits when X does.\n");
}

static void
options_parse(Options *o, int argc, char *argv[])
{
	o->once = 0;
	o->config = CONF_PATH;
	o->state = LAST_PATH;
	o->output = NULL;

	struct option longopts[] = {
		{ "once",    no_argument,       0, OPT_ONCE    },
		{ "config",  required_argument, 0, OPT_CONFIG  },
		{ "state",   required_argument, 0, OPT_STATE   },
		{ "output",  required_argument, 0, OPT_OUTPUT  },
		{ "verbose", no_argument,       0, OPT_VERBOSE },
		{ "version", no_argument,       0, OPT_VERSION },
		{ "help",    no_argument,       0, OPT_HELP    },
		{ 0,         0,                 0, 0           },
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
		switch (opt) {
		case OPT_ONCE:
			o->once = 1;
			break;

		case OPT_CONFIG:
			o->config = optarg;
			break;

		case OPT_STATE:
			o->state = optarg;
			break;

		case OPT_OUTPUT:
			o->output = optarg;
			break;

		case OPT_VERBOSE:
			verbose = 1;
			break;

		case OPT_VERSION:
			printf("%s %s\n", get_name(), VERSION);
			exit(0);

		case OPT_HELP:
			usage();
			exit(0);

		default:
			argerr("unknown option");
		}
	}

	if (optind < argc)
		argerr("unexpected argument \"%s\"", argv[optind]);
}

int
main(int argc, char *argv[])
{
	Options o;
	Display *dpy;
	XEvent xev;
	union {
		char buf[4096];
		struct inotify_event align;
	} u;
	struct pollfd pfd[2];
	char applied[32] = "";
	int wd = -1;

	set_name("powerprofile-x");

	options_parse(&o, argc, argv);
	forced = o.output;

	if (!(dpy = XOpenDisplay(NULL)))
		die("cannot open display (run it inside your X session)");
	XSetErrorHandler(xerr);

	sync_profile(dpy, &o, applied, sizeof(applied));
	if (o.once)
		return 0;

	if ((pfd[0].fd = inotify_init1(IN_CLOEXEC)) < 0)
		die("inotify_init1:");
	pfd[0].events = POLLIN;
	pfd[1].fd = ConnectionNumber(dpy);
	pfd[1].events = POLLIN;

	for (;;) {
		/* (Re)attach the watch: the file may not exist yet at login. */
		if (wd < 0 && (wd = inotify_add_watch(pfd[0].fd, o.state,
		                                      IN_CLOSE_WRITE | IN_DELETE_SELF)) >= 0)
			sync_profile(dpy, &o, applied, sizeof(applied));

		if (poll(pfd, 2, wd < 0 ? 2000 : -1) <= 0)
			continue;

		/* The server going away shows up as readable; XPending() then
		 * exits through Xlib's own IO error handler.
		 */
		if (pfd[1].revents)
			while (XPending(dpy))
				XNextEvent(dpy, &xev);

		if (pfd[0].revents & POLLIN) {
			ssize_t n = read(pfd[0].fd, u.buf, sizeof(u.buf));
			size_t off = 0;

			while (n > 0 && off < (size_t)n) {
				const struct inotify_event *ev = (const void *)(u.buf + off);

				if (ev->mask & (IN_IGNORED | IN_DELETE_SELF))
					wd = -1;
				off += sizeof(*ev) + ev->len;
			}
			sync_profile(dpy, &o, applied, sizeof(applied));
		}
	}
}
