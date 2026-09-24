/* powerprofile
 * Copyright (c) 2026 Dimitris Gerasimou
 * Licensed under the GNU General Public License v3.
 *
 * Power profile switcher for MSI laptops. One config file, any number of
 * profiles; a profile sets CPU EPP, turbo and RAPL limits, PCIe ASPM,
 * Wi-Fi power saving, the embedded controller (through the msi-ec driver),
 * nvidia-powerd, and the screen brightness.
 *
 * Runs as root and never talks to X: the refresh rate is applied by
 * powerprofile-x, which follows the profile name recorded in LAST_PATH.
 *
 * To understand everything, start reading main().
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <glob.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ec.h"
#include "ini.h"
#include "utils.h"

#ifndef VERSION
#define VERSION "unknown"
#endif

/* Overridable so the test build can point at a fake tree and run without
 * privileges.
 */
#ifndef SYSFS
#define SYSFS "/sys"
#endif

#ifndef REQUIRE_ROOT
#define REQUIRE_ROOT 1
#endif

#define OVERRIDE_PATH  RUNDIR "/powerprofile-override"
#define RAPL_SAVE_PATH RUNDIR "/powerprofile-rapl"

#define CPU_EPP      SYSFS "/devices/system/cpu/cpu[0-9]*/cpufreq/energy_performance_preference"
#define CPU0_EPP     SYSFS "/devices/system/cpu/cpu0/cpufreq/energy_performance_preference"
#define CPU0_DRIVER  SYSFS "/devices/system/cpu/cpu0/cpufreq/scaling_driver"
#define CPU_GOV      SYSFS "/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor"
#define CPU0_GOV     SYSFS "/devices/system/cpu/cpu0/cpufreq/scaling_governor"
#define CPU0_GOVS    SYSFS "/devices/system/cpu/cpu0/cpufreq/scaling_available_governors"
#define CPU_EPB      SYSFS "/devices/system/cpu/cpu[0-9]*/power/energy_perf_bias"
#define CPU0_EPB     SYSFS "/devices/system/cpu/cpu0/power/energy_perf_bias"
#define PSTATE_MODE  SYSFS "/devices/system/cpu/intel_pstate/status"
#define NO_TURBO     SYSFS "/devices/system/cpu/intel_pstate/no_turbo"
#define MAX_PERF     SYSFS "/devices/system/cpu/intel_pstate/max_perf_pct"
#define ASPM         SYSFS "/module/pcie_aspm/parameters/policy"
#define BACKLIGHT    SYSFS "/class/backlight"
#define POWER_SUPPLY SYSFS "/class/power_supply"
#define NET_CLASS    SYSFS "/class/net"
#define RAPL_DOMAIN  SYSFS "/class/powercap/intel-rapl:0"
#define RAPL_PL1     RAPL_DOMAIN "/constraint_0_power_limit_uw"
#define RAPL_PL2     RAPL_DOMAIN "/constraint_1_power_limit_uw"

#define MAXPROFILES 16
#define MAXWIFI     8

/* EC attributes a profile may set, named as in the table in ec.c. */
#define NEC 7
static const char *const ec_keys[NEC] = {
	"shift_mode", "fan_mode", "cooler_boost", "super_battery",
	"charge_start", "charge_end", "kbd_backlight",
};

typedef enum {
	STATUS = 0,
	LIST,
	AUTO,
	PROFILE,
} Action;

enum {
	OPT_STATUS = 1000,
	OPT_LIST,
	OPT_AUTO,
	OPT_PROFILE,
	OPT_DRYRUN,
	OPT_CONFIG,
	OPT_VERBOSE,
	OPT_VERSION,
	OPT_HELP,
};

typedef struct {
	Action a;
	const char *profile; /* --profile */
	const char *config;  /* --config */
} Options;

typedef struct {
	char name[32];
	char epp[32];
	char governor[32];
	char epb[24];
	char aspm[32];
	char turbo[8];
	char max_perf[8];
	char pl1[16];            /* watts, empty = firmware default */
	char pl2[16];
	char nvidia_powerd[8];
	char wifi_powersave[8];
	char brightness[8];      /* % of max */
	char refresh[16];        /* read by powerprofile-x, ignored here */
	char only[8];            /* "ac", "bat" or empty */
	char run[256];           /* extra shell command, run as root */
	char ec[NEC][32];        /* values for ec_keys, empty = leave alone */
} Profile;

typedef enum {
	F_STR = 0,
	F_ONOFF,
	F_INT,
	F_NUM,
	F_ENUM,
} Kind;

typedef struct {
	const char *key;
	size_t off, len;              /* where the value lives in a Profile */
	Kind kind;
	long min, max;                /* F_INT bounds, inclusive */
	const char *const *values;    /* F_ENUM words, F_NUM keywords */
} Field;

static const char *const epp_values[]       = { "performance", "balance_performance",
                                                "balance_power", "power", NULL };
static const char *const epb_values[]       = { "performance", "balance-performance", "normal",
                                                "balance-power", "power", NULL };
static const char *const aspm_values[]      = { "default", "powersave", "powersupersave", NULL };
static const char *const only_values[]      = { "ac", "bat", NULL };
static const char *const refresh_keywords[] = { "max", NULL };

#define FIELD(k, m, kind, lo, hi, v) \
	{ k, offsetof(Profile, m), sizeof(((Profile *)0)->m), kind, lo, hi, v }

static const Field fields[] = {
	FIELD("epp",            epp,            F_ENUM,  0,   0, epp_values),
	FIELD("governor",       governor,       F_STR,   0,   0, NULL),
	FIELD("epb",            epb,            F_ENUM,  0,   0, epb_values),
	FIELD("aspm",           aspm,           F_ENUM,  0,   0, aspm_values),
	FIELD("turbo",          turbo,          F_ONOFF, 0,   0, NULL),
	FIELD("max_perf",       max_perf,       F_INT,   1, 100, NULL),
	FIELD("pl1",            pl1,            F_NUM,   0,   0, NULL),
	FIELD("pl2",            pl2,            F_NUM,   0,   0, NULL),
	FIELD("nvidia_powerd",  nvidia_powerd,  F_ONOFF, 0,   0, NULL),
	FIELD("wifi_powersave", wifi_powersave, F_ONOFF, 0,   0, NULL),
	FIELD("brightness",     brightness,     F_INT,   1, 100, NULL),
	FIELD("refresh",        refresh,        F_NUM,   0,   0, refresh_keywords),
	FIELD("only",           only,           F_ENUM,  0,   0, only_values),
	FIELD("run",            run,            F_STR,   0,   0, NULL),
};

static Profile profiles[MAXPROFILES];
static Profile defaults; /* the [default] section, inherited by every profile */
static Profile *cur;     /* the section being read */
static int nprof, have_defaults;
static char backlight[64];
static int dry;

static int         in_list(const char *const *list, const char *word);
static void        join_list(const char *const *list, char *buf, size_t bufsz);
static int         field_check(const Field *f, const char *val, char *err, size_t errsz);
static void        setstr(char *dst, size_t len, const char *key, const char *val);
static const Profile *find(const char *name);
static int         conf_cb(void *user, const char *sec, const char *key, const char *val);
static void        inherit(Profile *p);
static void        load(const char *path);
static int         wr(const char *path, const char *val);
static void        run_shell(const char *cmd);
static int         run_argv(char *const argv[], char *out, size_t outsz);
static void        show_argv(char *const argv[]);
static int         ac_online(void);
static void        save(const char *path, const char *val);
static int         fits(const Profile *p, int ac);
static const char *pick(int ac, char *ov, size_t ovsz);
static void        rapl_defaults(char *pl1, char *pl2);
static const char *limit(const char *watts, const char *def, char *buf, size_t bufsz);
static void        set_brightness(const char *pct);
static int         word_in(const char *list, const char *word);
static size_t      wr_glob(const char *pattern, const char *val);
static void        apply_cpu(const Profile *p);
static void        apply_rapl(const Profile *p);
static void        apply_ec(const Profile *p);
static void        nm_uuid(const char *list, const char *ifname, char *uuid, size_t uuidsz);
static void        apply_wifi(int on);
static void        apply(const Profile *p, int first);
static int         action_status(void);
static int         action_list(void);
static int         action_apply(const Options *o);
static void        usage(void);
static void        options_parse(Options *o, int argc, char *argv[]);

static int
in_list(const char *const *list, const char *word)
{
	if (!list)
		return 0;

	for (; *list; list++)
		if (strcmp(*list, word) == 0)
			return 1;

	return 0;
}

static void
join_list(const char *const *list, char *buf, size_t bufsz)
{
	size_t off = 0;

	buf[0] = '\0';
	for (; *list; list++) {
		int n = snprintf(buf + off, bufsz - off, "%s%s", off ? ", " : "", *list);

		if (n < 0 || (size_t)n >= bufsz - off)
			break;
		off += (size_t)n;
	}
}

/* Checks a config value against what its key accepts. Returns 0 if fine,
 * -1 with an explanation in err.
 */
static int
field_check(const Field *f, const char *val, char *err, size_t errsz)
{
	char *end;
	long l;
	double d;

	switch (f->kind) {
	case F_ONOFF:
		if (strcmp(val, "on") != 0 && strcmp(val, "off") != 0) {
			snprintf(err, errsz, "takes on or off, not \"%s\"", val);
			return -1;
		}
		break;

	case F_INT:
		errno = 0;
		l = strtol(val, &end, 10);
		if (errno != 0 || end == val || *end != '\0' || l < f->min || l > f->max) {
			snprintf(err, errsz, "takes an integer in [%ld, %ld], not \"%s\"",
			         f->min, f->max, val);
			return -1;
		}
		break;

	case F_NUM:
		if (in_list(f->values, val))
			break;
		d = strtod(val, &end);
		if (end == val || *end != '\0' || d <= 0) {
			snprintf(err, errsz, "takes a positive number%s%s, not \"%s\"",
			         f->values ? " or " : "", f->values ? f->values[0] : "", val);
			return -1;
		}
		break;

	case F_ENUM:
		if (!in_list(f->values, val)) {
			char words[128];

			join_list(f->values, words, sizeof(words));
			snprintf(err, errsz, "takes one of %s, not \"%s\"", words, val);
			return -1;
		}
		break;

	case F_STR:
		break;
	}

	return 0;
}

static void
setstr(char *dst, size_t len, const char *key, const char *val)
{
	int n = snprintf(dst, len, "%s", val);

	if (n < 0 || (size_t)n >= len)
		warn("value of \"%s\" is too long, truncated", key);
}

static const Profile *
find(const char *name)
{
	for (int i = 0; i < nprof; i++)
		if (strcmp(profiles[i].name, name) == 0)
			return &profiles[i];

	return NULL;
}

static int
conf_cb(void *user, const char *sec, const char *key, const char *val)
{
	char err[192];

	(void)user;

	if (!key) { /* section header */
		if (strcmp(sec, "default") == 0) {
			if (have_defaults) {
				warn("cannot use section [default] twice");
				return INI_ERROR;
			}
			have_defaults = 1;
			cur = &defaults;
			return INI_OK;
		}

		if (!sec[0] || nprof == MAXPROFILES || find(sec)) {
			warn("cannot use section [%s]", sec);
			return INI_ERROR;
		}
		setstr(profiles[nprof].name, sizeof(profiles[nprof].name), "section", sec);
		cur = &profiles[nprof++];
		return INI_OK;
	}

	if (!sec[0]) { /* before the first section: global settings */
		if (strcmp(key, "backlight") != 0)
			return INI_UNKNOWN;
		setstr(backlight, sizeof(backlight), key, val);
		return INI_OK;
	}

	for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
		if (strcmp(key, fields[i].key) != 0)
			continue;
		if (cur == &defaults && strcmp(key, "only") == 0) {
			warn("[default] cannot set only: it would restrict every profile");
			return INI_ERROR;
		}
		if (field_check(&fields[i], val, err, sizeof(err)) < 0) {
			warn("[%s] %s %s", sec, key, err);
			return INI_ERROR;
		}
		setstr((char *)cur + fields[i].off, fields[i].len, key, val);
		return INI_OK;
	}

	/* EC values depend on the firmware; they are checked when applied. */
	for (size_t i = 0; i < NEC; i++) {
		if (ec_keys[i] && strcmp(key, ec_keys[i]) == 0) {
			setstr(cur->ec[i], sizeof(cur->ec[i]), key, val);
			return INI_OK;
		}
	}

	return INI_UNKNOWN;
}

/* Fills every setting the profile leaves out from [default], so that
 * switching profiles always sets the same things: a toggle one profile
 * turns on is turned off again by the next, unless nothing names it at all.
 * `only` is never inherited.
 */
static void
inherit(Profile *p)
{
	for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
		char *dst = (char *)p + fields[i].off;
		const char *src = (const char *)&defaults + fields[i].off;

		if (strcmp(fields[i].key, "only") == 0 || dst[0] || !src[0])
			continue;
		snprintf(dst, fields[i].len, "%s", src);
	}

	for (size_t i = 0; i < NEC; i++)
		if (!p->ec[i][0])
			snprintf(p->ec[i], sizeof(p->ec[i]), "%s", defaults.ec[i]);
}

static void
load(const char *path)
{
	FILE *f;
	struct stat st;

	if (!(f = fopen(path, "r")))
		die("%s:", path);

	/* As root, refuse a config anybody else could have edited: `run =`
	 * executes as root.
	 */
	if (geteuid() == 0 && (fstat(fileno(f), &st) < 0 || st.st_uid != 0
	    || (st.st_mode & (S_IWGRP | S_IWOTH))))
		die("%s must be owned by root and not writable by others", path);

	if (ini_parse(f, path, conf_cb, NULL, 0) < 0)
		die("%s: malformed config", path);
	fclose(f);

	if (!find("bat") || !find("ac"))
		die("%s: needs [bat] and [ac] sections", path);

	for (int i = 0; i < nprof; i++)
		inherit(&profiles[i]);
}

static int
wr(const char *path, const char *val)
{
	FILE *f;
	int err;

	if (!val || !*val)
		return 0;

	if (dry) {
		printf("would write \"%s\" to %s\n", val, path);
		return 0;
	}

	if (!(f = fopen(path, "w"))) {
		err = errno;
		warn("%s:", path);
		errno = err;
		return -1;
	}

	fputs(val, f);
	if (fclose(f) == EOF) { /* sysfs reports write errors on flush */
		err = errno;
		warn("%s:", path);
		errno = err;
		return -1;
	}

	vinfo("wrote \"%s\" to %s", val, path);
	return 0;
}

static void
run_shell(const char *cmd)
{
	if (!*cmd)
		return;

	if (dry) {
		printf("would run: %s\n", cmd);
		return;
	}

	vinfo("running: %s", cmd);
	if (system(cmd) != 0)
		warn("command failed: %s", cmd);
}

/* Runs argv without a shell. With out set, its stdout is collected there.
 * Returns the exit status, or -1 if it could not be run at all.
 */
static int
run_argv(char *const argv[], char *out, size_t outsz)
{
	int fd[2], st;
	size_t len = 0;
	ssize_t r;
	pid_t pid;

	if (out && pipe(fd) < 0)
		return -1;

	if ((pid = fork()) < 0) {
		if (out) {
			close(fd[0]);
			close(fd[1]);
		}
		return -1;
	}

	if (pid == 0) {
		if (out) {
			dup2(fd[1], STDOUT_FILENO);
			close(fd[0]);
			close(fd[1]);
		}
		execvp(argv[0], argv);
		_exit(127);
	}

	if (out) {
		close(fd[1]);
		while (len < outsz - 1 && (r = read(fd[0], out + len, outsz - 1 - len)) > 0)
			len += (size_t)r;
		out[len] = '\0';
		close(fd[0]);
	}

	if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st))
		return -1;

	return WEXITSTATUS(st);
}

static void
show_argv(char *const argv[])
{
	printf("would run:");
	for (int i = 0; argv[i]; i++)
		printf(" %s", argv[i]);
	putchar('\n');
}

static int
ac_online(void)
{
	DIR *d;
	const struct dirent *de;
	char path[512], buf[32];
	int online = 0;

	if (!(d = opendir(POWER_SUPPLY)))
		return 0;

	while (!online && (de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;

		snprintf(path, sizeof(path), POWER_SUPPLY "/%s/type", de->d_name);
		if (readfile(path, buf, sizeof(buf)) < 0 || strcmp(buf, "Mains") != 0)
			continue;

		snprintf(path, sizeof(path), POWER_SUPPLY "/%s/online", de->d_name);
		online = readfile(path, buf, sizeof(buf)) == 0 && strcmp(buf, "1") == 0;
	}
	closedir(d);

	return online;
}

/* Writes val to a state file, or removes the file if val is NULL. */
static void
save(const char *path, const char *val)
{
	FILE *f;

	if (dry)
		return;

	if (!val) {
		unlink(path);
		return;
	}

	if ((f = fopen(path, "w"))) {
		fputs(val, f);
		fclose(f);
		chmod(path, 0644); /* powerprofile-x reads it as a normal user */
	}
}

/* Does the profile's `only =` match the current power source? */
static int
fits(const Profile *p, int ac)
{
	if (strcmp(p->only, "ac") == 0)
		return ac;
	if (strcmp(p->only, "bat") == 0)
		return !ac;

	return 1;
}

/* The profile --auto chooses: the manual override, left in ov, if it still
 * fits the power source, otherwise ac or bat.
 */
static const char *
pick(int ac, char *ov, size_t ovsz)
{
	const Profile *p;

	if (readfile(OVERRIDE_PATH, ov, ovsz) == 0 && (p = find(ov)) && fits(p, ac))
		return ov;

	return ac ? "ac" : "bat";
}

/* The firmware RAPL limits, captured the first time this runs after boot so
 * that an empty pl1/pl2 can restore them. pl1 and pl2 need 32 bytes each.
 */
static void
rapl_defaults(char *pl1, char *pl2)
{
	FILE *f;

	pl1[0] = pl2[0] = '\0';

	if ((f = fopen(RAPL_SAVE_PATH, "r"))) {
		if (fscanf(f, "%31s %31s", pl1, pl2) != 2)
			pl1[0] = pl2[0] = '\0';
		fclose(f);
		return;
	}

	readfile(RAPL_PL1, pl1, 32);
	readfile(RAPL_PL2, pl2, 32);

	if (!dry && pl1[0] && pl2[0] && (f = fopen(RAPL_SAVE_PATH, "w"))) {
		fprintf(f, "%s %s\n", pl1, pl2);
		fclose(f);
	}
}

/* Watts as microwatts in buf, or def if watts is empty. */
static const char *
limit(const char *watts, const char *def, char *buf, size_t bufsz)
{
	if (!*watts)
		return def;

	snprintf(buf, bufsz, "%.0f", strtod(watts, NULL) * 1e6);
	return buf;
}

static void
set_brightness(const char *pct)
{
	char dev[256], path[320], buf[32], val[32];
	const struct dirent *de;
	DIR *d;
	long max = 0, want = atol(pct), v;

	snprintf(dev, sizeof(dev), "%s", backlight);

	/* No `backlight =` in the config: take the first device. */
	if (!dev[0] && (d = opendir(BACKLIGHT))) {
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.')
				continue;
			snprintf(dev, sizeof(dev), "%s", de->d_name);
			break;
		}
		closedir(d);
	}

	if (!dev[0]) {
		warn("no backlight device found");
		return;
	}

	snprintf(path, sizeof(path), BACKLIGHT "/%s/max_brightness", dev);
	if (readfile(path, buf, sizeof(buf)) < 0 || (max = atol(buf)) <= 0) {
		warn("cannot read %s", path);
		return;
	}

	if ((v = max * want / 100) < 1)
		v = 1;

	snprintf(val, sizeof(val), "%ld", v);
	snprintf(path, sizeof(path), BACKLIGHT "/%s/brightness", dev);
	wr(path, val);
}

/* Is `word` one of the whitespace-separated words of `list`? */
static int
word_in(const char *list, const char *word)
{
	size_t len = strlen(word);

	while (*list) {
		size_t n;

		list += strspn(list, " \t");
		n = strcspn(list, " \t");
		if (n == len && strncmp(list, word, n) == 0)
			return 1;
		list += n;
	}

	return 0;
}

/* Writes val to every file matching pattern. Returns how many matched. */
static size_t
wr_glob(const char *pattern, const char *val)
{
	glob_t g;
	size_t n;

	if (glob(pattern, 0, NULL, &g) != 0)
		return 0;

	for (size_t i = 0; i < g.gl_pathc; i++)
		wr(g.gl_pathv[i], val);

	n = g.gl_pathc;
	globfree(&g);

	return n;
}

/* CPU frequency policy. Which knob exists depends on the cpufreq driver:
 * epp only with HWP (Intel Speed Shift), which also makes intel_pstate
 * default to active mode; without it the driver is intel_cpufreq (passive),
 * where the governor and the older energy performance bias (epb) are what
 * there is. --status shows which one this machine has.
 */
static void
apply_cpu(const Profile *p)
{
	char drv[64], avail[256];

	if (readfile(CPU0_DRIVER, drv, sizeof(drv)) < 0)
		snprintf(drv, sizeof(drv), "unknown");

	if (p->epp[0] && wr_glob(CPU_EPP, p->epp) == 0)
		warn("epp needs HWP (Intel Speed Shift), which is not active here "
		     "(cpufreq driver: %s); use governor and epb instead", drv);

	if (p->governor[0]) {
		/* only performance and powersave exist in intel_pstate's active mode */
		if (readfile(CPU0_GOVS, avail, sizeof(avail)) == 0 && !word_in(avail, p->governor))
			warn("governor \"%s\" is not available with the %s cpufreq driver; "
			     "supported: %s", p->governor, drv, avail);
		else if (wr_glob(CPU_GOV, p->governor) == 0)
			warn("governor: no cpufreq policy to set");
	}

	if (p->epb[0] && wr_glob(CPU_EPB, p->epb) == 0)
		warn("epb is not available on this CPU");

	if (p->turbo[0])
		wr(NO_TURBO, strcmp(p->turbo, "off") == 0 ? "1" : "0");

	wr(MAX_PERF, p->max_perf);

	if (p->aspm[0] && wr(ASPM, p->aspm) < 0 && errno == EPERM)
		warn("aspm: the kernel has PCIe ASPM disabled (by the firmware, or "
		     "pcie_aspm=off); see dmesg | grep -i aspm");
}

static void
apply_rapl(const Profile *p)
{
	char d1[32], d2[32], w1[32], w2[32];

	rapl_defaults(d1, d2);
	wr(RAPL_PL1, limit(p->pl1, d1, w1, sizeof(w1)));
	wr(RAPL_PL2, limit(p->pl2, d2, w2, sizeof(w2)));
}

/* Writes the profile's EC attributes through the table in ec.c, so every
 * value is checked against what this firmware accepts first.
 */
static void
apply_ec(const Profile *p)
{
	static int state; /* 0 = not tried, 1 = ready, -1 = no msi-ec */
	char err[EC_VALUE_MAX];

	for (size_t i = 0; i < NEC; i++) {
		const EcAttr *a;

		if (!p->ec[i][0])
			continue;

		if (state == 0)
			state = ec_init() == 0 ? 1 : -1;
		if (state < 0)
			return;

		if (!(a = ec_find(ec_keys[i])) || !ec_available(a)) {
			warn("%s is not supported by this firmware", ec_keys[i]);
			continue;
		}

		if (ec_validate(a, p->ec[i], err, sizeof(err)) < 0) {
			warn("%s", err);
			continue;
		}

		if (dry) {
			printf("would set %s to \"%s\"\n", ec_keys[i], p->ec[i]);
			continue;
		}

		if (ec_write(a, p->ec[i], err, sizeof(err)) < 0)
			warn("%s", err);
	}
}

/* Copies into uuid the NetworkManager connection uuid of `ifname` from the
 * "DEVICE:CON-UUID" lines of `list`; empty if it has none.
 */
static void
nm_uuid(const char *list, const char *ifname, char *uuid, size_t uuidsz)
{
	size_t len = strlen(ifname);

	uuid[0] = '\0';

	for (const char *line = list; line && *line; ) {
		const char *end = strchr(line, '\n');
		size_t n = end ? (size_t)(end - line) : strlen(line);

		if (n > len + 1 && strncmp(line, ifname, len) == 0 && line[len] == ':') {
			/* a uuid, not "--" or empty */
			if (n - len - 1 == 36 && n - len - 1 < uuidsz) {
				memcpy(uuid, line + len + 1, 36);
				uuid[36] = '\0';
			}
			return;
		}
		line = end ? end + 1 : NULL;
	}
}

/* Wi-Fi power saving on every wireless interface, found in sysfs so that no
 * device is ever named. `iw` changes the running device at once, which
 * NetworkManager cannot: it refuses to reapply 802-11-wireless.powersave to
 * an active connection. So the same setting is also put on the connection,
 * in memory only, for NetworkManager to reuse the next time it connects.
 */
static void
apply_wifi(int on)
{
	char *const query[] = { "nmcli", "-t", "-f", "DEVICE,CON-UUID", "device", NULL };
	char names[MAXWIFI][32], out[2048], uuid[40];
	const struct dirent *de;
	size_t n = 0;
	DIR *d;

	if (!(d = opendir(NET_CLASS)))
		return;

	while (n < MAXWIFI && (de = readdir(d))) {
		char path[320];
		size_t len = strlen(de->d_name);

		if (de->d_name[0] == '.' || len >= sizeof(names[0]))
			continue;

		snprintf(path, sizeof(path), NET_CLASS "/%s/wireless", de->d_name);
		if (access(path, F_OK) == 0)
			memcpy(names[n++], de->d_name, len + 1);
	}
	closedir(d);

	if (n == 0) {
		vinfo("wifi_powersave: no wireless interface");
		return;
	}

	/* NetworkManager is optional: without it only the running device changes. */
	if (run_argv(query, out, sizeof(out)) != 0)
		out[0] = '\0';

	for (size_t i = 0; i < n; i++) {
		char *const iw[] = { "iw", "dev", names[i], "set", "power_save",
		                     on ? "on" : "off", NULL };

		nm_uuid(out, names[i], uuid, sizeof(uuid));

		if (dry) {
			show_argv(iw);
		} else {
			vinfo("wifi power saving %s on %s", on ? "on" : "off", names[i]);
			if (run_argv(iw, NULL, 0) != 0)
				warn("wifi_powersave: iw failed on %s (is iw installed?)", names[i]);
		}

		if (uuid[0]) {
			char *const modify[] = { "nmcli", "connection", "modify", "--temporary",
			                         "uuid", uuid, "802-11-wireless.powersave",
			                         on ? "3" : "2", NULL };

			if (dry)
				show_argv(modify);
			else if (run_argv(modify, NULL, 0) != 0)
				vinfo("wifi_powersave: NetworkManager did not take it for %s", names[i]);
		}
	}
}

/* first: the profile just changed, so brightness may be applied. */
static void
apply(const Profile *p, int first)
{
	apply_cpu(p);
	apply_rapl(p);
	apply_ec(p);

	if (p->nvidia_powerd[0])
		run_shell(strcmp(p->nvidia_powerd, "on") == 0
		          ? "systemctl start --no-block nvidia-powerd"
		          : "systemctl stop --no-block nvidia-powerd");

	if (p->wifi_powersave[0])
		apply_wifi(strcmp(p->wifi_powersave, "on") == 0);

	run_shell(p->run);

	if (first && p->brightness[0])
		set_brightness(p->brightness);

	save(LAST_PATH, p->name);
	vinfo("applied profile %s", p->name);
}

static int
action_status(void)
{
	char ov[32], last[32], drv[64], mode[32], gov[32], epp[64], epb[32];
	char nt[16], mp[16], p1[32], p2[32];

	if (readfile(CPU0_DRIVER, drv, sizeof(drv)) < 0)
		snprintf(drv, sizeof(drv), "unknown");
	if (readfile(PSTATE_MODE, mode, sizeof(mode)) < 0)
		snprintf(mode, sizeof(mode), "n/a");
	if (readfile(CPU0_GOV, gov, sizeof(gov)) < 0)
		snprintf(gov, sizeof(gov), "n/a");
	if (readfile(CPU0_EPP, epp, sizeof(epp)) < 0)
		snprintf(epp, sizeof(epp), "n/a");
	if (readfile(CPU0_EPB, epb, sizeof(epb)) < 0)
		snprintf(epb, sizeof(epb), "n/a");
	readfile(NO_TURBO, nt, sizeof(nt));
	readfile(MAX_PERF, mp, sizeof(mp));
	readfile(RAPL_PL1, p1, sizeof(p1));
	readfile(RAPL_PL2, p2, sizeof(p2));

	if (readfile(OVERRIDE_PATH, ov, sizeof(ov)) < 0)
		snprintf(ov, sizeof(ov), "none");
	if (readfile(LAST_PATH, last, sizeof(last)) < 0)
		snprintf(last, sizeof(last), "none");

	printf("ac: %s  profile: %s  override: %s\n"
	       "cpufreq: %s  intel_pstate: %s  governor: %s\n"
	       "epp: %s  epb: %s  no_turbo: %s  max_perf: %s%%  PL1: %s  PL2: %s\n",
	       ac_online() ? "yes" : "no", last, ov, drv, mode, gov, epp, epb, nt, mp,
	       p1, p2);

	return 0;
}

static int
action_list(void)
{
	for (int i = 0; i < nprof; i++)
		puts(profiles[i].name);

	return 0;
}

/* --auto and --profile. A profile other than bat and ac is a manual
 * override: it lasts until it stops fitting its `only =`, or until another
 * one is applied.
 */
static int
action_apply(const Options *o)
{
	const Profile *p;
	char ov[32], last[32];
	int ac;

	if (o->a == PROFILE && !(p = find(o->profile)))
		die("no such profile \"%s\"", o->profile);

	if (REQUIRE_ROOT && !dry && geteuid() != 0)
		die("must run as root (use sudo)");

	ac = ac_online();

	if (o->a == AUTO) {
		const char *name = pick(ac, ov, sizeof(ov));

		if (name != ov)
			save(OVERRIDE_PATH, NULL); /* stale override */
		p = find(name);
	} else {
		p = find(o->profile);

		if (!fits(p, ac))
			die("%s is for %s power", p->name,
			    strcmp(p->only, "ac") == 0 ? "AC" : "battery");

		save(OVERRIDE_PATH,
		     strcmp(p->name, "bat") != 0 && strcmp(p->name, "ac") != 0 ? p->name : NULL);
	}

	apply(p, readfile(LAST_PATH, last, sizeof(last)) < 0 || strcmp(last, p->name) != 0);

	return 0;
}

static void
usage(void)
{
	printf("usage: %s [options]\n%s", get_name(),
	      "\tOptions:\n"
	      "\t\t[--help][--version]\n"
	      "\t\t[--status][--list][--auto][--profile name]\n"
	      "\t\t[--dry-run][--config file][--verbose]\n"
	      "\n"
	      "--help              Print this message and exit\n"
	      "--version           Print version and exit\n"
	      "--status            Print the AC state and live values (default)\n"
	      "--list              Print the profile names\n"
	      "--auto              Apply bat or ac from the AC adapter, keeping a manual\n"
	      "                    override while it still fits\n"
	      "--profile NAME      Apply one profile now\n"
	      "--dry-run           With --auto or --profile, print what would be done\n"
	      "--config FILE       Read FILE instead of " CONF_PATH "\n"
	      "--verbose           Log what is being done to stderr\n"
	      "\n"
	      "--auto and --profile need root unless --dry-run is given.\n");
}

static void
options_parse(Options *o, int argc, char *argv[])
{
	int nactions = 0;

	o->a = STATUS;
	o->profile = NULL;
	o->config = NULL;

	struct option longopts[] = {
		{ "status",  no_argument,       0, OPT_STATUS  },
		{ "list",    no_argument,       0, OPT_LIST    },
		{ "auto",    no_argument,       0, OPT_AUTO    },
		{ "profile", required_argument, 0, OPT_PROFILE },
		{ "dry-run", no_argument,       0, OPT_DRYRUN  },
		{ "config",  required_argument, 0, OPT_CONFIG  },
		{ "verbose", no_argument,       0, OPT_VERBOSE },
		{ "version", no_argument,       0, OPT_VERSION },
		{ "help",    no_argument,       0, OPT_HELP    },
		{ 0,         0,                 0, 0           },
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
		switch (opt) {
		case OPT_STATUS:
			nactions++;
			o->a = STATUS;
			break;

		case OPT_LIST:
			nactions++;
			o->a = LIST;
			break;

		case OPT_AUTO:
			nactions++;
			o->a = AUTO;
			break;

		case OPT_PROFILE:
			nactions++;
			o->a = PROFILE;
			o->profile = optarg;
			break;

		case OPT_DRYRUN:
			dry = 1;
			break;

		case OPT_CONFIG:
			o->config = optarg;
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

	if (nactions > 1)
		argerr("only one of --status, --list, --auto or --profile at a time");
}

int
main(int argc, char *argv[])
{
	Options o;

	set_name("powerprofile");

	options_parse(&o, argc, argv);

	if (o.a == STATUS)
		return action_status();

	load(o.config ? o.config : CONF_PATH);

	if (o.a == LIST)
		return action_list();

	return action_apply(&o);
}
