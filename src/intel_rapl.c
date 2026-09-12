/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Rafal Rabczuk
 *
 * Intel RAPL (Running Average Power Limit) for FreeBSD.
 *
 * Why this exists
 * ---------------
 * FreeBSD can change CPU frequency (cpufreq/powerd), but it has no way to
 * tell the processor how much power it may draw.  On a laptop whose cooling
 * has degraded, frequency capping is a blunt instrument: it limits the top
 * of the range regardless of how much thermal headroom there actually is.
 * RAPL is the mechanism Intel provides for exactly this problem - the
 * hardware keeps itself inside a power envelope and manages the frequency
 * on its own.
 *
 * Scope
 * -----
 * Package domain only, and only the parts needed to cap sustained power and
 * observe the result:
 *
 *   - read the scaling units and the package TDP
 *   - read and set PL1 (sustained) and PL2 (burst) limits
 *   - report actual package power, derived from the energy counter
 *
 * Deliberately not implemented: the DRAM, PP0 and PP1 domains, energy
 * accounting per core, and the PCI-mapped DPTF interface.  None of them are
 * needed to keep a laptop from cooking itself, and each would add hardware
 * variation to test.
 *
 * This driver is a plain module rather than a newbus device.  RAPL is a CPU
 * feature reached through MSRs, not a device on a bus; attaching to the
 * Processor Thermal Subsystem PCI function would add a dependency without
 * adding capability.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <machine/cpufunc.h>
#include <machine/cputypes.h>	/* CPU_VENDOR_INTEL */
#include <machine/md_var.h>
#include <machine/specialreg.h>

#include "intel_rapl.h"

static struct rapl_softc sc;
static struct sysctl_ctx_list rapl_sysctl_ctx;
static struct sysctl_oid *rapl_sysctl_tree;

/*
 * Convert between watts and the hardware's power units.
 *
 * All arithmetic is integer: the kernel has no floating point, and the
 * hardware granularity (typically 0.125 W) is finer than anything a user
 * would want to express anyway.
 */
static inline uint32_t
rapl_watts_to_units(uint32_t watts)
{
	return (watts << sc.power_unit_shift);
}

static inline uint32_t
rapl_units_to_watts(uint32_t units)
{
	return (units >> sc.power_unit_shift);
}

/*
 * Read the current package power limit register.
 */
static int
rapl_read_limit(uint64_t *val)
{
	return (rdmsr_safe(MSR_PKG_POWER_LIMIT, val));
}

/*
 * Replace one power limit field, leaving everything else in the register
 * untouched.  Read-modify-write matters here: the register also carries the
 * time window, the enable and clamp bits and the lock, and clobbering any of
 * them would change behaviour in ways the caller did not ask for.
 */
static int
rapl_set_limit(bool pl2, uint32_t watts)
{
	uint64_t reg, field;
	int shift, error;

	if (sc.locked)
		return (EPERM);
	error = rapl_check_bounds(watts, sc.min_allowed, sc.max_allowed);
	if (error != 0)
		return (error);

	error = rapl_read_limit(&reg);
	if (error != 0)
		return (error);

	shift = pl2 ? PKG_LIMIT_PL2_SHIFT : 0;
	field = (uint64_t)rapl_watts_to_units(watts) & PKG_LIMIT_POWER_MASK;

	reg &= ~(PKG_LIMIT_POWER_MASK << shift);
	reg |= field << shift;
	/* a limit nobody enforces is not a limit */
	reg |= PKG_LIMIT_ENABLE << shift;

	error = wrmsr_safe(MSR_PKG_POWER_LIMIT, reg);
	if (error != 0)
		return (error);

	/*
	 * Verify rather than trust.  When the register is locked the write is
	 * dropped silently, and on some firmware the lock bit is not set even
	 * though writes do not stick.
	 */
	error = rapl_read_limit(&reg);
	if (error != 0)
		return (error);
	if (((reg >> shift) & PKG_LIMIT_POWER_MASK) != field)
		return (EPERM);

	return (0);
}

static int
rapl_get_limit(bool pl2, uint32_t *watts)
{
	uint64_t reg;
	int error, shift;

	error = rapl_read_limit(&reg);
	if (error != 0)
		return (error);

	shift = pl2 ? PKG_LIMIT_PL2_SHIFT : 0;
	*watts = rapl_units_to_watts((reg >> shift) & PKG_LIMIT_POWER_MASK);
	return (0);
}

/*
 * Average package power since the previous call, in milliwatts.
 *
 * MSR_PKG_ENERGY_STATUS is a free-running 32-bit counter of consumed energy
 * units.  Power is its rate of change, so a single reading says nothing -
 * two readings and the interval between them are needed.  The counter wraps
 * roughly every 60 seconds at full load, which unsigned 32-bit subtraction
 * handles correctly for a single wrap.
 */
static int
rapl_power_mw(uint32_t *mw)
{
	uint64_t raw;
	uint32_t delta_units;
	sbintime_t now, elapsed;
	int error;

	error = rdmsr_safe(MSR_PKG_ENERGY_STATUS, &raw);
	if (error != 0)
		return (error);

	mtx_lock(&sc.sample_lock);
	now = sbinuptime();
	elapsed = now - sc.last_sample;

	/*
	 * Return the previous result when the samples are too close together
	 * rather than recomputing from a near-zero interval.
	 *
	 * This is not merely a guard against a busy caller: sysctl(9) invokes
	 * a handler twice for a single userland read, once to size the buffer
	 * and once for the value.  Updating the baseline on the first call
	 * left the second one dividing by an interval of almost nothing, so
	 * every read returned zero.
	 */
	if (elapsed < SBT_1MS * 200) {
		*mw = sc.last_mw;
		mtx_unlock(&sc.sample_lock);
		return (0);
	}

	delta_units = (uint32_t)raw - (uint32_t)sc.last_energy_raw;
	sc.last_energy_raw = raw;
	sc.last_sample = now;

	/*
	 * energy_mJ = delta_units * 1000 / 2^energy_unit_shift
	 * power_mW  = energy_mJ * 1000 / elapsed_ms
	 *
	 * Folded into one expression and ordered to keep the intermediate
	 * inside 64 bits even at high counts.
	 */
	sc.last_mw = (uint32_t)(((uint64_t)delta_units * 1000000ULL >>
	    sc.energy_unit_shift) / (uint64_t)(elapsed / SBT_1MS));
	*mw = sc.last_mw;
	mtx_unlock(&sc.sample_lock);

	return (0);
}

static int
sysctl_rapl_pl(SYSCTL_HANDLER_ARGS)
{
	uint32_t watts;
	bool pl2 = (arg2 != 0);
	int error;

	error = rapl_get_limit(pl2, &watts);
	if (error != 0)
		return (error);

	error = sysctl_handle_int(oidp, &watts, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);

	return (rapl_set_limit(pl2, watts));
}

static int
sysctl_rapl_window(SYSCTL_HANDLER_ARGS)
{
	uint64_t reg;
	uint32_t sec, field;
	bool pl2 = (arg2 != 0);
	int shift, error;

	shift = pl2 ? PKG_LIMIT_PL2_SHIFT : 0;

	error = rapl_read_limit(&reg);
	if (error != 0)
		return (error);

	field = (uint32_t)((reg >> (shift + PKG_LIMIT_TIME_Y_SHIFT)) &
	    PKG_LIMIT_TIME_MASK);
	sec = rapl_window_decode(field, sc.time_unit_shift);

	error = sysctl_handle_int(oidp, &sec, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);

	if (sc.locked)
		return (EPERM);

	field = rapl_window_encode(sec, sc.time_unit_shift);
	reg &= ~(PKG_LIMIT_TIME_MASK << (shift + PKG_LIMIT_TIME_Y_SHIFT));
	reg |= (uint64_t)field << (shift + PKG_LIMIT_TIME_Y_SHIFT);

	error = wrmsr_safe(MSR_PKG_POWER_LIMIT, reg);
	if (error != 0)
		return (error);

	/* same reason as for the limit itself: a locked register lies */
	error = rapl_read_limit(&reg);
	if (error != 0)
		return (error);
	if (((reg >> (shift + PKG_LIMIT_TIME_Y_SHIFT)) & PKG_LIMIT_TIME_MASK)
	    != field)
		return (EPERM);

	return (0);
}

static int
sysctl_rapl_power(SYSCTL_HANDLER_ARGS)
{
	uint32_t mw;
	int error;

	error = rapl_power_mw(&mw);
	if (error != 0)
		return (error);
	return (sysctl_handle_int(oidp, &mw, 0, req));
}

/*
 * RAPL is present on Intel family 6 from Sandy Bridge onwards.  Rather than
 * carry a model table that would need updating for every new CPU, probe the
 * hardware: if the unit register reads back something sane, the feature is
 * there.
 */
static int
rapl_probe(void)
{
	uint64_t units;

	if (cpu_vendor_id != CPU_VENDOR_INTEL)
		return (ENXIO);
	if (rdmsr_safe(MSR_RAPL_POWER_UNIT, &units) != 0)
		return (ENXIO);
	if (units == 0 || units == ~0ULL)
		return (ENXIO);

	return (0);
}

static int
rapl_init(void)
{
	uint64_t units, info, limit;
	int error;

	error = rapl_probe();
	if (error != 0)
		return (error);

	(void)rdmsr_safe(MSR_RAPL_POWER_UNIT, &units);
	sc.power_unit_shift = (units >> RAPL_UNIT_POWER_SHIFT) &
	    RAPL_UNIT_POWER_MASK;
	sc.energy_unit_shift = (units >> RAPL_UNIT_ENERGY_SHIFT) &
	    RAPL_UNIT_ENERGY_MASK;
	sc.time_unit_shift = (units >> RAPL_UNIT_TIME_SHIFT) &
	    RAPL_UNIT_TIME_MASK;

	if (rdmsr_safe(MSR_PKG_POWER_INFO, &info) == 0) {
		sc.tdp_watts = rapl_units_to_watts(info & PKG_INFO_TDP_MASK);
		sc.hw_min_watts = rapl_units_to_watts(
		    (info >> PKG_INFO_MIN_SHIFT) & PKG_INFO_TDP_MASK);
		sc.hw_max_watts = rapl_units_to_watts(
		    (info >> PKG_INFO_MAX_SHIFT) & PKG_INFO_TDP_MASK);
	}

	/*
	 * Seed the enforced bounds from the hardware where it reports them,
	 * and fall back where it does not.  Leaving max at zero would mean no
	 * ceiling at all, which is exactly the case on hardware that fills in
	 * only the TDP field.
	 */
	sc.min_allowed = sc.hw_min_watts != 0 ? sc.hw_min_watts :
	    RAPL_DEFAULT_MIN_WATTS;
	sc.max_allowed = sc.hw_max_watts != 0 ? sc.hw_max_watts :
	    sc.tdp_watts * RAPL_DEFAULT_MAX_FACTOR;

	if (rapl_read_limit(&limit) == 0)
		sc.locked = (limit & PKG_LIMIT_LOCKED) != 0;

	mtx_init(&sc.sample_lock, "intel_rapl sample", NULL, MTX_DEF);
	/*
	 * Establish the baseline now rather than on the first read, so the
	 * first reading a user takes covers the interval since load instead
	 * of since boot - the energy counter starts at power-on.
	 */
	(void)rdmsr_safe(MSR_PKG_ENERGY_STATUS, &sc.last_energy_raw);
	sc.last_sample = sbinuptime();
	sc.last_mw = 0;

	sysctl_ctx_init(&rapl_sysctl_ctx);
	rapl_sysctl_tree = SYSCTL_ADD_NODE(&rapl_sysctl_ctx,
	    SYSCTL_STATIC_CHILDREN(_hw), OID_AUTO, "intel_rapl",
	    CTLFLAG_RD, 0, "Intel RAPL package power limits");

	SYSCTL_ADD_UINT(&rapl_sysctl_ctx, SYSCTL_CHILDREN(rapl_sysctl_tree),
	    OID_AUTO, "tdp_watts", CTLFLAG_RD, &sc.tdp_watts, 0,
	    "Package thermal design power (W)");
	SYSCTL_ADD_UINT(&rapl_sysctl_ctx, SYSCTL_CHILDREN(rapl_sysctl_tree),
	    OID_AUTO, "hw_max_watts", CTLFLAG_RD, &sc.hw_max_watts, 0,
	    "Ceiling reported by the hardware (W), 0 when not reported");
	SYSCTL_ADD_UINT(&rapl_sysctl_ctx, SYSCTL_CHILDREN(rapl_sysctl_tree),
	    OID_AUTO, "min_allowed_watts", CTLFLAG_RW, &sc.min_allowed, 0,
	    "Lowest limit this driver will accept (W)");
	SYSCTL_ADD_UINT(&rapl_sysctl_ctx, SYSCTL_CHILDREN(rapl_sysctl_tree),
	    OID_AUTO, "max_allowed_watts", CTLFLAG_RW, &sc.max_allowed, 0,
	    "Highest limit this driver will accept (W), 0 disables the check");
	SYSCTL_ADD_BOOL(&rapl_sysctl_ctx, SYSCTL_CHILDREN(rapl_sysctl_tree),
	    OID_AUTO, "locked", CTLFLAG_RD, &sc.locked, 0,
	    "Firmware locked the limit register; writes will fail");

	SYSCTL_ADD_PROC(&rapl_sysctl_ctx, SYSCTL_CHILDREN(rapl_sysctl_tree),
	    OID_AUTO, "pl1_watts", CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE,
	    NULL, 0, sysctl_rapl_pl, "IU",
	    "Sustained package power limit (W)");
	SYSCTL_ADD_PROC(&rapl_sysctl_ctx, SYSCTL_CHILDREN(rapl_sysctl_tree),
	    OID_AUTO, "pl2_watts", CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE,
	    NULL, 1, sysctl_rapl_pl, "IU",
	    "Short-term burst power limit (W)");
	SYSCTL_ADD_PROC(&rapl_sysctl_ctx, SYSCTL_CHILDREN(rapl_sysctl_tree),
	    OID_AUTO, "pl1_window_sec",
	    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE,
	    NULL, 0, sysctl_rapl_window, "IU",
	    "Interval the sustained limit is averaged over (s)");
	SYSCTL_ADD_PROC(&rapl_sysctl_ctx, SYSCTL_CHILDREN(rapl_sysctl_tree),
	    OID_AUTO, "pl2_window_sec",
	    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE,
	    NULL, 1, sysctl_rapl_window, "IU",
	    "Interval the burst limit is averaged over (s)");

	SYSCTL_ADD_PROC(&rapl_sysctl_ctx, SYSCTL_CHILDREN(rapl_sysctl_tree),
	    OID_AUTO, "power_mw", CTLTYPE_UINT | CTLFLAG_RD | CTLFLAG_MPSAFE,
	    NULL, 0, sysctl_rapl_power, "IU",
	    "Average package power since previous read (mW)");

	printf("intel_rapl: package TDP %u W, accepting %u-%u W%s\n",
	    sc.tdp_watts, sc.min_allowed, sc.max_allowed,
	    sc.locked ? ", register locked by firmware" : "");

	return (0);
}

static void
rapl_uninit(void)
{
	sysctl_ctx_free(&rapl_sysctl_ctx);
	mtx_destroy(&sc.sample_lock);
}

static int
rapl_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		return (rapl_init());
	case MOD_UNLOAD:
		rapl_uninit();
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t intel_rapl_mod = {
	"intel_rapl",
	rapl_modevent,
	NULL
};

DECLARE_MODULE(intel_rapl, intel_rapl_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(intel_rapl, 1);
