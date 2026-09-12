# intel_rapl

[![FreeBSD](https://img.shields.io/badge/FreeBSD-15.1-AB2B28?style=for-the-badge&logo=freebsd&logoColor=white)](https://www.freebsd.org/)
[![Licence](https://img.shields.io/badge/Licence-BSD--2--Clause-4C8EDA?style=for-the-badge)](LICENSE)
[![Version](https://img.shields.io/badge/Version-0.1.0-2E7D32?style=for-the-badge)](CHANGELOG.md)

Read and set the Intel CPU package power envelope on FreeBSD.

## Why

FreeBSD can change CPU frequency through `cpufreq` and `powerd`, but it has no
interface to the processor's power budget. On a thermally healthy machine that
does not matter. On a laptop whose heatsink is clogged or whose thermal paste
has dried out, it matters a great deal: frequency capping limits the top of the
range whether or not there is headroom, while RAPL lets the processor manage
its own frequency inside a power envelope you choose.

It is also the only way on FreeBSD to answer a simple question — *how many
watts is this CPU actually drawing right now* — which turns out to be the
fastest way to tell a cooling problem from a configuration one.

## What it measures, on real hardware

From a Dell Latitude E7450, Core i7-5600U, 15 W TDP, with a heatsink overdue
for cleaning:

```
idle              3.5 - 6.4 W      52 C
full load        17.9 - 20.8 W     97-100 C, throttled to 500 MHz
after stopping         2.2 W
```

Sustained draw of 18-21 W against a 15 W design point, which is what a
degraded thermal path looks like from the software side. Without this driver
the same machine only reports "hot and slow".

## Scope

Package domain only, and within it only what is needed to cap sustained power
and observe the result. DRAM, PP0 and PP1 domains, per-core energy accounting
and the PCI-mapped DPTF interface are deliberately left out: none of them help
keep a laptop from cooking itself, and each adds hardware variation to test.

The driver is a plain kernel module rather than a newbus device. RAPL is a CPU
feature reached through MSRs, so attaching to the Processor Thermal Subsystem
PCI function would add a dependency without adding capability.

## Requirements

- FreeBSD 13 or later, amd64
- Intel CPU, Sandy Bridge or newer
- kernel sources in `/usr/src` to build the module

## Build and load

```sh
make build
sudo make install      # into /boot/modules
sudo make load
make status
```

To load at boot, add to `/etc/rc.conf`:

```sh
kld_list="intel_rapl"
```

`/boot/loader.conf` works too, but `kld_list` is preferred: it runs after the
filesystems are mounted, which avoids a class of early-boot surprises.

## Use

```sh
# what the hardware reports
sysctl hw.intel_rapl

hw.intel_rapl.tdp_watts: 15
hw.intel_rapl.max_watts: 0
hw.intel_rapl.locked: 1
hw.intel_rapl.pl1_watts: 15
hw.intel_rapl.pl2_watts: 25
hw.intel_rapl.power_mw: 18761
```

| sysctl | meaning |
|---|---|
| `tdp_watts` | package thermal design power, read-only |
| `max_watts` | highest limit the hardware advertises; `0` when firmware leaves the field empty |
| `locked` | firmware sealed the limit register — writes will fail |
| `pl1_watts` | sustained power limit, read/write |
| `pl2_watts` | short-term burst limit, read/write |
| `power_mw` | actual package power since the previous read |

Lowering the sustained limit:

```sh
sysctl hw.intel_rapl.pl1_watts=10
```

`power_mw` is a rate, not a total: it covers the interval since the previous
read of that same sysctl. Read it twice a few seconds apart to get a figure
that means anything. Reads closer together than 200 ms return the previous
result rather than dividing by a near-zero interval.

## When writes fail

```
sysctl: hw.intel_rapl.pl1_watts=10: Operation not permitted
```

Many vendors lock `MSR_PKG_POWER_LIMIT` in firmware, after which the register
is read-only until the next reset. The Latitude above does exactly this, so on
that machine the driver can measure but not intervene.

Writes are always verified by reading the register back, because a locked
register accepts a write and silently discards it. Reporting `EPERM` is more
useful than reporting a success that did not happen.

There is no way around the lock from the OS. If the firmware offers a
configurable TDP setting, that is where to change it.

## Safety

- limits below 5 W are refused: a package starved that far looks broken rather
  than slow, and undoing it needs a working shell
- limits above what the hardware advertises are refused
- the limit register is updated read-modify-write, so the time window, enable,
  clamp and lock bits are preserved

## Licence

BSD-2-Clause. See [LICENSE](LICENSE).
