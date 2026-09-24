/* See LICENSE file for copyright and license details. */

#ifndef POWERPROFILE_EC_H
#define POWERPROFILE_EC_H

#include <stddef.h>

/* Longest value we ever read from or write to a sysfs attribute. The
 * available_* enumerations are newline-separated lists and are the only
 * entries that need real room; everything else is a word or a number.
 */
#define EC_VALUE_MAX 256

typedef enum {
	EC_RO = 0,
	EC_RW,
} EcAccess;

typedef enum {
	EC_BASE_PLATFORM = 0, /* under /sys/devices/platform/msi-ec */
	EC_BASE_BATTERY,      /* under /sys/class/power_supply/BAT* */
} EcBase;

typedef enum {
	EC_ENUM = 0, /* one of a fixed or firmware-published set of words */
	EC_BOOL,     /* on / off */
	EC_INT,      /* integer within [min, max] */
	EC_LED,      /* integer within [0, max_brightness], read at init */
	EC_STR,      /* free-form; read-only in practice */
} EcType;

typedef struct {
	const char *name;    /* name exposed over D-Bus and on the CLI */
	const char *file;    /* path relative to the entry's base directory */
	EcBase base;
	EcAccess access;
	EcType type;

	/* EC_ENUM takes its valid values from whichever of these is set:
	 * `choices` names another attribute holding a newline-separated list
	 * published by the firmware, `values` is a static NULL-terminated
	 * list for the settings the driver does not enumerate itself.
	 */
	const char *choices;
	const char *const *values;

	long min, max;       /* EC_INT and EC_LED bounds, inclusive */
	const char *desc;    /* one-line description, shown by --list */
} EcAttr;

/*
 * Locates the msi-ec platform device and the battery power_supply device,
 * and reads the upper bound of every LED attribute from its
 * max_brightness file.
 *
 * Returns 0 on success, -1 if the msi-ec driver is not present (the
 * daemon cannot do anything useful in that case).
 */
int ec_init(void);

/* Returns the attribute table, terminated by an entry with a NULL name. */
const EcAttr *ec_attrs(void);

/* Returns the attribute named `name`, or NULL if there is no such entry. */
const EcAttr *ec_find(const char *name);

/*
 * True if the attribute's backing sysfs file exists on this machine.
 * The driver only exposes what a given firmware supports, so the table
 * is a superset of any one laptop's capabilities.
 */
int ec_available(const EcAttr *a);

/*
 * Describes what an attribute accepts, e.g. "eco, comfort, sport" or
 * "0-3", for --list and for error messages. Writes "read-only" for
 * read-only entries. Always NUL-terminates.
 */
void ec_range(const EcAttr *a, char *buf, size_t bufsz);

/*
 * Reads an attribute into `buf`, stripped of its trailing newline.
 *
 * Returns 0 on success, -1 on failure (errno is set).
 */
int ec_read(const EcAttr *a, char *buf, size_t bufsz);

/*
 * Checks `value` against the attribute's type without touching sysfs:
 * read-only entries are refused, booleans must be on or off, integers must
 * be in range and enumerations one of the accepted words.
 *
 * Returns 0 if `value` would be written, -1 otherwise. On failure errno is
 * set to EINVAL and `errbuf` (if non-NULL) receives an explanation.
 */
int ec_validate(const EcAttr *a, const char *value, char *errbuf, size_t errsz);

/*
 * Validates `value` with ec_validate() and writes it.
 *
 * Returns 0 on success, -1 on failure. On a validation failure errno is
 * set to EINVAL and `errbuf` (if non-NULL) receives an explanation
 * suitable for showing to the user.
 */
int ec_write(const EcAttr *a, const char *value, char *errbuf, size_t errsz);

#endif /* POWERPROFILE_EC_H */
