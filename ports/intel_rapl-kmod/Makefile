PORTNAME=	intel_rapl-kmod
DISTVERSIONPREFIX=	v
DISTVERSION=	0.2.0
CATEGORIES=	sysutils

MAINTAINER=	please@forget.me.uk
COMMENT=	Intel RAPL package power limits and power measurement
WWW=		https://github.com/spagu/intel_rapl

LICENSE=	BSD2CLAUSE
LICENSE_FILE=	${WRKSRC}/LICENSE

ONLY_FOR_ARCHS=	amd64 i386
ONLY_FOR_ARCHS_REASON=	RAPL is an Intel x86 feature

USES=		kmod uidfix

USE_GITHUB=	yes
GH_ACCOUNT=	spagu
GH_PROJECT=	intel_rapl

# The module lives in src/; the top-level Makefile is a convenience wrapper
# and would recurse into tests/ as well, which needs no installing.
WRKSRC_SUBDIR=	src

PLIST_FILES=	${KMODDIR}/intel_rapl.ko \
		share/man/man4/intel_rapl.4.gz

post-install:
	${INSTALL_MAN} ${WRKSRC}/../man/intel_rapl.4 \
		${STAGEDIR}${PREFIX}/share/man/man4/

# The unit tests build and run in userland - no kernel, no hardware, so they
# are safe to run anywhere the port is built.
do-test:
	@cd ${WRKSRC}/../tests/unit && ${MAKE} run

.include <bsd.port.mk>
