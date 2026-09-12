/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Rafal Rabczuk
 *
 * Unit tests for the RAPL encodings.
 *
 * These are the parts that fail quietly: a wrong time-window exponent does
 * not crash, it just averages the power limit over the wrong interval, and
 * the only symptom is a machine that runs hotter than expected.  Checking
 * them against values decoded by hand is the cheapest way to be sure.
 */

#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "../../src/intel_rapl.h"

static int failures;
static int checks;

static void
check_eq(const char *what, uint32_t got, uint32_t want)
{
	checks++;
	if (got != want) {
		printf("  FAIL  %-44s got %u, want %u\n", what, got, want);
		failures++;
	}
}

/*
 * Known values, decoded by hand from the Intel SDM formula
 *
 *	window = 2^Y * (1 + Z/4) * time_unit
 *
 * with the time unit shift of 10 (1/1024 s) that Broadwell reports.
 */
static void
test_window_decode_known(void)
{
	/* Y=14, Z=3 -> 2^14 * 1.75 = 28672 ticks / 1024 = 28 s.
	   This is the value the XPS 13 9343 ships with for PL1. */
	check_eq("decode Y=14 Z=3 -> 28 s",
	    rapl_window_decode(14 | (3u << 5), 10), 28);

	/* Y=10, Z=0 -> 1024 ticks / 1024 = 1 s */
	check_eq("decode Y=10 Z=0 -> 1 s",
	    rapl_window_decode(10, 10), 1);

	/* Y=13, Z=0 -> 8192 / 1024 = 8 s */
	check_eq("decode Y=13 Z=0 -> 8 s",
	    rapl_window_decode(13, 10), 8);

	/* Y=15, Z=0 -> 32768 / 1024 = 32 s */
	check_eq("decode Y=15 Z=0 -> 32 s",
	    rapl_window_decode(15, 10), 32);

	/* sub-tick windows floor to zero rather than wrapping */
	check_eq("decode Y=0 Z=0 -> 0 s",
	    rapl_window_decode(0, 10), 0);
}

/*
 * Encoding need not be exact - the representable values are sparse - but it
 * must land on the nearest one, and decoding it must give back what was
 * asked for wherever the value is representable.
 */
static void
test_window_roundtrip(void)
{
	static const uint32_t exact[] = { 1, 2, 4, 8, 16, 28, 32, 64 };
	uint32_t i, field, back;
	char buf[64];

	for (i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
		field = rapl_window_encode(exact[i], 10);
		back = rapl_window_decode(field, 10);
		snprintf(buf, sizeof(buf), "roundtrip %u s", exact[i]);
		check_eq(buf, back, exact[i]);
	}
}

static void
test_window_clamps(void)
{
	uint32_t back;

	/* zero is meaningless as an averaging interval; treated as 1 s */
	back = rapl_window_decode(rapl_window_encode(0, 10), 10);
	check_eq("encode 0 s clamps up to 1 s", back, 1);

	/* absurd values clamp instead of overflowing the exponent */
	back = rapl_window_decode(rapl_window_encode(100000, 10), 10);
	checks++;
	if (back > RAPL_MAX_WINDOW_SEC) {
		printf("  FAIL  %-44s got %u, want <= %u\n",
		    "encode 100000 s clamps", back, RAPL_MAX_WINDOW_SEC);
		failures++;
	}
}

/*
 * The bounds check is the last thing standing between a typo and a machine
 * throttled into uselessness, so it gets tested on its own rather than only
 * through the MSR path - which on locked hardware is never reached at all.
 */
static void
test_bounds(void)
{
	check_eq("10 W within 5-30 accepted",
	    rapl_check_bounds(10, 5, 30) == 0, 1);
	check_eq("5 W exactly at the floor accepted",
	    rapl_check_bounds(5, 5, 30) == 0, 1);
	check_eq("30 W exactly at the ceiling accepted",
	    rapl_check_bounds(30, 5, 30) == 0, 1);
	check_eq("4 W below the floor refused",
	    rapl_check_bounds(4, 5, 30) == EINVAL, 1);
	check_eq("31 W above the ceiling refused",
	    rapl_check_bounds(31, 5, 30) == EINVAL, 1);
	check_eq("0 W refused",
	    rapl_check_bounds(0, 5, 30) == EINVAL, 1);
	/* a zero ceiling means "no ceiling", used when hardware reports none */
	check_eq("ceiling 0 disables the upper check",
	    rapl_check_bounds(999, 5, 0) == 0, 1);
	check_eq("floor still applies with ceiling 0",
	    rapl_check_bounds(1, 5, 0) == EINVAL, 1);
}

int
main(void)
{
	printf("intel_rapl unit tests\n");

	test_window_decode_known();
	test_window_roundtrip();
	test_window_clamps();
	test_bounds();

	printf("%d checks, %d failures\n", checks, failures);
	return (failures != 0);
}
