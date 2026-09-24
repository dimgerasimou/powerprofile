/* See LICENSE file for copyright and license details. */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ec.h"
#include "utils.h"

/* Overridable so the test build can point at a fake sysfs tree. */
#ifndef EC_PLATFORM_DIR
#define EC_PLATFORM_DIR "/sys/devices/platform/msi-ec"
#endif

#ifndef EC_SUPPLY_DIR
#define EC_SUPPLY_DIR   "/sys/class/power_supply"
#endif

/* Deliberately far smaller than PATH_MAX: this only ever holds
 * EC_SUPPLY_DIR plus a device name like "BAT0", and keeping it bounded
 * lets the compiler prove attr_path() cannot truncate.
 */
static char battery_dir[128];

/* Settings the driver accepts but does not publish a list for. */
static const char *const key_positions[] = { "left", "right", NULL };

/* The set of attributes the daemon is willing to touch. Anything not in
 * this table cannot be read or written through the service, no matter
 * what a client asks for - the table *is* the security boundary.
 *
 * EC_LED upper bounds are filled in by ec_init() from each LED's own
 * max_brightness, so they are correct per model rather than assumed.
 */
static EcAttr attrs[] = {
	/* name, file, base, access, type, choices, values, min, max, desc */

	{ "shift_mode", "shift_mode", EC_BASE_PLATFORM, EC_RW, EC_ENUM,
	  "available_shift_modes", NULL, 0, 0,
	  "CPU/GPU performance profile" },
	{ "fan_mode", "fan_mode", EC_BASE_PLATFORM, EC_RW, EC_ENUM,
	  "available_fan_modes", NULL, 0, 0,
	  "fan speed profile" },
	{ "cooler_boost", "cooler_boost", EC_BASE_PLATFORM, EC_RW, EC_BOOL,
	  NULL, NULL, 0, 0,
	  "maximum fan speed regardless of temperature" },
	{ "super_battery", "super_battery", EC_BASE_PLATFORM, EC_RW, EC_BOOL,
	  NULL, NULL, 0, 0,
	  "extended battery mode" },

	{ "charge_start", "charge_control_start_threshold", EC_BASE_BATTERY,
	  EC_RW, EC_INT, NULL, NULL, 0, 100,
	  "battery percentage below which charging begins" },
	{ "charge_end", "charge_control_end_threshold", EC_BASE_BATTERY,
	  EC_RW, EC_INT, NULL, NULL, 0, 100,
	  "battery percentage above which charging stops" },

	{ "kbd_backlight", "leds/msiacpi::kbd_backlight/brightness",
	  EC_BASE_PLATFORM, EC_RW, EC_LED, NULL, NULL, 0, 1,
	  "keyboard backlight level" },
	{ "mute_led", "leds/platform::mute/brightness",
	  EC_BASE_PLATFORM, EC_RW, EC_LED, NULL, NULL, 0, 1,
	  "speaker mute indicator" },
	{ "micmute_led", "leds/platform::micmute/brightness",
	  EC_BASE_PLATFORM, EC_RW, EC_LED, NULL, NULL, 0, 1,
	  "microphone mute indicator" },

	{ "webcam", "webcam", EC_BASE_PLATFORM, EC_RW, EC_BOOL, NULL, NULL, 0, 0,
	  "webcam power" },
	{ "webcam_block", "webcam_block", EC_BASE_PLATFORM, EC_RW, EC_BOOL,
	  NULL, NULL, 0, 0,
	  "block the webcam from being enabled" },
	{ "fn_key", "fn_key", EC_BASE_PLATFORM, EC_RW, EC_ENUM,
	  NULL, key_positions, 0, 0,
	  "Fn key position" },
	{ "win_key", "win_key", EC_BASE_PLATFORM, EC_RW, EC_ENUM,
	  NULL, key_positions, 0, 0,
	  "Super key position" },

	{ "cpu_temp", "cpu/realtime_temperature", EC_BASE_PLATFORM, EC_RO,
	  EC_STR, NULL, NULL, 0, 0,
	  "CPU temperature (C)" },
	{ "gpu_temp", "gpu/realtime_temperature", EC_BASE_PLATFORM, EC_RO,
	  EC_STR, NULL, NULL, 0, 0,
	  "GPU temperature (C)" },
	{ "cpu_fan_rpm", "cpu/realtime_fan_speed", EC_BASE_PLATFORM, EC_RO,
	  EC_STR, NULL, NULL, 0, 0,
	  "CPU fan speed" },
	{ "gpu_fan_rpm", "gpu/realtime_fan_speed", EC_BASE_PLATFORM, EC_RO,
	  EC_STR, NULL, NULL, 0, 0,
	  "GPU fan speed" },

	{ "available_shift_modes", "available_shift_modes", EC_BASE_PLATFORM,
	  EC_RO, EC_STR, NULL, NULL, 0, 0,
	  "shift modes supported by this firmware" },
	{ "available_fan_modes", "available_fan_modes", EC_BASE_PLATFORM,
	  EC_RO, EC_STR, NULL, NULL, 0, 0,
	  "fan modes supported by this firmware" },
	{ "battery_capacity", "capacity", EC_BASE_BATTERY, EC_RO, EC_STR,
	  NULL, NULL, 0, 0,
	  "battery charge level (%)" },
	{ "battery_status", "status", EC_BASE_BATTERY, EC_RO, EC_STR,
	  NULL, NULL, 0, 0,
	  "whether the battery is charging" },
	{ "fw_version", "fw_version", EC_BASE_PLATFORM, EC_RO, EC_STR,
	  NULL, NULL, 0, 0,
	  "motherboard firmware version" },
	{ "fw_release_date", "fw_release_date", EC_BASE_PLATFORM, EC_RO, EC_STR,
	  NULL, NULL, 0, 0,
	  "motherboard firmware release date" },

	{ NULL, NULL, EC_BASE_PLATFORM, EC_RO, EC_STR, NULL, NULL, 0, 0, NULL },
};

static void attr_path(const EcAttr *a, char *buf, size_t bufsz);
static int  find_battery(void);
static void init_led_bounds(void);
static int  read_file(const char *path, char *buf, size_t bufsz);
static int  enum_contains(const EcAttr *a, const char *value);

/* Builds the absolute sysfs path for an attribute. */
static void
attr_path(const EcAttr *a, char *buf, size_t bufsz)
{
	if (a->base == EC_BASE_BATTERY)
		snprintf(buf, bufsz, "%s/%s", battery_dir, a->file);
	else
		snprintf(buf, bufsz, "%s/%s", EC_PLATFORM_DIR, a->file);
}

/* Locates the first power_supply device exposing a charge threshold. The
 * name varies (BAT0, BAT1, ...) so it cannot be hardcoded.
 */
static int
find_battery(void)
{
	DIR *d;
	const struct dirent *de;
	int found = 0;

	if (!(d = opendir(EC_SUPPLY_DIR)))
		return -1;

	while (!found && (de = readdir(d))) {
		char probe[PATH_MAX];

		if (de->d_name[0] == '.')
			continue;

		snprintf(probe, sizeof(probe), "%s/%s/charge_control_end_threshold",
		         EC_SUPPLY_DIR, de->d_name);

		if (access(probe, F_OK) == 0) {
			int n = snprintf(battery_dir, sizeof(battery_dir), "%s/%s",
			                 EC_SUPPLY_DIR, de->d_name);

			/* Absurdly long device name; leave battery_dir empty so the
			 * battery attributes report as unavailable rather than
			 * pointing at a truncated path.
			 */
			if (n < 0 || (size_t)n >= sizeof(battery_dir)) {
				battery_dir[0] = '\0';
				warn("ignoring power supply with an over-long name");
				continue;
			}
			found = 1;
		}
	}
	closedir(d);

	return found ? 0 : -1;
}

/* Replaces the placeholder bound on every EC_LED entry with the value the
 * kernel reports in that LED's max_brightness, so a three-level keyboard
 * backlight and a one-bit mute indicator are each bounded correctly.
 */
static void
init_led_bounds(void)
{
	for (EcAttr *a = attrs; a->name; a++) {
		char path[PATH_MAX], maxpath[PATH_MAX], buf[EC_VALUE_MAX];
		char *end, *slash;
		long v;

		if (a->type != EC_LED || !ec_available(a))
			continue;

		attr_path(a, path, sizeof(path));

		/* .../brightness -> .../max_brightness */
		snprintf(maxpath, sizeof(maxpath), "%s", path);
		if (!(slash = strrchr(maxpath, '/')))
			continue;
		snprintf(slash + 1, sizeof(maxpath) - (size_t)(slash + 1 - maxpath),
		         "max_brightness");

		if (read_file(maxpath, buf, sizeof(buf)) < 0)
			continue;

		errno = 0;
		v = strtol(buf, &end, 10);
		if (errno == 0 && end != buf && v > 0) {
			a->max = v;
			vinfo("%s: max_brightness %ld", a->name, v);
		}
	}
}

int
ec_init(void)
{
	if (access(EC_PLATFORM_DIR, F_OK) != 0) {
		warn("%s not found; is the msi-ec module loaded?", EC_PLATFORM_DIR);
		return -1;
	}

	/* A missing battery node is not fatal: everything except the battery
	 * attributes still works, and ec_available() reports them absent.
	 */
	if (find_battery() < 0)
		vinfo("no battery with charge thresholds found");
	else
		vinfo("battery: %s", battery_dir);

	init_led_bounds();

	return 0;
}

const EcAttr *
ec_attrs(void)
{
	return attrs;
}

const EcAttr *
ec_find(const char *name)
{
	for (const EcAttr *a = attrs; a->name; a++) {
		if (strcmp(a->name, name) == 0)
			return a;
	}
	return NULL;
}

int
ec_available(const EcAttr *a)
{
	char path[PATH_MAX];

	if (a->base == EC_BASE_BATTERY && battery_dir[0] == '\0')
		return 0;

	attr_path(a, path, sizeof(path));
	return access(path, F_OK) == 0;
}

void
ec_range(const EcAttr *a, char *buf, size_t bufsz)
{
	if (a->access != EC_RW) {
		snprintf(buf, bufsz, "read-only");
		return;
	}

	switch (a->type) {
	case EC_BOOL:
		snprintf(buf, bufsz, "on, off");
		break;

	case EC_INT:
	case EC_LED:
		snprintf(buf, bufsz, "%ld-%ld", a->min, a->max);
		break;

	case EC_ENUM:
		if (a->values) {
			size_t off = 0;

			buf[0] = '\0';
			for (const char *const *v = a->values; *v; v++) {
				int n = snprintf(buf + off, bufsz - off, "%s%s",
				                 off ? ", " : "", *v);
				if (n < 0 || (size_t)n >= bufsz - off)
					break;
				off += (size_t)n;
			}
		} else if (a->choices) {
			const EcAttr *c = ec_find(a->choices);

			if (c && ec_read(c, buf, bufsz) == 0) {
				/* the file is one value per line; make it a phrase */
				for (char *p = buf; *p; p++)
					if (*p == '\n')
						*p = ' ';
			} else {
				snprintf(buf, bufsz, "see %s", a->choices);
			}
		} else {
			snprintf(buf, bufsz, "any");
		}
		break;

	case EC_STR:
	default:
		snprintf(buf, bufsz, "any");
		break;
	}
}

/* Reads a whole sysfs file and strips the trailing newline. */
static int
read_file(const char *path, char *buf, size_t bufsz)
{
	FILE *f;
	size_t n;

	if (!(f = fopen(path, "r")))
		return -1;

	n = fread(buf, 1, bufsz - 1, f);
	fclose(f);

	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';

	return 0;
}

int
ec_read(const EcAttr *a, char *buf, size_t bufsz)
{
	char path[PATH_MAX];

	attr_path(a, path, sizeof(path));
	return read_file(path, buf, bufsz);
}

/*
 * Whether `value` is accepted by an EC_ENUM attribute: checked against its
 * static list if it has one, otherwise against the list the firmware
 * publishes.
 *
 * Returns 1 if valid, 0 if not, -1 if there is nothing to check against.
 */
static int
enum_contains(const EcAttr *a, const char *value)
{
	const EcAttr *choices;
	char list[EC_VALUE_MAX];
	char *saveptr = NULL;
	char *tok;

	if (a->values) {
		for (const char *const *v = a->values; *v; v++) {
			if (strcmp(*v, value) == 0)
				return 1;
		}
		return 0;
	}

	if (!a->choices)
		return -1; /* nothing published; let the driver decide */

	if (!(choices = ec_find(a->choices)))
		return -1;

	if (ec_read(choices, list, sizeof(list)) < 0)
		return -1;

	for (tok = strtok_r(list, "\n", &saveptr); tok;
	     tok = strtok_r(NULL, "\n", &saveptr)) {
		if (strcmp(tok, value) == 0)
			return 1;
	}

	return 0;
}

int
ec_validate(const EcAttr *a, const char *value, char *errbuf, size_t errsz)
{
	if (a->access != EC_RW) {
		if (errbuf)
			snprintf(errbuf, errsz, "\"%s\" is read-only", a->name);
		errno = EINVAL;
		return -1;
	}

	switch (a->type) {
	case EC_BOOL:
		if (strcmp(value, "on") != 0 && strcmp(value, "off") != 0) {
			if (errbuf)
				snprintf(errbuf, errsz,
				         "\"%s\" takes on or off, not \"%s\"", a->name, value);
			errno = EINVAL;
			return -1;
		}
		break;

	case EC_INT:
	case EC_LED: {
		char *end;
		long v;

		errno = 0;
		v = strtol(value, &end, 10);
		if (errno != 0 || end == value || *end != '\0'
		    || v < a->min || v > a->max) {
			if (errbuf)
				snprintf(errbuf, errsz,
				         "\"%s\" takes an integer in [%ld, %ld], not \"%s\"",
				         a->name, a->min, a->max, value);
			errno = EINVAL;
			return -1;
		}
		break;
	}

	case EC_ENUM: {
		int ok = enum_contains(a, value);

		/* ok < 0 means there is no list to check against; pass the value
		 * through and let the driver reject it.
		 */
		if (ok == 0) {
			if (errbuf) {
				char range[EC_VALUE_MAX];

				ec_range(a, range, sizeof(range));
				snprintf(errbuf, errsz,
				         "\"%s\" is not valid for %s; supported: %s",
				         value, a->name, range);
			}
			errno = EINVAL;
			return -1;
		}
		break;
	}

	case EC_STR:
		break;
	}

	return 0;
}

int
ec_write(const EcAttr *a, const char *value, char *errbuf, size_t errsz)
{
	char path[PATH_MAX];
	FILE *f;

	if (ec_validate(a, value, errbuf, errsz) < 0)
		return -1;

	attr_path(a, path, sizeof(path));

	if (!(f = fopen(path, "w"))) {
		if (errbuf)
			snprintf(errbuf, errsz, "cannot open %s: %s", path, strerror(errno));
		return -1;
	}

	if (fprintf(f, "%s\n", value) < 0 || fclose(f) != 0) {
		if (errbuf)
			snprintf(errbuf, errsz, "cannot write %s: %s", path, strerror(errno));
		return -1;
	}

	vinfo("wrote \"%s\" to %s", value, path);
	return 0;
}
