# Makefile for the intel_rapl kernel module
#
# SPDX-License-Identifier: BSD-2-Clause
#
#   make          build the module
#   make clean    remove build artifacts
#   make load     load it (root)
#   make unload   unload it (root)
#   make install  install into /boot/modules

KMOD=	intel_rapl
SRCS=	intel_rapl.c

.if !exists(${.CURDIR}/../VERSION)
.error VERSION file missing - cannot set RAPL_DRV_VERSION
.endif
RAPL_VERSION!=	cat ${.CURDIR}/../VERSION
CFLAGS+=	-DRAPL_DRV_VERSION=\"${RAPL_VERSION}\"

# The kernel is built with these; matching them keeps warnings honest.
CWARNFLAGS+=	-Wall -Wstrict-prototypes -Wmissing-prototypes

.include <bsd.kmod.mk>
