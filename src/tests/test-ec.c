/* See LICENSE file for copyright and license details.
 *
 * Exercises the attribute table and its validation against a synthetic
 * sysfs tree, so the rules that guard every write can be checked without
 * MSI hardware. Built with EC_PLATFORM_DIR and EC_SUPPLY_DIR pointed at
 * TESTROOT; see the `test` target in the Makefile.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "ec.h"
#include "utils.h"

#ifndef TESTROOT
#define TESTROOT "build/tests/root"
#endif

static int failures;

static void ok(int cond, const char *what);
static void test_read(void);
static void test_enum(void);
static void test_bool(void);
static void test_int(void);
static void test_readonly(void);
static void test_lookup(void);
static void test_led(void);
static void test_static_enum(void);
static void test_range(void);

static void
ok(int cond, const char *what)
{
	printf("%s %s\n", cond ? "ok    " : "FAIL  ", what);
	if (!cond)
		failures++;
}

static void
test_read(void)
{
	const EcAttr *a = ec_find("shift_mode");
	char buf[EC_VALUE_MAX] = "";

	ok(a != NULL, "shift_mode is in the table");
	ok(a && ec_read(a, buf, sizeof(buf)) == 0, "shift_mode reads");
	ok(strcmp(buf, "comfort") == 0, "shift_mode reads without its newline");
}

static void
test_enum(void)
{
	const EcAttr *a = ec_find("shift_mode");
	char buf[EC_VALUE_MAX] = "", err[EC_VALUE_MAX] = "";

	ok(ec_write(a, "eco", err, sizeof(err)) == 0, "a supported shift mode is accepted");
	ec_read(a, buf, sizeof(buf));
	ok(strcmp(buf, "eco") == 0, "the written shift mode reads back");

	ok(ec_write(a, "ludicrous", err, sizeof(err)) < 0, "an unsupported shift mode is refused");
	ok(strstr(err, "eco") != NULL, "the refusal names the supported values");
}

static void
test_bool(void)
{
	const EcAttr *a = ec_find("cooler_boost");
	char err[EC_VALUE_MAX] = "";

	ok(ec_write(a, "on", err, sizeof(err)) == 0, "cooler_boost accepts on");
	ok(ec_write(a, "off", err, sizeof(err)) == 0, "cooler_boost accepts off");
	ok(ec_write(a, "yes", err, sizeof(err)) < 0, "cooler_boost refuses yes");
	ok(ec_write(a, "1", err, sizeof(err)) < 0, "cooler_boost refuses 1");
}

static void
test_int(void)
{
	const EcAttr *a = ec_find("charge_end");
	char buf[EC_VALUE_MAX] = "", err[EC_VALUE_MAX] = "";

	ok(ec_write(a, "80", err, sizeof(err)) == 0, "charge_end accepts 80");
	ec_read(a, buf, sizeof(buf));
	ok(strcmp(buf, "80") == 0, "charge_end reads back 80");

	ok(ec_write(a, "150", err, sizeof(err)) < 0, "charge_end refuses 150");
	ok(ec_write(a, "-1", err, sizeof(err)) < 0, "charge_end refuses -1");
	ok(ec_write(a, "abc", err, sizeof(err)) < 0, "charge_end refuses non-numeric");
	ok(ec_write(a, "80x", err, sizeof(err)) < 0, "charge_end refuses trailing garbage");
}

static void
test_readonly(void)
{
	const EcAttr *a = ec_find("cpu_temp");
	char err[EC_VALUE_MAX] = "";

	ok(ec_write(a, "50", err, sizeof(err)) < 0, "a read-only attribute refuses writes");
	ok(strstr(err, "read-only") != NULL, "the refusal says why");
}

static void
test_lookup(void)
{
	ok(ec_find("nonexistent") == NULL, "an unknown name resolves to nothing");
	ok(ec_available(ec_find("webcam_block")) == 0, "an absent attribute reports unavailable");
	ok(ec_available(ec_find("shift_mode")) == 1, "a present attribute reports available");
}

/* The LED bounds must come from each LED's own max_brightness, not a
 * hardcoded 0/1: the fixture gives mute one bit and the keyboard
 * backlight four levels.
 */
static void
test_led(void)
{
	const EcAttr *mute = ec_find("mute_led");
	const EcAttr *kbd = ec_find("kbd_backlight");
	char err[EC_VALUE_MAX] = "", buf[EC_VALUE_MAX] = "";

	ok(mute && mute->max == 1, "mute_led bound read from max_brightness");
	ok(kbd && kbd->max == 3, "kbd_backlight bound read from max_brightness");

	ok(ec_write(mute, "1", err, sizeof(err)) == 0, "mute_led accepts 1");
	ec_read(mute, buf, sizeof(buf));
	ok(strcmp(buf, "1") == 0, "mute_led reads back 1");
	ok(ec_write(mute, "2", err, sizeof(err)) < 0, "mute_led refuses 2");

	ok(ec_write(kbd, "3", err, sizeof(err)) == 0, "kbd_backlight accepts 3");
	ok(ec_write(kbd, "4", err, sizeof(err)) < 0, "kbd_backlight refuses 4");
}

/* fn_key publishes no available_* list, so its values come from the
 * static table instead.
 */
static void
test_static_enum(void)
{
	const EcAttr *a = ec_find("fn_key");
	char err[EC_VALUE_MAX] = "";

	ok(ec_write(a, "right", err, sizeof(err)) == 0, "fn_key accepts right");
	ok(ec_write(a, "sideways", err, sizeof(err)) < 0, "fn_key refuses sideways");
	ok(strstr(err, "left") != NULL, "the refusal names the static values");
}

static void
test_range(void)
{
	char buf[EC_VALUE_MAX];

	ec_range(ec_find("shift_mode"), buf, sizeof(buf));
	ok(strstr(buf, "turbo") != NULL, "range of a published enum lists its values");

	ec_range(ec_find("fn_key"), buf, sizeof(buf));
	ok(strcmp(buf, "left, right") == 0, "range of a static enum lists its values");

	ec_range(ec_find("kbd_backlight"), buf, sizeof(buf));
	ok(strcmp(buf, "0-3") == 0, "range of an LED shows its bounds");

	ec_range(ec_find("cooler_boost"), buf, sizeof(buf));
	ok(strcmp(buf, "on, off") == 0, "range of a bool shows on and off");

	ec_range(ec_find("cpu_temp"), buf, sizeof(buf));
	ok(strcmp(buf, "read-only") == 0, "range of a read-only entry says so");
}

int
main(void)
{
	set_name("test-ec");

	if (ec_init() < 0) {
		printf("FAIL   ec_init against %s\n", TESTROOT);
		return 1;
	}

	test_read();
	test_enum();
	test_bool();
	test_int();
	test_readonly();
	test_lookup();
	test_led();
	test_static_enum();
	test_range();

	printf("\n%s\n", failures ? "FAILURES" : "all tests passed");

	return failures != 0;
}
