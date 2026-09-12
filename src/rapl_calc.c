/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Rafal Rabczuk
 *
 * Pure arithmetic shared by the driver and its unit tests.
 *
 * Nothing here touches an MSR, the softc or any kernel facility, which is
 * the point: the fiddly parts of RAPL are the encodings, and encodings can
 * be got wrong silently.  Keeping them in a file that compiles in userland
 * means they can be checked against known values instead of against a
 * machine that happens to be on the desk.
 */

#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#else
#include <stdint.h>
#include <errno.h>
#endif

#include "intel_rapl.h"

/*
 * Time window conversions.
 *
 * The hardware stores the averaging interval as a mantissa/exponent pair:
 *
 *	window = 2^Y * (1 + Z/4) * time_unit
 *
 * Both directions are pure arithmetic on the raw field, taking the time unit
 * shift as a parameter rather than reading the softc, so they can be unit
 * tested outside the kernel.
 */
uint32_t
rapl_window_decode(uint32_t field, uint32_t time_shift)
{
	uint32_t y, z, ticks;

	y = (field >> (PKG_LIMIT_TIME_Y_SHIFT - PKG_LIMIT_TIME_Y_SHIFT)) &
	    PKG_LIMIT_TIME_Y_MASK;
	z = (field >> (PKG_LIMIT_TIME_Z_SHIFT - PKG_LIMIT_TIME_Y_SHIFT)) &
	    PKG_LIMIT_TIME_Z_MASK;

	if (y >= 31)		/* would overflow the shift below */
		return (RAPL_MAX_WINDOW_SEC);

	/* ticks = 2^Y * (4 + Z) / 4, then scale by the time unit */
	ticks = ((1u << y) * (4 + z)) / 4;
	return (ticks >> time_shift);
}

uint32_t
rapl_window_encode(uint32_t seconds, uint32_t time_shift)
{
	uint32_t target, y, z, best_y, best_z, cand;
	uint32_t best_err = ~0u, err;

	if (seconds == 0)
		seconds = 1;
	if (seconds > RAPL_MAX_WINDOW_SEC)
		seconds = RAPL_MAX_WINDOW_SEC;

	target = seconds << time_shift;

	/*
	 * Search rather than solve.  The representable values are sparse and
	 * unevenly spaced, so picking the nearest one outright is simpler and
	 * less error-prone than inverting the formula and rounding.
	 */
	best_y = 0;
	best_z = 0;
	for (y = 0; y < 31; y++) {
		for (z = 0; z < 4; z++) {
			cand = ((1u << y) * (4 + z)) / 4;
			err = (cand > target) ? cand - target : target - cand;
			if (err < best_err) {
				best_err = err;
				best_y = y;
				best_z = z;
			}
		}
	}

	return ((best_y & PKG_LIMIT_TIME_Y_MASK) |
	    ((best_z & PKG_LIMIT_TIME_Z_MASK) <<
	    (PKG_LIMIT_TIME_Z_SHIFT - PKG_LIMIT_TIME_Y_SHIFT)));
}

/*
 * Is this limit acceptable given the configured bounds?
 *
 * Split out from the MSR path so the policy can be tested on its own.
 */
int
rapl_check_bounds(uint32_t watts, uint32_t min_allowed, uint32_t max_allowed)
{
	if (watts < min_allowed)
		return (EINVAL);
	if (max_allowed != 0 && watts > max_allowed)
		return (EINVAL);
	return (0);
}

