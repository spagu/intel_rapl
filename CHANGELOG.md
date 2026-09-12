# Changelog

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
this project follows [Semantic Versioning](https://semver.org/).

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

### Known limitations

- Package domain only. DRAM, PP0 and PP1 are not implemented.
- On hardware where the firmware locks `MSR_PKG_POWER_LIMIT`, limits can be
  read but not changed. This is a firmware decision, not a driver one; the
  `locked` sysctl reports it.
