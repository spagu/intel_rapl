# Convenience wrapper - the real build lives in src/
#
#   make build    compile the module
#   make install  install into /boot/modules
#   make load     load into the running kernel
#   make status   show what the driver reports

SUBDIR=	src

build:
	@${MAKE} -C src

clean:
	@${MAKE} -C src clean

install:
	@${MAKE} -C src install

load:
	@${MAKE} -C src load

unload:
	@${MAKE} -C src unload

status:
	@sysctl hw.intel_rapl 2>/dev/null || echo "intel_rapl nie jest zaladowany"

.PHONY: build clean install load unload status
