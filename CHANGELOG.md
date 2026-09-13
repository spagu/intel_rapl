# Changelog

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
this project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed

- **Documented `loader.conf` as the way to load the driver at boot**, and said
  why. `rcorder` runs `/etc/rc.d/sysctl` first and `/etc/rc.d/kld` twenty-five
  places later, so a `hw.intel_rapl` setting placed in `sysctl.conf` is applied
  before a module loaded from `kld_list` exists and fails with `unknown oid`.
  The README and `intel_rapl(4)` previously recommended `kld_list` without
  qualification. Both now also warn that `kld_list` must be appended to rather
  than replaced, show how to confirm the module loaded, and explain that
  `register locked by firmware` means there is nothing to persist beyond the
  module itself.
- `SEE ALSO` in `intel_rapl(4)` lists the configuration files the driver is
  set up through, in the order `mandoc -T lint` expects.

## [0.2.0] - 2026-09-12

### Added

- `hw.intel_rapl.pl1_window_sec` and `pl2_window_sec` - the interval each
  limit is averaged over, read/write. The encoding was already decoded in the
  header but never exposed; without it the limit tells only half the story,
  since the same figure over a 1 s and a 30 s window behave very differently.
- `hw.intel_rapl.min_allowed_watts` and `max_allowed_watts` - the driver's own
  bounds, writable, seeded from the hardware where it reports a range.
- Unit tests for the encodings (`tests/unit`), 23 checks. They compile and run
  in userland against known values decoded by hand from the SDM formula.
- A port skeleton under `ports/intel_rapl-kmod`.

### Fixed

- The ceiling check never ran. It was written as
  `max_watts != 0 && watts > max_watts`, and `MSR_PKG_POWER_INFO` reports zero
  for the min/max fields on the XPS 13 9343 - only the TDP field is populated.
  There was in effect no upper bound at all. The bounds now fall back to twice
  TDP when the hardware declines to say.

### Changed

- Pure arithmetic moved to `src/rapl_calc.c` so it compiles outside the kernel
  and can be tested without hardware. This matters more than it sounds: on
  firmware-locked machines the MSR write path is never reached, so the bounds
  check could not otherwise be exercised at all.

## [0.1.0] - 2026-09-12

First working version. Written because FreeBSD has no way to read or set the
CPU package power envelope, which left frequency capping as the only lever on
a laptop whose cooling had degraded.

### Added

- Package-domain RAPL support through the MSR interface
- `hw.intel_rapl.tdp_watts` - package thermal design power
- `hw.intel_rapl.pl1_watts` - sustained power limit, read/write
- `hw.intel_rapl.pl2_watts` - short-term burst limit, read/write
- `hw.intel_rapl.power_mw` - actual package power, from the energy counter
- `hw.intel_rapl.locked` - whether firmware sealed the limit register
- Writes are verified by reading the register back, so a limit that the
  firmware silently refuses is reported as `EPERM` instead of appearing to
  have been applied

### Fixed during development

- `power_mw` always returned zero. `sysctl(9)` calls a handler twice for a
  single userland read - once to size the buffer, once for the value - and
  the baseline was being advanced on the first call, so the second divided by
  an interval of almost nothing. The sampler now keeps the previous result
  and only recomputes when at least 200 ms have passed.

### Tested on

Dell XPS 13 9343, Core i7-5600U (Broadwell-U), 15 W package TDP, BIOS A20,
FreeBSD 15.1-RELEASE-p3 amd64. The limit register is locked by firmware on
this machine, so the write path has been exercised only against a refusal.

### Known limitations

- Package domain only. DRAM, PP0 and PP1 are not implemented.
- On hardware where the firmware locks `MSR_PKG_POWER_LIMIT`, limits can be
  read but not changed. This is a firmware decision, not a driver one; the
  `locked` sysctl reports it.
