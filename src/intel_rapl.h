/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Rafal Rabczuk
 *
 * Intel RAPL (Running Average Power Limit) for FreeBSD.
 */

#ifndef _INTEL_RAPL_H_
#define _INTEL_RAPL_H_

/*
 * RAPL model-specific registers.
 *
 * These are documented in the Intel SDM, Volume 4 ("Model-Specific
 * Registers"), in the section on platform-specific power management.
 * They exist on Sandy Bridge and later; this driver targets the package
 * domain only, which is the one that governs the whole CPU package and
 * is therefore the one that matters for a thermally limited laptop.
 */
#define	MSR_RAPL_POWER_UNIT	0x606	/* scaling for power/energy/time */
#define	MSR_PKG_POWER_LIMIT	0x610	/* PL1 and PL2, read/write */
#define	MSR_PKG_ENERGY_STATUS	0x611	/* monotonic energy counter */
#define	MSR_PKG_POWER_INFO	0x614	/* TDP and min/max limits, read-only */

/*
 * MSR_RAPL_POWER_UNIT layout.  Each field is a power-of-two divisor, not
 * a value: power unit 3 means one step is 1/2^3 = 0.125 W.
 */
#define	RAPL_UNIT_POWER_SHIFT	0
#define	RAPL_UNIT_POWER_MASK	0x0f
#define	RAPL_UNIT_ENERGY_SHIFT	8
#define	RAPL_UNIT_ENERGY_MASK	0x1f
#define	RAPL_UNIT_TIME_SHIFT	16
#define	RAPL_UNIT_TIME_MASK	0x0f

/*
 * MSR_PKG_POWER_LIMIT layout.
 *
 * PL1 lives in the low 32 bits, PL2 in the high 32.  Bit 63 is a lock:
 * once the firmware sets it, the register is read-only until the next
 * reset and any write is silently dropped.  We check it rather than
 * pretending a write succeeded.
 */
#define	PKG_LIMIT_POWER_MASK	0x7fffULL	/* bits 14:0  */
#define	PKG_LIMIT_ENABLE	(1ULL << 15)
#define	PKG_LIMIT_CLAMP		(1ULL << 16)
#define	PKG_LIMIT_TIME_SHIFT	17		/* bits 23:17 */
#define	PKG_LIMIT_TIME_MASK	0x7fULL
#define	PKG_LIMIT_PL2_SHIFT	32
#define	PKG_LIMIT_LOCKED	(1ULL << 63)

/*
 * MSR_PKG_POWER_INFO layout: thermal design power and the range the
 * hardware is willing to accept, all in power units.
 */
#define	PKG_INFO_TDP_MASK	0x7fffULL	/* bits 14:0  */
#define	PKG_INFO_MIN_SHIFT	16		/* bits 30:16 */
#define	PKG_INFO_MAX_SHIFT	32		/* bits 46:32 */

/*
 * Default guard rails for what a limit may be set to.
 *
 * A package starved into the low single-digit watts looks broken rather than
 * slow, and undoing it needs a shell responsive enough to type into.  The
 * ceiling exists because MSR_PKG_POWER_INFO does not always report one: on
 * the XPS 13 9343 only the TDP field is populated and the min/max fields read
 * as zero, so trusting the hardware alone would leave no upper bound at all.
 *
 * Both are starting values only - they are exposed as writable sysctls so an
 * operator who knows their hardware can widen or narrow them.
 */
#define	RAPL_DEFAULT_MIN_WATTS	5
#define	RAPL_DEFAULT_MAX_FACTOR	2	/* fallback ceiling: TDP x this */

/*
 * Time window encoding for MSR_PKG_POWER_LIMIT, bits 23:17.
 *
 *	window = 2^Y * (1 + Z/4) * time_unit
 *
 * with Y in bits 21:17 and Z in bits 23:22.  This is the interval the
 * sustained limit is averaged over, and it matters as much as the limit
 * itself: the same PL1 with a 1 s window and a 30 s window produce very
 * different thermal behaviour.
 */
#define	PKG_LIMIT_TIME_Y_SHIFT	17
#define	PKG_LIMIT_TIME_Y_MASK	0x1fULL
#define	PKG_LIMIT_TIME_Z_SHIFT	22
#define	PKG_LIMIT_TIME_Z_MASK	0x03ULL
#define	RAPL_MAX_WINDOW_SEC	128

#ifdef _KERNEL
struct rapl_softc {
	uint32_t	power_unit_shift;
	uint32_t	energy_unit_shift;
	uint32_t	time_unit_shift;

	uint32_t	tdp_watts;
	uint32_t	hw_min_watts;	/* as reported, 0 when absent */
	uint32_t	hw_max_watts;

	/* enforced bounds - writable, seeded from the hardware */
	uint32_t	min_allowed;
	uint32_t	max_allowed;

	bool		locked;

	/* for computing average power from the energy counter */
	uint64_t	last_energy_raw;
	sbintime_t	last_sample;
	uint32_t	last_mw;
	struct mtx	sample_lock;
};
#endif /* _KERNEL */

/*
 * Pure helpers, shared with the unit tests.  These touch no MSRs and no
 * softc, so they can be compiled and exercised in userland.
 */
uint32_t rapl_window_decode(uint32_t field, uint32_t time_shift);
uint32_t rapl_window_encode(uint32_t seconds, uint32_t time_shift);
int	 rapl_check_bounds(uint32_t watts, uint32_t min_allowed,
	    uint32_t max_allowed);

#endif /* _INTEL_RAPL_H_ */
